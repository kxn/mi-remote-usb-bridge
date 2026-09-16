/*
 * ico_decoder.c — decode Unicom BLE voice-remote FC frames to WAV.
 *
 * The remote's audio payload ("iFLYTEK ICO", a.k.a. 讯飞16倍压缩) is
 * ITU-T G.722.1 (7 kHz mode, 16 kbit/s = 40 bytes per 20 ms frame) wrapped
 * in a small obfuscation layer:
 *
 *   1) treat the 40 payload bytes as 20 little-endian u16;
 *   2) reorder with ICO_PERM[], XOR each u16 with 0x0416;
 *   3) decode with the standard G.722.1 fixed-point decoder (MSB-first);
 *   4) clear the low 2 bits of each output sample.
 *
 * State notes:
 *   - noise fill (categories 5/6/7) uses the ITU 4-seed LCG, seeds {1,1,1,1};
 *     reset it whenever the remote starts a new recording (voice key press).
 *   - error concealment state: old_coefs[280] + old_mag_shift.
 *
 * Usage:
 *   ico_decoder.exe <groups.jsonl> <outdir>
 *   (reads data/derived/groups.jsonl, writes <outdir>/<label>.wav)
 *
 * Build (after running fetch_reference.py, which creates ./g7221/):
 *   cc -I pj_shim -I . -I g7221/common -I g7221/decode -o ico_decoder \
 *      g7221/common/basic_op.c g7221/common/common.c \
 *      g7221/common/huff_tab.c g7221/common/tables.c \
 *      g7221/decode/coef2sam.c g7221/decode/dct4_s.c \
 *      g7221/decode/decoder.c ico_decoder.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <direct.h>
#define ICO_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ICO_MKDIR(p) mkdir(p, 0777)
#endif

#include "defs.h"
#include "tables.h"
#include "huff_def.h"
#include "count.h"

extern void decoder(Bit_Obj *bitobj, Rand_Obj *randobj, Word16 number_of_regions,
                    Word16 *decoder_mlt_coefs, Word16 *p_mag_shift,
                    Word16 *p_old_mag_shift, Word16 *old_decoder_mlt_coefs,
                    Word16 frame_error_flag);
extern void rmlt_coefs_to_samples(Word16 *coefs, Word16 *old_samples,
                                  Word16 *out_samples, Word16 dct_length,
                                  Word16 mag_shift);

#define ICO_FRAME_BYTES 40
static const unsigned char ICO_PERM[20] = {0, 1, 18, 8, 9, 5, 2, 17, 11, 16, 10,
                                           3, 12, 7, 14, 15, 4, 13, 6, 19};

typedef struct {
    Word16 old_coefs[280];      /* error-concealment history */
    Word16 old_mag_shift;
    Word16 old_samples[160];    /* MLT overlap-add history   */
    Rand_Obj rand;
} IcoState;

static void ico_reset(IcoState *st)
{
    memset(st->old_coefs, 0, sizeof(st->old_coefs));
    memset(st->old_samples, 0, sizeof(st->old_samples));
    st->old_mag_shift = 0;
    st->rand.seed0 = st->rand.seed1 = st->rand.seed2 = st->rand.seed3 = 1;
}

static void ico_decode_frame(IcoState *st, const uint8_t *frame, Word16 *pcm)
{
    Word16 words[20], coefs[320], mag_shift = 0;
    int k;

    for (k = 0; k < 20; k++) {
        Word16 v = (Word16)(frame[ICO_PERM[k] * 2] | (frame[ICO_PERM[k] * 2 + 1] << 8));
        words[k] = v ^ 0x0416;
    }

    Bit_Obj bo;
    bo.code_word_ptr = words;
    bo.code_bit_count = 0;
    bo.current_word = words[0];
    bo.number_of_bits_left = ICO_FRAME_BYTES * 8;
    bo.next_bit = 0;

    decoder(&bo, &st->rand, 14 /* NUMBER_OF_REGIONS, 7 kHz mode */, coefs,
            &mag_shift, &st->old_mag_shift, st->old_coefs, 0);

    rmlt_coefs_to_samples(coefs, st->old_samples, pcm, 320, mag_shift);

    for (k = 0; k < 320; k++)
        pcm[k] &= (Word16)~3;
}

/* ---------------- minimal WAV writer ---------------- */
#ifndef ICO_LIBRARY
static void write_wav(const char *path, const int16_t *samples, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t rate = 16000;
    uint32_t data_bytes = (uint32_t)(n * 2);
    uint32_t riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt16 = 16; uint16_t pcm16 = 1, ch = 1;
    fwrite(&fmt16, 4, 1, f); fwrite(&pcm16, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    uint32_t byte_rate = rate * 2; uint16_t align = 2, bits = 16;
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(samples, 2, n, f);
    fclose(f);
}

/* ---------------- JSONL helpers ---------------- */
/* groups.jsonl rows look like:
 *   {"sequence": 1, "label": "fb_single_01_00", ...,
 *    "joined_content_hex": "<96 hex chars = 48-byte group>", ...}
 * The 48-byte group is [40B ICO frame][u16 energy][u16 seq][4B tail repeat],
 * so the ICO frame is the first 40 bytes of the hex string. */
static const char *json_str(const char *line, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    const char *p = strstr(line, pat);
    if (!p) return NULL;
    return p + strlen(pat);
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            ICO_MKDIR(tmp);
            *p = c;
        }
    }
    ICO_MKDIR(tmp);
}

static void flush_wav(const char *outdir, const char *label,
                      const int16_t *buf, size_t len)
{
    char path[1024];
    if (!len) return;
    snprintf(path, sizeof(path), "%s/%s.wav", outdir, label);
    write_wav(path, buf, len);
    printf("%s: %zu samples (%.0f ms)\n", path, len, len / 16.0);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <groups.jsonl> <outdir>\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    mkdirs(argv[2]);   /* best effort; fopen() below reports real errors */

    char line[8192];
    char cur_label[128] = "";
    int have_label = 0;
    int16_t *buf = NULL;
    size_t buf_len = 0, buf_cap = 0;
    IcoState st;
    ico_reset(&st);

    while (fgets(line, sizeof(line), in)) {
        const char *lp = json_str(line, "label");
        const char *hp = json_str(line, "joined_content_hex");
        if (!lp || !hp) continue;
        size_t llen = (size_t)(strchr(lp, '"') - lp);
        if (!have_label || strlen(cur_label) != llen ||
            memcmp(lp, cur_label, llen) != 0) {
            flush_wav(argv[2], cur_label, buf, buf_len);
            if (llen >= sizeof(cur_label)) llen = sizeof(cur_label) - 1;
            memcpy(cur_label, lp, llen);
            cur_label[llen] = '\0';
            have_label = 1;
            buf_len = 0;
            ico_reset(&st);   /* new recording: codec state restarts */
        }
        uint8_t frame[ICO_FRAME_BYTES];
        for (int i = 0; i < ICO_FRAME_BYTES; i++) {
            unsigned v;
            if (sscanf(hp + 2 * i, "%2x", &v) != 1) { fprintf(stderr, "bad hex\n"); return 1; }
            frame[i] = (uint8_t)v;
        }
        int16_t pcm[320];
        ico_decode_frame(&st, frame, pcm);
        if (buf_len + 320 > buf_cap) {
            buf_cap = buf_cap ? buf_cap * 2 : 320 * 512;
            buf = realloc(buf, buf_cap * sizeof(int16_t));
            if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }
        }
        memcpy(buf + buf_len, pcm, 320 * sizeof(int16_t));
        buf_len += 320;
    }
    flush_wav(argv[2], cur_label, buf, buf_len);
    fclose(in);
    free(buf);
    return 0;
}
#endif

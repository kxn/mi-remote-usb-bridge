/* Golden vector generator for RBP/2.0.
 *
 * Emits protocol/vectors/golden.json.  The Python client implementation and
 * the C host tests both verify against this file; regenerate only when the
 * protocol version changes.  Deterministic output (fixed test seed data).
 */
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static int first = 1;

static void hexout(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) fprintf(out, "%02x", d[i]);
}

static void vec_begin(const char *name, const char *desc)
{
    if (!first) fprintf(out, ",\n");
    first = 0;
    fprintf(out, "  {\"name\": \"%s\", \"desc\": \"%s\", \"data\": \"", name, desc);
}

static void vec_end(void) { fprintf(out, "\"}"); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "protocol/vectors/golden.json";
    out = fopen(path, "wb");
    if (!out) { perror(path); return 1; }
    fprintf(out, "[\n");

    /* --- crc32c --- */
    {
        struct { const char *in; uint32_t crc; } cases[] = {
            { "", 0x00000000u },
            { "123456789", 0xE3069283u },
            { "\x00\x00\x00\x00", 0x7E32BB50u }, /* filled below by compute, kept for doc */
            { "\xff", 0xA42D65AEu },
        };
        (void)cases;
        /* compute with the reference implementation (constant strings) */
        const char *ins[] = { "", "123456789", "The quick brown fox jumps over the lazy dog" };
        for (int i = 0; i < 3; i++) {
            uint8_t buf[64];
            size_t n = strlen(ins[i]);
            memcpy(buf, ins[i], n);
            char name[64];
            snprintf(name, sizeof(name), "crc32c_%d", i);
            vec_begin(name, ins[i][0] ? ins[i] : "empty");
            uint8_t b4[4] = {
                (uint8_t)(rbp_crc32c(buf, n)),
                (uint8_t)(rbp_crc32c(buf, n) >> 8),
                (uint8_t)(rbp_crc32c(buf, n) >> 16),
                (uint8_t)(rbp_crc32c(buf, n) >> 24) };
            hexout(b4, 4);
            vec_end();
        }
    }

    /* --- cobs round trips --- */
    {
        uint8_t inputs[6][300];
        size_t lens[6];
        memset(inputs, 0, sizeof(inputs));
        /* 0: simple with embedded zero */
        inputs[0][0] = 0x01; inputs[0][1] = 0x00; inputs[0][2] = 0x02; lens[0] = 3;
        /* 1: no zeros */
        memcpy(inputs[1], "ABC", 3); lens[1] = 3;
        /* 2: empty */
        lens[2] = 0;
        /* 3: 254 non-zero then a zero */
        memset(inputs[3], 0xAA, 254); inputs[3][254] = 0; lens[3] = 255;
        /* 4: 255 non-zero */
        memset(inputs[4], 0x55, 255); lens[4] = 255;
        /* 5: zeros only */
        lens[5] = 4;

        for (int i = 0; i < 6; i++) {
            uint8_t enc[400];
            uint8_t dec[400];
            size_t enc_len = rbp_cobs_encode(inputs[i], lens[i], enc);
            size_t dec_len = rbp_cobs_decode(enc, enc_len, dec);
            if (dec_len != lens[i] || memcmp(dec, inputs[i], lens[i]) != 0) {
                fprintf(stderr, "COBS self-test %d failed\n", i);
                return 2;
            }
            char name[64];
            snprintf(name, sizeof(name), "cobs_%d", i);
            vec_begin(name, "cobs encode of data");
            hexout(enc, enc_len);
            vec_end();
        }
    }

    /* --- a full HELLO request frame --- */
    {
        rbp_header_t h;
        memset(&h, 0, sizeof(h));
        h.kind = RBP_KIND_REQUEST;
        h.session_id = 0;
        h.request_id = 1;
        h.opcode = RBP_OP_HELLO;
        h.connection_id = 0;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        uint8_t nonce[16];
        for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(0xA0 + i);
        rbp_tlv_put_bytes(&w, 1, nonce, 16);
        rbp_txseq_t seq;
        rbp_txseq_init(&seq);
        uint8_t frame[600];
        size_t n = rbp_frame_encode(&h, &seq, w.buf, w.len, frame);
        vec_begin("hello_request_frame", "COBS framed HELLO request incl. delimiter");
        hexout(frame, n);
        vec_end();
    }

    /* --- HELLO response frame with all fixed TLVs --- */
    {
        rbp_header_t h;
        memset(&h, 0, sizeof(h));
        h.kind = RBP_KIND_RESPONSE;
        h.session_id = 0x01020304;
        h.tx_seq = 1;
        h.request_id = 1;
        h.opcode = RBP_OP_HELLO;
        h.status = RBP_STATUS_OK;
        h.connection_id = 0;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        uint8_t echo[16], uid[16];
        for (int i = 0; i < 16; i++) { echo[i] = (uint8_t)(0xA0 + i); uid[i] = (uint8_t)(i * 0x11); }
        rbp_tlv_put_bytes(&w, 1, echo, 16);
        rbp_tlv_put_bytes(&w, 2, uid, 16);
        rbp_tlv_put_text(&w, 3, "bridge sim 1.0.0");
        rbp_tlv_put_u8(&w, 4, 64);
        rbp_tlv_put_u16(&w, 5, 512);
        rbp_tlv_put_u32(&w, 6, 0x0F);
        rbp_txseq_t seq;
        rbp_txseq_init(&seq);
        uint8_t frame[600];
        size_t n = rbp_frame_encode(&h, &seq, w.buf, w.len, frame);
        vec_begin("hello_response_frame", "COBS framed HELLO response incl. delimiter");
        hexout(frame, n);
        vec_end();
    }

    /* --- KEYS_STATE 24B struct (kind=physical) --- */
    {
        uint8_t ks[RBP_KEYS_STRUCT_SIZE];
        memset(ks, 0, sizeof(ks));
        uint32_t seq = 0x00000007;
        uint64_t us = 0x0000000051322000ULL;
        ks[0] = (uint8_t)seq; ks[1] = (uint8_t)(seq >> 8); ks[2] = (uint8_t)(seq >> 16); ks[3] = (uint8_t)(seq >> 24);
        for (int i = 0; i < 8; i++) ks[4 + i] = (uint8_t)(us >> (8 * i));
        uint64_t bits = (1ULL << 8) | (1ULL << 2); /* slots 8 (Home) and 2 (Down) */
        for (int i = 0; i < 8; i++) ks[12 + i] = (uint8_t)(bits >> (8 * i));
        ks[20] = RBP_KEYS_KIND_PHYSICAL;
        ks[21] = RBP_KEYS_REASON_NONE;
        vec_begin("keys_state_physical", "24B keys struct: Home+Down pressed");
        hexout(ks, sizeof(ks));
        vec_end();
    }

    /* RBP3: one compressed unit producing four samples. */
    {
        uint8_t vd[42]={0};vd[0]=0x12;vd[4]=3;vd[8]=1;vd[12]=3;vd[16]=2;
        uint64_t index=480;for(int i=0;i<8;i++)vd[24+i]=(uint8_t)(index>>(8*i));
        vd[32]=4;vd[40]=0x17;vd[41]=0x80;
        vec_begin("voice_data_payload","RBP3 coding unit header + 2 ADPCM bytes");hexout(vd,sizeof vd);vec_end();
    }

    /* --- voice audio event frame (VOICE_STARTED) --- */
    {
        rbp_header_t h;
        memset(&h, 0, sizeof(h));
        h.kind = RBP_KIND_EVENT;
        h.session_id = 0x01020304;
        h.tx_seq = 9;
        h.opcode = RBP_OP_VOICE_STARTED_EV;
        h.connection_id = 0x000000AB;
        rbp_tlv_writer_t w;
        rbp_tlv_writer_init(&w);
        rbp_tlv_put_u32(&w, 1, 0x12);
        rbp_tlv_put_u32(&w,2,1);rbp_tlv_put_u32(&w,3,1);rbp_tlv_put_u16(&w,4,1);
        rbp_tlv_put_u32(&w,5,16000);rbp_tlv_put_u8(&w,6,1);uint8_t seed[4]={0};
        rbp_tlv_put_bytes(&w,7,seed,4);rbp_tlv_put_u32(&w,8,512);rbp_tlv_put_u64(&w,9,0);
        rbp_tlv_put_u64(&w,10,123456789ULL);rbp_tlv_put_u32(&w,11,1);
        rbp_txseq_t seq;
        rbp_txseq_init(&seq);
        seq.next_tx_seq = 9;
        uint8_t frame[600];
        size_t n = rbp_frame_encode(&h, &seq, w.buf, w.len, frame);
        vec_begin("voice_started_event", "VOICE_STARTED event frame");
        hexout(frame, n);
        vec_end();
    }

    /* --- TLV edge: unknown tag with unknown type must be skippable --- */
    {
        rbp_header_t h;
        memset(&h, 0, sizeof(h));
        h.kind = RBP_KIND_REQUEST;
        h.request_id = 2;
        h.opcode = RBP_OP_PING;
        uint8_t payload[16];
        /* tag=0xF7 type=0x55 len=4 value=deadbeef; then tag=1 u32 cookie=0 */
        payload[0] = 0xF7; payload[1] = 0x55; payload[2] = 4; payload[3] = 0;
        payload[4] = 0xDE; payload[5] = 0xAD; payload[6] = 0xBE; payload[7] = 0xEF;
        payload[8] = 1; payload[9] = 3; payload[10] = 4; payload[11] = 0;
        payload[12] = 0x78; payload[13] = 0x56; payload[14] = 0x34; payload[15] = 0x12;
        rbp_txseq_t seq;
        rbp_txseq_init(&seq);
        uint8_t frame[600];
        size_t n = rbp_frame_encode(&h, &seq, payload, 16, frame);
        vec_begin("ping_unknown_tag_frame", "PING with unknown tag/type to skip");
        hexout(frame, n);
        vec_end();
    }

    fprintf(out, "\n]\n");
    fclose(out);
    printf("wrote %s\n", path);
    return 0;
}

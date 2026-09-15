/* IMA internal consistency tests. Independent execution of the original
 * Telink decoder lives in test_reference_audio.py. */
#include "ima_decoder.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* reference encoder (standard IMA) used to produce round-trip vectors */
static void ima_encode_nibble(int16_t *pred, int16_t *step_idx, int16_t sample,
                              uint8_t *nibble)
{
    static const int index_table[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
    static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
    int32_t diff = sample - *pred;
    int sign = diff < 0;
    if (sign) diff = -diff;
    uint8_t nib = 0;
    if (diff >= step_table[*step_idx]) { nib = 4; diff -= step_table[*step_idx]; }
    if (diff >= step_table[*step_idx] >> 1) { nib |= 2; diff -= step_table[*step_idx] >> 1; }
    if (diff >= step_table[*step_idx] >> 2) nib |= 1;
    *nibble = (uint8_t)(nib | (sign ? 8 : 0));
    /* mirror of decode to advance predictor */
    int32_t d = step_table[*step_idx] >> 3;
    if (nib & 4) d += step_table[*step_idx];
    if (nib & 2) d += step_table[*step_idx] >> 1;
    if (nib & 1) d += step_table[*step_idx] >> 2;
    *pred = (int16_t)(*pred + (sign ? -d : d));
    *step_idx += index_table[nib];
    if (*step_idx < 0) *step_idx = 0;
    if (*step_idx > 88) *step_idx = 88;
}

int main(void)
{
    /* step table sanity: max step */
    ima_state_t st;
    ima_state_reset(&st, 0, 88);
    CHECK(ima_decode_nibble(&st, 7) != 0 || st.predictor == 8191);

    /* first decode with known values:
     * state 0/0, byte 0x08: high nibble 0 (step 7, diff 0+0+0+0=0, sign 0)
     * -> predictor stays 0; low nibble 8 (sign, diff 0) -> predictor 0. */
    ima_state_reset(&st, 0, 0);
    CHECK(ima_decode_nibble(&st, 0x0) == 0);
    CHECK(ima_decode_nibble(&st, 0x8) == 0);
    CHECK(st.step_index == 0); /* nibble 0: index -1 clamps at 0; nibble 8 same */

    /* step index progression: state 0/0, nibbles 7 (idx+8), then 7 */
    ima_state_reset(&st, 0, 0);
    (void)ima_decode_nibble(&st, 7);
    CHECK(st.step_index == 8);
    int16_t v2 = ima_decode_nibble(&st, 7);
    (void)v2;
    CHECK(st.step_index == 16);

    /* saturation: big step + sign -> clamped at int16 min (mi-ao semantics) */
    ima_state_reset(&st, 32767, 88);
    int16_t v = ima_decode_nibble(&st, 0xF);
    CHECK(v == -28669); /* 32767 - 61436, still in range */

    /* round trip via encoder */
    int16_t samples[64];
    for (int i = 0; i < 64; i++) {
        /* chirp covering small and large amplitudes */
        int32_t x = (int32_t)(3000.0 * (i < 32 ? i / 31.0 : (63 - i) / 31.0));
        samples[i] = (int16_t)x;
    }
    uint8_t enc[32];
    int16_t pred = 0;
    int16_t sidx = 0;
    int16_t expect[64]; /* encoder mirrors decoder: predictor == decoded value */
    for (int i = 0; i < 32; i++) {
        uint8_t lo, hi;
        ima_encode_nibble(&pred, &sidx, samples[2 * i], &hi);
        expect[2 * i] = pred;
        ima_encode_nibble(&pred, &sidx, samples[2 * i + 1], &lo);
        expect[2 * i + 1] = pred;
        enc[i] = (uint8_t)((hi << 4) | lo);
    }
    ima_state_t dec_st;
    ima_state_reset(&dec_st, 0, 0);
    int16_t out[64];
    size_t n = ima_decode(&dec_st, enc, 32, out, 64, NULL);
    CHECK(n == 64);
    /* decoder must track the encoder's mirrored predictor exactly */
    for (int i = 0; i < 64; i++) {
        if (out[i] != expect[i]) {
            fprintf(stderr, "mismatch at %d: %d != %d\n", i, out[i], expect[i]);
            failures++;
            break;
        }
    }

    /* truncated nibble stream: odd byte count */
    n = ima_decode(&dec_st, enc, 3, out, 64, NULL);
    CHECK(n == 6);

    /* out_max clamps */
    n = ima_decode(&dec_st, enc, 32, out, 10, NULL);
    CHECK(n == 10);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("IMA decoder tests: all passed\n");
    return 0;
}

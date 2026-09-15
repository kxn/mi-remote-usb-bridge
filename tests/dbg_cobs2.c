#include "rbp/frame.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint32_t st = 12345;
static uint32_t rnd(void)
{
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

int main(void)
{
    uint8_t in[700], enc[900], dec[900];
    int fails = 0;
    /* deterministic patterns first */
    for (int L = 1; L <= 600 && fails < 6; L++) {
        for (int pat = 0; pat < 4; pat++) {
            for (int i = 0; i < L; i++) {
                switch (pat) {
                case 0: in[i] = 0x00; break;
                case 1: in[i] = 0xFF; break;
                case 2: in[i] = (uint8_t)(i % 253 ? 0x41 : 0x00); break;
                default: in[i] = (uint8_t)rnd(); break;
                }
            }
            size_t e = rbp_cobs_encode(in, (size_t)L, enc);
            size_t d = rbp_cobs_decode(enc, e, dec);
            if (d != (size_t)L || memcmp(in, dec, (size_t)L) != 0) {
                printf("FAIL L=%d pat=%d enc=%zu dec=%zu\n", L, pat, e, d);
                if (++fails >= 6) return 0;
            }
        }
    }
    printf("scan done\n");
    return 0;
}

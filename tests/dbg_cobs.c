#include "rbp/frame.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_AUDIO;
    h.opcode = RBP_OP_VOICE_DATA;
    h.session_id = 0xDEADBEEF;
    uint8_t payload[RBP_MAX_PAYLOAD];
    memset(payload, 0xAB, sizeof(payload));
    rbp_txseq_t q;
    rbp_txseq_init(&q);
    uint8_t wire[RBP_MAX_DECODED + 8];
    size_t n = rbp_frame_encode(&h, &q, payload, sizeof(payload), wire);
    printf("wire_len=%zu\n", n);

    /* feed through the rx parser exactly like the harness */
    rbp_rxparser_t p;
    rbp_rx_event_ctx_t ctx;
    rbp_rxparser_init(&p, 0);
    size_t got = 0;
    const uint8_t *cur = wire;
    size_t left = n;
    while (left > 0) {
        rbp_rx_event_t ev = rbp_rxparser_feed(&p, cur, left, 0, &ctx);
        cur += (left - ctx.resume_len);
        left = ctx.resume_len;
        printf("ev=%d\n", (int)ev);
        if (ev == RBP_RX_FRAME) {
            got++;
            printf("  payload_len=%u opcode=%04x match=%d\n", ctx.payload_len,
                   ctx.header.opcode,
                   ctx.payload_len == RBP_MAX_PAYLOAD &&
                       memcmp(ctx.payload, payload, RBP_MAX_PAYLOAD) == 0);
        }
    }
    printf("frames=%zu\n", got);

    /* also: decode manually excluding the delimiter */
    uint8_t raw[RBP_MAX_DECODED];
    size_t raw_len = rbp_cobs_decode(wire, n - 1, raw);
    printf("manual decode raw_len=%zu expect %u\n", raw_len, RBP_HEADER_SIZE + RBP_MAX_PAYLOAD + 4);
    return 0;
}

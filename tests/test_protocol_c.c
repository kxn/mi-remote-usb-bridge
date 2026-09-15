/* C unit tests for the RBP wire protocol library.  Exit code 0 = pass. */
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_crc32c(void)
{
    CHECK(rbp_crc32c((const uint8_t *)"123456789", 9) == 0xE3069283u);
    CHECK(rbp_crc32c((const uint8_t *)"", 0) == 0x00000000u);
    CHECK(rbp_crc32c((const uint8_t *)"\xff", 1) == 0xFF000000u);
}

static void test_cobs_edges(void)
{
    uint8_t buf[600], enc[700], dec[700];
    /* round trip various sizes incl. block boundaries */
    size_t sizes[] = { 0, 1, 2, 253, 254, 255, 256, 257, 508, 509, 548 };
    for (int s = 0; s < (int)(sizeof(sizes) / sizeof(sizes[0])); s++) {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 7 + 1); /* no zeros */
        size_t e = rbp_cobs_encode(buf, n, enc);
        size_t d = rbp_cobs_decode(enc, e, dec);
        CHECK(d == n && memcmp(dec, buf, n) == 0);
        memcpy(dec,enc,e);
        d=rbp_cobs_decode(dec,e,dec);
        CHECK(d==n && memcmp(dec,buf,n)==0);
        /* every 32nd byte zero */
        for (size_t i = 0; i < n; i++) buf[i] = (i % 32 == 31) ? 0 : (uint8_t)(i + 1);
        e = rbp_cobs_encode(buf, n, enc);
        d = rbp_cobs_decode(enc, e, dec);
        CHECK(d == n && memcmp(dec, buf, n) == 0);
        memcpy(dec,enc,e);d=rbp_cobs_decode(dec,e,dec);
        CHECK(d==n && memcmp(dec,buf,n)==0);
    }
    /* corrupt code byte must fail or not crash */
    enc[0] = 0xFF; enc[1] = 0x01;
    CHECK(rbp_cobs_decode(enc, 2, dec) == 0);
    /* embedded zero code byte is invalid */
    enc[0] = 0x02; enc[1] = 0x00; enc[2] = 0x00;
    CHECK(rbp_cobs_decode(enc, 3, dec) == 0);
}

static void test_header_roundtrip(void)
{
    rbp_header_t h = {
        RBP_MAGIC0, RBP_MAGIC1, RBP_MAJOR, RBP_MINOR, RBP_KIND_RESPONSE, 0,
        RBP_HEADER_SIZE, 0xDEADBEEFu, 42u, 7u, RBP_OP_PING, RBP_STATUS_OK,
        0, 0, 0x12345678u,
    };
    uint8_t raw[RBP_HEADER_SIZE];
    rbp_header_pack(&h, raw);
    rbp_header_t p;
    CHECK(rbp_header_parse(raw, &p));
    CHECK(p.session_id == 0xDEADBEEFu && p.tx_seq == 42 && p.request_id == 7);
    CHECK(p.opcode == RBP_OP_PING && p.connection_id == 0x12345678u);
    raw[0] = 'X';
    CHECK(!rbp_header_parse(raw, &p));
    raw[0] = RBP_MAGIC0;
    raw[26] = 1; /* reserved must be zero */
    CHECK(!rbp_header_parse(raw, &p));
}

static void test_frame_roundtrip(void)
{
    rbp_txseq_t seq;
    rbp_txseq_init(&seq);
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_REQUEST;
    h.opcode = RBP_OP_GET_DEVICE;

    uint8_t wire[600];
    size_t n = rbp_frame_encode(&h, &seq, NULL, 0, wire);
    CHECK(n >= RBP_HEADER_SIZE + 5);

    /* feed byte-by-byte to receiver */
    rbp_rxparser_t p;
    rbp_rx_event_ctx_t ctx;
    rbp_rxparser_init(&p, 0);
    rbp_rx_event_t ev = RBP_RX_NONE;
    for (size_t i = 0; i < n && ev == RBP_RX_NONE; i++)
        ev = rbp_rxparser_feed(&p, &wire[i], 1, (uint32_t)i / 10, &ctx);
    CHECK(ev == RBP_RX_FRAME);
    CHECK(ctx.crc_ok);
    CHECK(ctx.header.opcode == RBP_OP_GET_DEVICE && ctx.payload_len == 0);
    CHECK(ctx.resume_len == 0);

    /* wrong CRC detected */
    wire[5] ^= 0x40; /* flip a bit inside flags */
    rbp_rxparser_init(&p, 0);
    ev = rbp_rxparser_feed(&p, wire, n, 0, &ctx);
    CHECK(ev == RBP_RX_FRAME && !ctx.crc_ok);
}

static void test_rx_multi_and_garbage(void)
{
    rbp_txseq_t seq;
    rbp_txseq_init(&seq);
    uint8_t w1[600], w2[600];
    rbp_header_t h1 = { 0 }, h2 = { 0 };
    h1.kind = RBP_KIND_REQUEST; h1.opcode = RBP_OP_PING;
    h2.kind = RBP_KIND_REQUEST; h2.opcode = RBP_OP_GET_STATS;
    size_t n1 = rbp_frame_encode(&h1, &seq, NULL, 0, w1);
    size_t n2 = rbp_frame_encode(&h2, &seq, NULL, 0, w2);

    uint8_t stream[1400];
    size_t off = 0;
    stream[off++] = 0x00; /* stray empty delimiter */
    memcpy(stream + off, w1, n1); off += n1;
    stream[off++] = 0x77; stream[off++] = 0x00; /* 1 garbage byte frame */
    memcpy(stream + off, w2, n2); off += n2;

    rbp_rxparser_t q;
    rbp_rx_event_ctx_t ctx;
    rbp_rxparser_init(&q, 0);
    const uint8_t *cur = stream;
    size_t left = off;
    int got_ping = 0, got_stats = 0, garbage = 0;
    while (left > 0) {
        rbp_rx_event_t ev = rbp_rxparser_feed(&q, cur, left, 0, &ctx);
        cur += (left - ctx.resume_len);
        left = ctx.resume_len;
        if (ev == RBP_RX_FRAME && ctx.crc_ok) {
            if (ctx.header.opcode == RBP_OP_PING) got_ping++;
            if (ctx.header.opcode == RBP_OP_GET_STATS) got_stats++;
        } else if (ev == RBP_RX_GARBAGE) {
            garbage++;
        }
    }
    CHECK(got_ping == 1 && got_stats == 1 && garbage == 1);
    CHECK(left == 0);
}

static void test_rx_overflow_and_timeout(void)
{
    rbp_rxparser_t p;
    rbp_rx_event_ctx_t ctx;
    rbp_rxparser_init(&p, 0);
    uint8_t junk[700];
    memset(junk, 0xAB, sizeof(junk));
    /* more than 552 bytes with no delimiter -> overflow */
    rbp_rx_event_t ev = rbp_rxparser_feed(&p, junk, 600, 0, &ctx);
    CHECK(ev == RBP_RX_OVERFLOW);
    /* still discarding */
    ev = rbp_rxparser_feed(&p, junk, 100, 0, &ctx);
    CHECK(ev == RBP_RX_NONE);
    uint8_t endjunk[4] = { 1, 2, 3, 0x00 };
    ev = rbp_rxparser_feed(&p, endjunk, 4, 0, &ctx);
    CHECK(ev == RBP_RX_NONE);
    /* now a valid frame must parse */
    rbp_txseq_t seq;
    rbp_txseq_init(&seq);
    rbp_header_t h = { 0 };
    h.kind = RBP_KIND_REQUEST; h.opcode = RBP_OP_PING;
    uint8_t w[600];
    size_t n = rbp_frame_encode(&h, &seq, NULL, 0, w);
    ev = rbp_rxparser_feed(&p, w, n, 0, &ctx);
    CHECK(ev == RBP_RX_FRAME && ctx.crc_ok);

    /* half-frame timeout */
    rbp_rxparser_init(&p, 1000);
    uint8_t half[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    ev = rbp_rxparser_feed(&p, half, 10, 1000, &ctx);
    CHECK(ev == RBP_RX_NONE);
    ev = rbp_rxparser_feed(&p, half, 0, 2100, &ctx);
    CHECK(ev == RBP_RX_TIMEOUT);
}

static void test_tlv_basics(void)
{
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u64(&w, 1, 0x1122334455667788ULL);
    rbp_tlv_put_bool(&w, 2, true);
    rbp_tlv_put_u8(&w, 3, 7);
    rbp_tlv_put_text(&w, 4, "h\xc3\xa9llo");
    rbp_tlv_put_bytes(&w, 7, (const uint8_t *)"\x01\x02", 2);
    rbp_tlv_put_u16(&w, 10, 0xBEEF);
    CHECK(!w.overflow);

    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, w.buf, w.len);
    bool found;
    uint8_t v8; uint16_t v16; uint64_t v64; bool vb;
    const uint8_t *txt; uint16_t tl;
    CHECK(rbp_tlv_get_u8(&r, 3, &v8, &found) == RBP_TLV_OK && found && v8 == 7);
    CHECK(rbp_tlv_get_u16(&r, 10, &v16, &found) == RBP_TLV_OK && found && v16 == 0xBEEF);
    CHECK(rbp_tlv_get_bool(&r, 2, &vb, &found) == RBP_TLV_OK && found && vb);
    CHECK(rbp_tlv_get_text(&r, 4, &txt, &tl, &found) == RBP_TLV_OK && found && tl == 6);
    CHECK(rbp_tlv_get_u64(&r, 1, &v64, &found) == RBP_TLV_OK && found && v64 == 0x1122334455667788ULL);

    /* wrong type rejected */
    rbp_tlv_reader_init(&r, w.buf, w.len);
    uint32_t v32;
    CHECK(rbp_tlv_get_u32(&r, 3, &v32, &found) == RBP_TLV_ERR_TYPE);
    /* bool with value 2 rejected */
    uint8_t bad[16];
    bad[0] = 5; bad[1] = 5; bad[2] = 1; bad[3] = 0; bad[4] = 2;
    rbp_tlv_reader_init(&r, bad, 5);
    CHECK(rbp_tlv_get_bool(&r, 5, &vb, &found) == RBP_TLV_ERR_VALUE);
    /* duplicate tag rejected */
    uint8_t dup[10];
    dup[0] = 9; dup[1] = 1; dup[2] = 1; dup[3] = 0; dup[4] = 0xAA;
    dup[5] = 9; dup[6] = 1; dup[7] = 1; dup[8] = 0; dup[9] = 0xBB;
    rbp_tlv_reader_init(&r, dup, 10);
    uint8_t tmp;
    CHECK(rbp_tlv_get_u8(&r, 9, &tmp, &found) == RBP_TLV_ERR_DUPLICATE);
    /* malformed: length runs past end */
    rbp_tlv_reader_init(&r, dup, 7);
    CHECK(rbp_tlv_get_u8(&r, 9, &tmp, &found) == RBP_TLV_ERR_MALFORMED);
    /* invalid utf8 (0xFF) */
    uint8_t badu[5] = { 4, 6, 1, 0, 0xFF };
    rbp_tlv_reader_init(&r, badu, 5);
    CHECK(rbp_tlv_get_text(&r, 4, &txt, &tl, &found) == RBP_TLV_ERR_VALUE);
    /* embedded NUL rejected */
    uint8_t nul[5] = { 4, 6, 1, 0, 0 };
    rbp_tlv_reader_init(&r, nul, 5);
    CHECK(rbp_tlv_get_text(&r, 4, &txt, &tl, &found) == RBP_TLV_ERR_VALUE);
    /* overlong encoding rejected (C0 80) */
    uint8_t over[6] = { 4, 6, 2, 0, 0xC0, 0x80 };
    rbp_tlv_reader_init(&r, over, 6);
    CHECK(rbp_tlv_get_text(&r, 4, &txt, &tl, &found) == RBP_TLV_ERR_VALUE);
    /* unknown type in unknown tag is skippable */
    uint8_t unk[13];
    unk[0] = 0xF0; unk[1] = 0x55; unk[2] = 4; unk[3] = 0; unk[4] = 1; unk[5] = 2; unk[6] = 3; unk[7] = 4;
    unk[8] = 6; unk[9] = 1; unk[10] = 1; unk[11] = 0; unk[12] = 0;
    rbp_tlv_reader_init(&r, unk, 13);
    CHECK(rbp_tlv_get_u8(&r, 6, &tmp, &found) == RBP_TLV_OK && found && tmp == 0);
}

static void test_tlv_writer_order_and_capacity(void)
{
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u8(&w, 5, 1);
    rbp_tlv_put_u8(&w, 2, 1); /* descending order not allowed */
    CHECK(w.overflow);
    rbp_tlv_writer_init(&w);
    char big[120];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    rbp_tlv_put_text(&w, 1, big); /* >96 */
    CHECK(w.overflow);
    rbp_tlv_writer_init(&w);
    uint8_t many[600];
    memset(many, 1, sizeof(many));
    rbp_tlv_put_bytes(&w, 1, many, 490); /* >480 */
    CHECK(w.overflow);
}

static void test_direct_encoder_boundaries(void)
{
    uint8_t payload[RBP_MAX_PAYLOAD], direct[RBP_MAX_ENCODED], built[RBP_MAX_ENCODED];
    for (size_t i=0; i<sizeof(payload); i++) payload[i]=(uint8_t)i;
    for(unsigned pattern=0;pattern<3;pattern++) {
    for(size_t i=0;i<sizeof(payload);i++)payload[i]=pattern==0?0:pattern==1?0xff:(uint8_t)i;
    for (size_t len=0; len<=sizeof(payload); len++) {
        rbp_header_t h={0}; h.kind=RBP_KIND_AUDIO;
        rbp_txseq_t a,b; rbp_txseq_init(&a); rbp_txseq_init(&b);
        rbp_txframe_t f; rbp_txframe_begin(&f,&h);
        CHECK(rbp_txframe_payload(&f,payload,len/2));
        CHECK(rbp_txframe_payload(&f,payload+len/2,len-len/2));
        size_t na=rbp_frame_encode(&h,&a,payload,len,direct);
        size_t nb=rbp_txframe_commit(&f,&b,built);
        CHECK(na==nb && memcmp(direct,built,na)==0);
        uint8_t raw[RBP_MAX_FRAME_RAW],reference[RBP_MAX_ENCODED];
        rbp_header_pack(&f.hdr,raw);memcpy(raw+RBP_HEADER_SIZE,payload,len);
        uint32_t crc=rbp_crc32c(raw,RBP_HEADER_SIZE+len);
        for(unsigned j=0;j<4;j++)raw[RBP_HEADER_SIZE+len+j]=(uint8_t)(crc>>(8*j));
        size_t nr=rbp_cobs_encode(raw,RBP_HEADER_SIZE+len+4,reference);reference[nr++]=0;
        CHECK(nr==na&&memcmp(reference,direct,na)==0);
        CHECK(h.tx_seq==0 && f.hdr.tx_seq==1 && a.next_tx_seq==2 && b.next_tx_seq==2);
        CHECK(rbp_frame_encode(&h,&a,payload,RBP_MAX_PAYLOAD+1,direct)==0);
        CHECK(a.next_tx_seq==2);
        CHECK(!rbp_txframe_payload(&f,payload,(size_t)-1));
        CHECK(f.payload_len==len);
    }
    }
}

int main(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[crc]\n"); test_crc32c();
    fprintf(stderr, "[cobs]\n"); test_cobs_edges();
    fprintf(stderr, "[header]\n"); test_header_roundtrip();
    fprintf(stderr, "[frame]\n"); test_frame_roundtrip(); test_direct_encoder_boundaries();
    fprintf(stderr, "[multi]\n"); test_rx_multi_and_garbage();
    fprintf(stderr, "[ovf]\n"); test_rx_overflow_and_timeout();
    fprintf(stderr, "[tlv]\n"); test_tlv_basics();
    fprintf(stderr, "[tlvw]\n"); test_tlv_writer_order_and_capacity();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("protocol C tests: all passed\n");
    return 0;
}

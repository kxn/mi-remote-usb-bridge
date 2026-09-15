/* Product server tests: drives rbp_server_t directly with a scripted
 * transport/radio/store, asserting wire behaviour per docs/wire-protocol.md. */
#include "rbp_server.h"
#include "device_model.h"
#include "rbp/frame.h"
#include "rbp/tlv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* ---------- harness ---------- */

#define OUT_CAP (64 * 1024)
static uint8_t out_buf[OUT_CAP];
static size_t out_len;
static bool sink_blocked;

static size_t out_sink(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    if(sink_blocked)return 0;
    if (len > OUT_CAP - out_len) len = OUT_CAP - out_len;
    memcpy(out_buf + out_len, data, len);
    out_len += len;
    return len;
}

static uint32_t rng_state = 42;
static uint32_t test_rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static rbp_peer_record_t stored_peer;
static bool stored_valid;
static int store_save_calls;
static bool store_fail;

static bool store_load(void *user, rbp_peer_record_t *out)
{
    (void)user;
    if (!stored_valid) return false;
    *out = stored_peer;
    return true;
}

static bool store_save(void *user, const rbp_peer_record_t *rec)
{
    (void)user;
    store_save_calls++;
    if (store_fail) return false;
    stored_peer = *rec;
    stored_valid = rec->peer_id != 0;
    return true;
}

static bool store_clear(void *user)
{
    (void)user;
    stored_valid = false;
    return true;
}

/* radio mock: keeps the server pointer to feed events back */
static rbp_server_t *g_srv;
static uint32_t g_test_capture_ms = 120000; /* per-test override */

static struct {
    int pair_begin_calls;
    int pair_cancel_calls;
    int forget_calls;
    int voice_stop_calls;
    int connect_calls;
    int disconnect_calls;
    int stop_find_calls;
} radio;

static uint16_t scan_submit_status;
static uint16_t rb_start_find(void *user, uint16_t duration_ms)
{
    (void)user;
    (void)duration_ms;
    if(scan_submit_status)return scan_submit_status;
    rbp_candidate_t c;
    memset(&c, 0, sizeof(c));
    c.candidate_id = 0xC1;
    c.support = 1;
    c.signal = 3;
    strcpy(c.name, "Xiaomi RC003");
    c.name_len = (uint8_t)strlen(c.name);
    rbp_server_on_scan_candidate(g_srv, &c);
    return RBP_STATUS_OK;
}

static void rb_stop_find(void *user) { (void)user; radio.stop_find_calls++; }
static void rb_pair_begin(void *user, uint32_t cid) { (void)user; (void)cid; radio.pair_begin_calls++; }
static void rb_pair_reply(void *user, bool a, bool hp, uint32_t pk)
{
    (void)user; (void)a; (void)hp; (void)pk;
}
static void rb_pair_cancel(void *user) { (void)user; radio.pair_cancel_calls++; }
static void rb_forget_peer(void *user) { (void)user; radio.forget_calls++; }
static void rb_connect_peer(void *user) { (void)user; radio.connect_calls++; }
static bool disconnect_sync;
static void rb_disconnect(void *user) { (void)user; radio.disconnect_calls++; if(disconnect_sync)rbp_server_on_link(g_srv,RBP_LINK_DISCONNECTED,0,0); }
static bool teardown_reenter;
static void rb_voice_stop(void *user) {
    (void)user;radio.voice_stop_calls++;
    if(teardown_reenter) {
        teardown_reenter=false;
        rbp_server_info_t info;rbp_server_get_info(g_srv,&info);
        CHECK(!info.session_active && info.session_id==0);
        rbp_server_on_usb_gone(g_srv,100);
        rbp_voice_evt_t end={0};end.type=RBP_VOICE_EVT_END;
        rbp_server_on_voice(g_srv,&end,100);
    }
}

static unsigned voice_start_calls;static uint16_t voice_start_status;
static uint16_t rb_voice_start(void *u){(void)u;voice_start_calls++;return voice_start_status;}
static const rbp_radio_backend_t k_backend = {
    NULL, rb_start_find, rb_stop_find, rb_pair_begin, rb_pair_reply,
    rb_pair_cancel, rb_forget_peer, rb_connect_peer, rb_disconnect,
    rb_voice_stop, rb_voice_start,
};

/* ---------- output parsing ---------- */

#define MAX_FRAMES 256
typedef struct {
    rbp_header_t hdr;
    uint8_t payload[RBP_MAX_PAYLOAD];
    uint16_t len;
    bool crc_ok;
} frame_t;

static frame_t frames[MAX_FRAMES];
static size_t frame_count;
static rbp_rxparser_t rx_parser;

static void reset_frames(void) { frame_count = 0; rbp_rxparser_init(&rx_parser,0); }

static void parse_pending(void)
{
    if (frame_count >= MAX_FRAMES || out_len == 0) { out_len = 0; return; }
    const uint8_t *cur = out_buf;
    size_t left = out_len;
    rbp_rx_event_ctx_t ctx;

    while (left > 0) {
        rbp_rx_event_t ev = rbp_rxparser_feed(&rx_parser, cur, left, 0, &ctx);
        cur += (left - ctx.resume_len);
        left = ctx.resume_len;
        if (ev == RBP_RX_FRAME && frame_count < MAX_FRAMES) {
            frame_t *f = &frames[frame_count++];
            f->hdr = ctx.header;
            f->len = ctx.payload_len;
            f->crc_ok = ctx.crc_ok;
            memcpy(f->payload, ctx.payload, ctx.payload_len);
        }
    }
    out_len = 0;
}

static void tick(rbp_server_t *s, uint32_t now)
{
    parse_pending();
    rbp_server_tick(s, now);
    parse_pending();
}

static frame_t *find_frame(uint8_t kind, uint16_t opcode)
{
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.kind == kind && frames[i].hdr.opcode == opcode)
            return &frames[i];
    }
    return NULL;
}

/* ---------- client stub ---------- */

static uint32_t g_tx_seq;
static uint32_t g_req_id;
static uint32_t g_session;

static void client_reset(void)
{
    g_tx_seq = 0;
    g_req_id = 0;
    g_session = 0;
}

static void client_send_raw(rbp_server_t *s, uint32_t now, rbp_header_t *h,
                            const uint8_t *payload, uint16_t len)
{
    uint8_t wire_buf[RBP_MAX_ENCODED];
    g_tx_seq++;
    h->tx_seq = g_tx_seq;
    size_t n = rbp_frame_encode(h, &(rbp_txseq_t){ .next_tx_seq = g_tx_seq },
                                payload, len, wire_buf);
    /* rbp_frame_encode assigns its own seq via the struct we pass */
    rbp_server_on_usb_rx(s, wire_buf, n, now);
}

static void client_request(rbp_server_t *s, uint32_t now, uint16_t opcode,
                           const uint8_t *payload, uint16_t len)
{
    g_req_id++;
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_REQUEST;
    h.session_id = g_session;
    h.request_id = g_req_id;
    h.opcode = opcode;
    h.connection_id = 0;
    client_send_raw(s, now, &h, payload, len);
}

static void client_hello(rbp_server_t *s, uint32_t now)
{
    uint8_t nonce[16];
    for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(0xA0 + i);
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bytes(&w, 1, nonce, 16);
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_REQUEST;
    h.session_id = 0;
    h.request_id = 1;
    h.opcode = RBP_OP_HELLO;
    client_send_raw(s, now, &h, w.buf, w.len);
    g_req_id = 1;
}

/* ---------- voice input helper ---------- */

/* VOICE_ENDED payload is TLV 1:sid:u32, 2:reason:u8, 3:delivered:u64 */
static bool parse_ended(const frame_t *f, uint8_t *reason, uint64_t *delivered)
{
    rbp_tlv_reader_t rd;
    rbp_tlv_reader_init(&rd, f->payload, f->len);
    uint32_t sid = 0;
    bool f1 = false, f2 = false, f3 = false;
    if (rbp_tlv_get_u32(&rd, 1, &sid, &f1) != RBP_TLV_OK) return false;
    if (rbp_tlv_get_u8(&rd, 2, reason, &f2) != RBP_TLV_OK) return false;
    if (rbp_tlv_get_u64(&rd, 6, delivered, &f3) != RBP_TLV_OK) return false;
    (void)sid;
    return f1 && f2 && f3;
}

static uint8_t encoded_block[512];
static void feed_voice(rbp_server_t *s,uint32_t now,const rbp_voice_evt_t *input) {
    rbp_voice_evt_t ev=*input;static const uint8_t seed[4]={0};
    if(ev.type==RBP_VOICE_EVT_START || ev.type==RBP_VOICE_EVT_FORMAT) {
        uint32_t rate=ev.u.start.sample_rate;
        ev.u.start=(rbp_audio_format_t){RBP_CODEC_IMA_HI,rate,512,1,4,1,seed};
        if(ev.type==RBP_VOICE_EVT_START) {rbp_voice_evt_t begin={0};begin.type=RBP_VOICE_EVT_SOURCE_BEGIN;rbp_server_on_voice(s,&begin,now);}
    }
    rbp_server_on_voice(s,&ev,now);
}
static void feed_silence(rbp_server_t*s,uint32_t now,uint16_t samples) {
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_ENCODED;
    ev.u.encoded.data=encoded_block;ev.u.encoded.len=samples/2;
    ev.u.encoded.unit_size=samples/2;ev.u.encoded.samples=samples;feed_voice(s,now,&ev);
}

/* ---------- shared setup ---------- */

static rbp_server_t *server_new(void)
{
    g_srv = (rbp_server_t *)malloc(rbp_server_object_size());
    rbp_server_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = k_backend;
    cfg.store.load = store_load;
    cfg.store.save = store_save;
    cfg.store.clear = store_clear;
    cfg.out = out_sink;
    cfg.rng = test_rng;
    cfg.profile = &RBP_PROFILE_RC003;
    cfg.max_keys = 64;
    cfg.max_capture_ms = g_test_capture_ms;
    for (int i = 0; i < 16; i++) cfg.bridge_uid[i] = (uint8_t)(0x10 + i);
    cfg.firmware_version = "test 1";
    cfg.reset_reason = 1;
    rbp_server_init(g_srv, &cfg, NULL, 0);
    static const rbp_codec_t codecs[]={{1,1}};static const rbp_audio_caps_t caps={codecs,512,1};
    rbp_server_set_voice_caps(g_srv,&caps);
    return g_srv;
}

static void server_free(void)
{
    free(g_srv);
    g_srv = NULL;
}

/* request with explicit connection id */
static void client_request_conn(rbp_server_t *s, uint32_t now, uint16_t opcode,
                                const uint8_t *payload, uint16_t len,
                                uint32_t connection_id);

/* ---------- tests ---------- */

static void test_hello_ping(void)
{
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);

    client_hello(s, 20);
    tick(s, 21);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_HELLO);
    CHECK(resp != NULL);
    if (!resp) { server_free(); return; }
    CHECK(resp->hdr.tx_seq == 1); /* first server frame of the session */
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    CHECK(info.session_active && info.session_id != 0);
    g_session = info.session_id;

    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    const uint8_t *uid;
    uint16_t ul;
    bool found = false;
    CHECK(rbp_tlv_get_bytes(&r, 2, &uid, &ul, &found) == RBP_TLV_OK && found && ul == 16);
    CHECK(uid[0] == 0x10 && uid[15] == 0x1F);

    /* ping */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, 0xCAFEF00D);
    uint32_t t0 = 100;
    client_request(s, t0, RBP_OP_PING, w.buf, w.len);
    tick(s, t0 + 2);
    resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_PING);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    uint32_t cookie = 0;
    uint64_t board_us = 0;
    found = false;
    CHECK(rbp_tlv_get_u32(&r, 1, &cookie, &found) == RBP_TLV_OK && found);
    CHECK(cookie == 0xCAFEF00D);
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    CHECK(rbp_tlv_get_u64(&r, 2, &board_us, &found) == RBP_TLV_OK && found);
    CHECK(board_us == t0 * 1000ULL); /* stamped at request processing */

    /* duplicate request id rejected */
    client_request(s, t0 + 5, RBP_OP_PING, w.buf, w.len); /* id 3 */
    tick(s, t0 + 6);
    /* replay id 3 by rebuilding frame manually */
    {
        uint8_t wire_buf[RBP_MAX_ENCODED];
        rbp_header_t h;
        memset(&h, 0, sizeof(h));
        h.kind = RBP_KIND_REQUEST;
        h.session_id = g_session;
        h.request_id = 3;
        h.opcode = RBP_OP_PING;
        g_tx_seq++;
        rbp_txseq_t seq;
        seq.next_tx_seq = g_tx_seq;
        size_t n = rbp_frame_encode(&h, &seq, w.buf, w.len, wire_buf);
        rbp_server_on_usb_rx(s, wire_buf, n, t0 + 7);
    }
    tick(s, t0 + 8);
    resp = NULL;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.kind == RBP_KIND_RESPONSE &&
            frames[i].hdr.opcode == RBP_OP_PING &&
            frames[i].hdr.status == RBP_STATUS_DUPLICATE) {
            resp = &frames[i];
        }
    }
    CHECK(resp != NULL);

    server_free();
}

static void test_find_pair_keys(void)
{
    memset(&radio, 0, sizeof(radio));
    stored_valid = false;
    store_save_calls = 0;
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* find */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u16(&w, 1, 300);
    scan_submit_status=RBP_STATUS_RESOURCE_LIMIT;
    client_request(s,90,RBP_OP_FIND_START,w.buf,w.len);tick(s,91);
    frame_t *rejected=find_frame(RBP_KIND_RESPONSE,RBP_OP_FIND_START);
    CHECK(rejected && rejected->hdr.status==RBP_STATUS_RESOURCE_LIMIT);
    CHECK(find_frame(RBP_KIND_EVENT,RBP_OP_FIND_DONE_EV)==NULL);
    scan_submit_status=RBP_STATUS_OK;reset_frames();
    client_request(s, 100, RBP_OP_FIND_START, w.buf, w.len);
    tick(s, 101);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_FIND_START);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    uint32_t search_id = 0;
    bool found = false;
    CHECK(rbp_tlv_get_u32(&r, 1, &search_id, &found) == RBP_TLV_OK && found);

    /* FIND_LIST while scanning */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, search_id);
    rbp_tlv_put_u8(&w, 2, 0);
    client_request(s, 120, RBP_OP_FIND_LIST, w.buf, w.len);
    tick(s, 121);
    resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_FIND_LIST);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    uint8_t next_cursor = 0;
    const uint8_t *entries;
    uint16_t elen;
    CHECK(rbp_tlv_get_u8(&r, 1, &next_cursor, &found) == RBP_TLV_OK && found);
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    CHECK(rbp_tlv_get_bytes(&r, 2, &entries, &elen, &found) == RBP_TLV_OK && found);
    CHECK(entries[0] == 1); /* one candidate */
    uint32_t cid;
    memcpy(&cid, entries + 1, 4);
    CHECK(cid == 0xC1);
    CHECK(elen==8+strlen("Xiaomi RC003") && entries[7]==strlen("Xiaomi RC003"));
    CHECK(!memcmp(entries+8,"Xiaomi RC003",strlen("Xiaomi RC003")));

    /* expiry -> FIND_DONE */
    tick(s, 500);
    frame_t *fd = find_frame(RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV);
    CHECK(fd != NULL);
    rbp_tlv_reader_init(&r, fd->payload, fd->len);
    uint8_t reason = 255;
    CHECK(rbp_tlv_get_u8(&r, 2, &reason, &found) == RBP_TLV_OK && found);
    CHECK(reason == RBP_FIND_DONE_EXPIRED);

    /* pair */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, search_id);
    rbp_tlv_put_u32(&w, 2, 0xC1);
    client_request(s, 520, RBP_OP_PAIR_BEGIN, w.buf, w.len);
    tick(s, 521);
    resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_PAIR_BEGIN);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_ACCEPTED);
    uint32_t op_id = 0;
    {
        rbp_tlv_reader_t rr;
        rbp_tlv_reader_init(&rr, resp->payload, resp->len);
        bool f2 = false;
        CHECK(rbp_tlv_get_u32(&rr, 1, &op_id, &f2) == RBP_TLV_OK && f2);
    }
    CHECK(op_id == g_req_id);
    CHECK(radio.pair_begin_calls == 1);
    rbp_server_on_pair_prompt(s,RBP_PROMPT_ENTER_PASSKEY,0,120000);
    tick(s,522);
    frame_t *prompt=find_frame(RBP_KIND_EVENT,RBP_OP_PAIR_PROMPT_EV);
    CHECK(prompt!=NULL);
    if(prompt) {
        rbp_tlv_reader_t pr;rbp_tlv_reader_init(&pr,prompt->payload,prompt->len);
        uint32_t remaining=0;bool has_remaining=false;
        CHECK(rbp_tlv_get_u32(&pr,5,&remaining,&has_remaining)==RBP_TLV_OK&&has_remaining);
        CHECK(remaining>0&&remaining<=60000);
    }

    /* pairing completes (radio side) */
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "Xiaomi RC003");
    rec.auto_reconnect = false; /* server must default it to true */
    /* Hardware ordering: adapter finishes before durable pair commit. */
    rbp_server_on_link(s,RBP_LINK_READY,777,521);
    rbp_server_info_t pending;
    rbp_server_get_info(s,&pending);
    CHECK(pending.link_state==RBP_LINK_INITIALIZING);
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 777);
    tick(s, 522);
    frame_t *op = find_frame(RBP_KIND_EVENT, RBP_OP_OPERATION_EV);
    CHECK(op != NULL);
    rbp_tlv_reader_init(&r, op->payload, op->len);
    uint16_t result = 0xFFFF;
    CHECK(rbp_tlv_get_u16(&r, 3, &result, &found) == RBP_TLV_OK && found);
    CHECK(result == RBP_STATUS_OK);
    CHECK(stored_valid && stored_peer.peer_id == 0x1234 && stored_peer.auto_reconnect);
    CHECK(store_save_calls==1);
    rbp_server_get_info(s,&pending);CHECK(pending.link_state==RBP_LINK_READY);
    frame_t *ds = find_frame(RBP_KIND_EVENT, RBP_OP_DEVICE_STATE_EV);
    CHECK(ds != NULL); /* initializing */

    /* adapter finishes discovery */
    rbp_server_on_voice_state(s, RBP_VOICE_READY, RBP_VI_HTT, 16000);
    rbp_server_on_link(s, RBP_LINK_READY, 777, 600);
    tick(s, 601);
    {
        uint8_t st = 255;
        bool fnd = false;
        for (size_t i = 0; i < frame_count; i++) {
            if (frames[i].hdr.kind == RBP_KIND_EVENT &&
                frames[i].hdr.opcode == RBP_OP_DEVICE_STATE_EV) {
                rbp_tlv_reader_init(&r, frames[i].payload, frames[i].len);
                uint8_t v = 255;
                if (rbp_tlv_get_u8(&r, 3, &v, &fnd) == RBP_TLV_OK && fnd) st = v;
            }
        }
        CHECK(st == RBP_DEVSTATE_READY);
    }

    /* key catalog (connection scoped request; paged, <=12 per page) */
    {
        uint8_t total = 0;
        uint8_t cursor = 0;
        bool first_page = true;
        for (int page = 0; page < 4; page++) {
            rbp_tlv_writer_init(&w);
            if (cursor) rbp_tlv_put_u8(&w, 1, cursor);
            client_request_conn(s, (uint32_t)(610 + page * 2), RBP_OP_KEY_CATALOG,
                                w.buf, w.len, 777);
            tick(s, (uint32_t)(611 + page * 2));
            frame_t *cresp = NULL;
            for (size_t i = frame_count; i > 0; i--) {
                if (frames[i - 1].hdr.kind == RBP_KIND_RESPONSE &&
                    frames[i - 1].hdr.opcode == RBP_OP_KEY_CATALOG &&
                    frames[i - 1].hdr.request_id == g_req_id) {
                    cresp = &frames[i - 1];
                    break;
                }
            }
            CHECK(cresp != NULL && cresp->hdr.status == RBP_STATUS_OK);
            rbp_tlv_reader_init(&r, cresp->payload, cresp->len);
            const uint8_t *cat;
            uint16_t clen;
            uint8_t next_cursor = 255;
            CHECK(rbp_tlv_get_bytes(&r, 3, &cat, &clen, &found) == RBP_TLV_OK && found);
            rbp_tlv_reader_init(&r, cresp->payload, cresp->len);
            CHECK(rbp_tlv_get_u8(&r, 2, &next_cursor, &found) == RBP_TLV_OK && found);
            if (first_page) {
                CHECK(cat[0] == 12); /* page cap */
                CHECK(cat[1] == 0);  /* slot 0 */
                CHECK(cat[2] == 0x01 && cat[3] == 0x00); /* Power */
                first_page = false;
            }
            total = (uint8_t)(total + cat[0]);
            if (next_cursor == 255) break;
            cursor = next_cursor;
        }
        CHECK(total == 12);
    }

    /* events enable -> snapshot */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 620, RBP_OP_EVENTS_ENABLE, w.buf, w.len, 777);
    tick(s, 621);
    frame_t *snap = find_frame(RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV);
    CHECK(snap != NULL && snap->payload[20] == RBP_KEYS_KIND_SNAPSHOT);
    CHECK(snap->len == RBP_KEYS_STRUCT_SIZE);

    /* physical press */
    rbp_keys_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = RBP_KEYS_KIND_PHYSICAL;
    rep.pressed_bits = (1ULL << 7);
    rep.captured_us = 630000;
    rbp_server_on_keys(s, &rep);
    tick(s, 631);
    frame_t *ke = find_frame(RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV);
    /* find a PHYSICAL event (kind byte 1) */
    ke = NULL;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_KEYS_STATE_EV &&
            frames[i].payload[20] == RBP_KEYS_KIND_PHYSICAL)
            ke = &frames[i];
    }
    CHECK(ke != NULL);
    if (!ke) { server_free(); return; }
    uint64_t bits = 0;
    memcpy(&bits, ke->payload + 12, 8);
    CHECK(bits == (1ULL << 7));

    server_free();
}

/* request with explicit connection id */
static void client_request_conn(rbp_server_t *s, uint32_t now, uint16_t opcode,
                                const uint8_t *payload, uint16_t len,
                                uint32_t connection_id)
{
    g_req_id++;
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_REQUEST;
    h.session_id = g_session;
    h.request_id = g_req_id;
    h.opcode = opcode;
    h.connection_id = connection_id;
    uint8_t enabled_payload[128];
    if(opcode==RBP_OP_VOICE_ENABLE && len==5 && payload[4]) {
        rbp_tlv_writer_t v;rbp_tlv_writer_init(&v);uint8_t codecs[6]={1,0,0,0,1,0};
        rbp_tlv_put_bool(&v,1,true);rbp_tlv_put_bytes(&v,2,codecs,6);rbp_tlv_put_u32(&v,3,65536);
        memcpy(enabled_payload,v.buf,v.len);payload=enabled_payload;len=v.len;
    }
    client_send_raw(s, now, &h, payload, len);
}

static void test_voice_delivery(void)
{
    memset(&radio, 0, sizeof(radio));
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* device ready + voice ready */
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rec.auto_reconnect = true;
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    rbp_server_on_voice_state(s, RBP_VOICE_READY, RBP_VI_HTT, 16000);
    rbp_server_on_link(s, RBP_LINK_READY, 900, 40);
    tick(s, 41);

    /* voice enable */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 50, RBP_OP_VOICE_ENABLE, w.buf, w.len, 900);
    tick(s, 51);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_VOICE_ENABLE);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);

    /* device starts a stream */
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 60, &ev);
    tick(s, 61);
    frame_t *started = find_frame(RBP_KIND_EVENT, RBP_OP_VOICE_STARTED_EV);
    CHECK(started != NULL);

    /* 3 PCM blocks: 246 + 246 + 100 samples */
    feed_silence(s, 62, 246);
    tick(s, 63);
    feed_silence(s, 64, 246);
    tick(s, 65);
    feed_silence(s, 66, 100);
    tick(s, 67);
    /* flush media */
    tick(s, 68);
    tick(s, 69);
    tick(s, 70);

    /* count VOICE_DATA frames and verify continuity */
    uint64_t total_samples = 0;
    uint32_t expect_seq = 1;
    int data_frames = 0;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_DATA) {
            uint32_t fseq, sid;
            uint64_t idx;
            memcpy(&sid, frames[i].payload, 4);
            memcpy(&fseq, frames[i].payload + 4, 4);
            memcpy(&idx, frames[i].payload + 24, 8);
            uint32_t cnt;
            memcpy(&cnt, frames[i].payload + 32, 4);
            (void)sid;
            CHECK(fseq == expect_seq++);
            CHECK(idx == total_samples);
            total_samples += cnt;
            data_frames++;
        }
    }
    CHECK(data_frames >= 3);
    CHECK(total_samples == 246 + 246 + 100);

    /* normal end */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_END;
    ev.u.end.reason = RBP_END_NORMAL;
    feed_voice(s, 80, &ev);
    tick(s, 81);
    tick(s, 82);
    frame_t *ended = NULL;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_ENDED_EV) ended = &frames[i];
    }
    CHECK(ended != NULL);
    uint8_t end_reason = 255;
    uint64_t delivered = 0;
    CHECK(parse_ended(ended, &end_reason, &delivered));
    CHECK(end_reason == RBP_END_NORMAL);
    CHECK(delivered == total_samples);

    /* mid-stream link loss ordering: END -> keys reset -> DEVICE_STATE */
    size_t base = frame_count; /* only consider frames of this second stream */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 90, &ev);
    feed_silence(s, 91, 100);
    /* key events must be enabled for the link-lost RESET to be deliverable */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 91, RBP_OP_EVENTS_ENABLE, w.buf, w.len, 900);
    rbp_server_on_link(s, RBP_LINK_DISCONNECTED, 0, 92);
    tick(s, 93);
    tick(s, 94);
    tick(s, 95);
    int end_idx = -1, key_idx = -1, dev_idx = -1;
    for (size_t i = base; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_ENDED_EV && end_idx < 0) end_idx = (int)i;
        if (frames[i].hdr.opcode == RBP_OP_KEYS_STATE_EV &&
            frames[i].payload[20] == RBP_KEYS_KIND_RESET && key_idx < 0) key_idx = (int)i;
        if (frames[i].hdr.opcode == RBP_OP_DEVICE_STATE_EV && dev_idx < 0) {
            /* disconnected state: payload connection field is 0 */
            rbp_tlv_reader_t dr;
            rbp_tlv_reader_init(&dr, frames[i].payload, frames[i].len);
            uint32_t dconn = 0xFFFFFFFF;
            bool df = false;
            if (rbp_tlv_get_u32(&dr, 1, &dconn, &df) == RBP_TLV_OK && df &&
                dconn == 0)
                dev_idx = (int)i;
        }
    }
    CHECK(end_idx >= 0);
    CHECK(key_idx >= 0);
    CHECK(dev_idx >= 0);
    CHECK(end_idx < key_idx && key_idx < dev_idx);
    if (end_idx >= 0) {
        uint8_t r = 255;
        uint64_t d = 0;
        CHECK(parse_ended(&frames[end_idx], &r, &d));
        CHECK(r == RBP_END_LINK_LOST);
    }

    server_free();
}

static void test_voice_stop_and_disable(void)
{
    memset(&radio, 0, sizeof(radio));
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    rbp_server_on_voice_state(s, RBP_VOICE_READY, RBP_VI_HTT, 16000);
    rbp_server_on_link(s, RBP_LINK_READY, 900, 40);
    tick(s, 41);

    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 50, RBP_OP_VOICE_ENABLE, w.buf, w.len, 900);
    tick(s, 51);

    /* stream begins */
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 60, &ev);
    feed_silence(s, 61, 246);
    tick(s, 62);
    tick(s, 63);

    /* get the stream id from VOICE_STARTED (TLV 1:sid:u32) */
    frame_t *started = find_frame(RBP_KIND_EVENT, RBP_OP_VOICE_STARTED_EV);
    CHECK(started != NULL);
    uint32_t stream_id = 0;
    {
        rbp_tlv_reader_t sr;
        bool sf = false;
        rbp_tlv_reader_init(&sr, started->payload, started->len);
        CHECK(rbp_tlv_get_u32(&sr, 1, &stream_id, &sf) == RBP_TLV_OK && sf);
    }

    /* VOICE_STOP -> OK then ENDED(requested_stop) after source stops */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, stream_id);
    client_request_conn(s, 70, RBP_OP_VOICE_STOP, w.buf, w.len, 900);
    tick(s, 71);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_VOICE_STOP);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    CHECK(radio.voice_stop_calls == 1);

    /* remote confirms stop */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_END;
    ev.u.end.reason = RBP_END_NORMAL;
    feed_voice(s, 75, &ev);
    tick(s, 76);
    tick(s, 77);
    frame_t *ended = NULL;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_ENDED_EV) ended = &frames[i];
    }
    CHECK(ended != NULL);
    if (ended) {
        uint8_t r = 255;
        uint64_t d = 0;
        CHECK(parse_ended(ended, &r, &d));
        CHECK(r == RBP_END_REQUESTED_STOP);
    }

    /* disable mid-stream: ENDED(consumer_disabled) must precede the OK */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 90, &ev);
    feed_silence(s, 91, 100);
    tick(s, 92);
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, false);
    client_request_conn(s, 93, RBP_OP_VOICE_ENABLE, w.buf, w.len, 900);
    tick(s, 94);
    tick(s, 95);
    tick(s, 96);
    int end_idx = -1, ok_idx = -1;
    for (size_t i = 0; i < frame_count; i++) {
        /* both are "last wins": this test already holds an earlier
         * ENDED(requested_stop) and an earlier ENABLE response */
        if (frames[i].hdr.kind == RBP_KIND_EVENT &&
            frames[i].hdr.opcode == RBP_OP_VOICE_ENDED_EV)
            end_idx = (int)i;
        if (frames[i].hdr.kind == RBP_KIND_RESPONSE &&
            frames[i].hdr.opcode == RBP_OP_VOICE_ENABLE)
            ok_idx = (int)i;
    }
    CHECK(end_idx >= 0 && ok_idx >= 0);
    CHECK(end_idx < ok_idx); /* END before the deferred OK */
    if (end_idx >= 0) {
        uint8_t r = 255;
        uint64_t d = 0;
        CHECK(parse_ended(&frames[end_idx], &r, &d));
        CHECK(r == RBP_END_CONSUMER_DISABLED);
    }

    server_free();
}

static void test_session_security(void)
{
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* corrupt frame inside session -> teardown */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, 1);
    uint8_t wire_buf[RBP_MAX_ENCODED];
    rbp_header_t h;
    memset(&h, 0, sizeof(h));
    h.kind = RBP_KIND_REQUEST;
    h.session_id = g_session;
    h.request_id = 2;
    h.opcode = RBP_OP_PING;
    g_tx_seq++;
    rbp_txseq_t seq;
    seq.next_tx_seq = g_tx_seq;
    size_t n = rbp_frame_encode(&h, &seq, w.buf, w.len, wire_buf);
    wire_buf[n - 3] ^= 0xFF; /* corrupt crc byte */
    rbp_server_on_usb_rx(s, wire_buf, n, 20);
    tick(s, 21);
    rbp_server_get_info(s, &info);
    CHECK(!info.session_active);

    /* fresh hello works (old session id in header must not disturb) */
    client_reset();
    reset_frames();
    client_hello(s, 30);
    tick(s, 31);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_HELLO);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    rbp_server_get_info(s, &info);
    CHECK(info.session_active);
    uint32_t s1 = info.session_id;
    g_session = s1;

    /* unknown opcode -> UNSUPPORTED */
    rbp_header_t h2;
    memset(&h2, 0, sizeof(h2));
    h2.kind = RBP_KIND_REQUEST;
    h2.session_id = g_session;
    h2.request_id = 2;
    h2.opcode = 0x7FFF;
    client_send_raw(s, 40, &h2, NULL, 0);
    tick(s, 41);
    resp = NULL;
    for (size_t i = 0; i < frame_count; i++) {
        if (frames[i].hdr.kind == RBP_KIND_RESPONSE && frames[i].hdr.opcode == 0x7FFF)
            resp = &frames[i];
    }
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_UNSUPPORTED);

    /* tx_seq gap -> teardown */
    memset(&h2, 0, sizeof(h2));
    h2.kind = RBP_KIND_REQUEST;
    h2.session_id = g_session;
    h2.request_id = 3;
    h2.opcode = RBP_OP_PING;
    g_tx_seq += 5; /* creates a gap */
    client_send_raw(s, 45, &h2, w.buf, w.len);
    tick(s, 46);
    rbp_server_get_info(s, &info);
    CHECK(!info.session_active);

    /* version mismatch hello */
    client_reset();
    reset_frames();
    client_hello(s, 50);
    tick(s, 51);
    rbp_server_get_info(s, &info);
    g_session = info.session_id;
    /* patch the hello: easiest path is to send major=3 manually */
    {
        uint8_t nonce[16] = { 0 };
        rbp_tlv_writer_t wv;
        rbp_tlv_writer_init(&wv);
        rbp_tlv_put_bytes(&wv, 1, nonce, 16);
        /* hand-craft: rbp_frame_encode forces the current version, but this
         * probe needs major=3 on the wire */
        uint8_t raw[RBP_MAX_FRAME_RAW];
        rbp_header_t hv;
        memset(&hv, 0, sizeof(hv));
        hv.magic0 = RBP_MAGIC0;
        hv.magic1 = RBP_MAGIC1;
        hv.header_size = RBP_HEADER_SIZE;
        hv.kind = RBP_KIND_REQUEST;
        hv.major = 2;
        hv.minor = 0;
        hv.request_id = 1;
        hv.tx_seq = 1;
        hv.opcode = RBP_OP_HELLO;
        hv.payload_size = (uint16_t)wv.len;
        rbp_header_pack(&hv, raw);
        memcpy(raw + RBP_HEADER_SIZE, wv.buf, wv.len);
        uint32_t crc = rbp_crc32c(raw, RBP_HEADER_SIZE + wv.len);
        for (int i = 0; i < 4; i++)
            raw[RBP_HEADER_SIZE + wv.len + i] = (uint8_t)(crc >> (8 * i));
        size_t raw_len = RBP_HEADER_SIZE + (size_t)wv.len + 4;
        uint8_t wb[RBP_MAX_ENCODED];
        size_t nn = rbp_cobs_encode(raw, raw_len, wb);
        wb[nn] = 0x00;
        rbp_server_on_usb_rx(s, wb, nn + 1, 52);
        /* the VERSION_MISMATCH reply is written immediately (not queued);
         * collect it before tick() clears the output buffer */
        parse_pending();
        tick(s, 53);
        resp = NULL;
        for (size_t i = 0; i < frame_count; i++) {
            if (frames[i].hdr.opcode == RBP_OP_HELLO &&
                frames[i].hdr.status == RBP_STATUS_VERSION_MISMATCH)
                resp = &frames[i];
        }
        CHECK(resp != NULL);
        if (resp)
            CHECK(resp->hdr.tx_seq == 1 && resp->hdr.request_id == 1);
    }

    server_free();
}

static void test_heartbeat_expiry(void)
{
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 1000);
    client_hello(s, 1001);
    tick(s, 1002);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    CHECK(info.session_active);
    /* no requests for 5 s */
    tick(s, 6600);
    rbp_server_get_info(s, &info);
    CHECK(!info.session_active);
    server_free();
}

static void test_forget_flow(void)
{
    memset(&radio, 0, sizeof(radio));
    stored_valid = false;
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* bond a peer first */
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    tick(s, 13);

    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, 0x1234);
    client_request(s, 20, RBP_OP_FORGET_PEER, w.buf, w.len);
    tick(s, 21);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_FORGET_PEER);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_ACCEPTED);
    CHECK(radio.forget_calls == 1);
    rbp_server_on_forget_done(s, RBP_STATUS_OK, false);
    tick(s, 22);
    frame_t *op = find_frame(RBP_KIND_EVENT, RBP_OP_OPERATION_EV);
    CHECK(op != NULL);
    /* peer gone */
    rbp_tlv_writer_init(&w);
    client_request(s, 25, RBP_OP_GET_PEER, w.buf, 0);
    tick(s, 26);
    resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_GET_PEER);
    CHECK(resp != NULL);
    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    uint32_t pid = 0xFFFF;
    bool found = false;
    CHECK(rbp_tlv_get_u32(&r, 1, &pid, &found) == RBP_TLV_OK && found);
    CHECK(pid == 0);
    rbp_server_get_info(s, &info);
    CHECK(info.link_state == RBP_LINK_UNBOUND);

    server_free();
}

static void test_goodbye_and_store_fail(void)
{
    memset(&radio, 0, sizeof(radio));
    stored_valid = false;
    store_fail = false;
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* storage failure during pairing: uncertain operation */
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    store_fail = true;
    uint32_t before = test_rng();
    (void)before;
    /* need an active pair op for the operation event; emulate full flow */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u16(&w, 1, 100);
    client_request(s, 20, RBP_OP_FIND_START, w.buf, w.len);
    tick(s, 21);
    frame_t *resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_FIND_START);
    CHECK(resp != NULL);
    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    uint32_t sid = 0;
    bool found = false;
    CHECK(rbp_tlv_get_u32(&r, 1, &sid, &found) == RBP_TLV_OK && found);
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, sid);
    rbp_tlv_put_u32(&w, 2, 0xC1);
    client_request(s, 25, RBP_OP_PAIR_BEGIN, w.buf, w.len);
    tick(s, 26);
    rbp_server_on_link(s,RBP_LINK_READY,950,26);
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 950);
    tick(s, 27);
    store_fail = false;
    frame_t *op = find_frame(RBP_KIND_EVENT, RBP_OP_OPERATION_EV);
    CHECK(op != NULL);
    uint16_t result = 0;
    if (op) {
        rbp_tlv_reader_init(&r, op->payload, op->len);
        CHECK(rbp_tlv_get_u16(&r, 3, &result, &found) == RBP_TLV_OK && found);
    }
    CHECK(result == RBP_STATUS_STORAGE_FAILED);
    rbp_server_get_info(s,&info);CHECK(info.link_state==RBP_LINK_ERROR);
    for(size_t i=0;i<frame_count;i++)if(frames[i].hdr.opcode==RBP_OP_DEVICE_STATE_EV) {
        uint8_t state=255;rbp_tlv_reader_init(&r,frames[i].payload,frames[i].len);
        CHECK(rbp_tlv_get_u8(&r,3,&state,&found)==RBP_TLV_OK && state!=RBP_DEVSTATE_READY);
    }

    /* goodbye closes the session */
    rbp_tlv_writer_init(&w);
    client_request(s, 30, RBP_OP_GOODBYE, w.buf, 0);
    tick(s, 31);
    resp = find_frame(RBP_KIND_RESPONSE, RBP_OP_GOODBYE);
    CHECK(resp && resp->hdr.session_id==g_session);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    rbp_server_get_info(s, &info);
    CHECK(!info.session_active);

    server_free();
}

/* ---- audit round 2: spec conformance tests ---- */

/* STARTED event: TLV 1:stream_id, 2:rate */
static bool parse_started(const frame_t *f, uint32_t *sid)
{
    rbp_tlv_reader_t rd;
    rbp_tlv_reader_init(&rd, f->payload, f->len);
    bool f1 = false;
    return rbp_tlv_get_u32(&rd, 1, sid, &f1) == RBP_TLV_OK && f1;
}

static frame_t *find_frame_base(size_t base, uint8_t kind, uint16_t opcode)
{
    for (size_t i = base; i < frame_count; i++) {
        if (frames[i].hdr.kind == kind && frames[i].hdr.opcode == opcode)
            return &frames[i];
    }
    return NULL;
}

static bool any_frame_base(size_t base, uint8_t kind, uint16_t opcode)
{
    return find_frame_base(base, kind, opcode) != NULL;
}

static void test_find_stop_user_done(void)
{
    memset(&radio, 0, sizeof(radio));
    stored_valid = false;
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;

    /* FIND_START(200ms): response carries search_id, scan sees candidate 0xC1 */
    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u16(&w, 1, 200);
    client_request(s, 20, RBP_OP_FIND_START, w.buf, w.len);
    tick(s, 21);
    frame_t *resp = find_frame_base(0, RBP_KIND_RESPONSE, RBP_OP_FIND_START);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    uint32_t search_id = 0;
    bool f1 = false;
    rbp_tlv_reader_t r;
    rbp_tlv_reader_init(&r, resp->payload, resp->len);
    CHECK(rbp_tlv_get_u32(&r, 1, &search_id, &f1) == RBP_TLV_OK && f1);

    /* FIND_STOP: OK, then FIND_DONE(reason=1 user stop); scan stopped once */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, search_id);
    client_request(s, 22, RBP_OP_FIND_STOP, w.buf, w.len);
    tick(s, 23);
    resp = find_frame_base(0, RBP_KIND_RESPONSE, RBP_OP_FIND_STOP);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    frame_t *done = find_frame_base(0, RBP_KIND_EVENT, RBP_OP_FIND_DONE_EV);
    CHECK(done != NULL);
    if (done) {
        uint32_t sid2 = 0;
        uint8_t reason = 255;
        bool g1 = false, g2 = false;
        rbp_tlv_reader_init(&r, done->payload, done->len);
        CHECK(rbp_tlv_get_u32(&r, 1, &sid2, &g1) == RBP_TLV_OK && g1 && sid2 == search_id);
        CHECK(rbp_tlv_get_u8(&r, 2, &reason, &g2) == RBP_TLV_OK && g2);
        CHECK(reason == RBP_FIND_DONE_USER_STOP);
    }
    CHECK(radio.stop_find_calls == 1);

    /* second FIND_STOP: search already finished -> NOT_FOUND */
    size_t base = frame_count;
    client_request(s, 24, RBP_OP_FIND_STOP, w.buf, w.len);
    tick(s, 25);
    resp = find_frame_base(base, RBP_KIND_RESPONSE, RBP_OP_FIND_STOP);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_NOT_FOUND);

    /* candidates stay valid after user stop: PAIR_BEGIN is accepted */
    base = frame_count;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_u32(&w, 1, search_id);
    rbp_tlv_put_u32(&w, 2, 0xC1);
    client_request(s, 26, RBP_OP_PAIR_BEGIN, w.buf, w.len);
    tick(s, 27);
    resp = find_frame_base(base, RBP_KIND_RESPONSE, RBP_OP_PAIR_BEGIN);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_ACCEPTED);
    CHECK(radio.pair_begin_calls == 1);

    server_free();
}

static void test_events_disable_clears_queue(void)
{
    memset(&radio, 0, sizeof(radio));
    stored_valid = false;
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    rbp_server_on_link(s, RBP_LINK_READY, 900, 40);
    tick(s, 41);

    rbp_tlv_writer_t w;
    /* enable: OK then snapshot */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 50, RBP_OP_EVENTS_ENABLE, w.buf, w.len, 900);
    tick(s, 51);
    frame_t *resp = find_frame_base(0, RBP_KIND_RESPONSE, RBP_OP_EVENTS_ENABLE);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    frame_t *snap = find_frame_base(0, RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV);
    CHECK(snap != NULL && snap->payload[20] == RBP_KEYS_KIND_SNAPSHOT);

    /* a queued press is dropped when EVENTS_ENABLE(false) lands */
    rbp_keys_report_t rep;
    memset(&rep, 0, sizeof(rep));
    rep.kind = RBP_KEYS_KIND_PHYSICAL;
    rep.pressed_bits = 0x1;
    rep.captured_us = 520;
    rbp_server_on_keys(s, &rep);
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, false);
    client_request_conn(s, 53, RBP_OP_EVENTS_ENABLE, w.buf, w.len, 900);
    size_t base = frame_count;
    tick(s, 54);
    resp = find_frame_base(base, RBP_KIND_RESPONSE, RBP_OP_EVENTS_ENABLE);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    CHECK(!any_frame_base(base, RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV));

    /* a report while disabled updates state but emits nothing */
    rep.captured_us = 540;
    rbp_server_on_keys(s, &rep);
    base = frame_count;
    tick(s, 55);
    CHECK(!any_frame_base(base, RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV));

    /* re-enable: fresh snapshot reflects the tracked (pressed) state */
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 56, RBP_OP_EVENTS_ENABLE, w.buf, w.len, 900);
    base = frame_count;
    tick(s, 57);
    resp = find_frame_base(base, RBP_KIND_RESPONSE, RBP_OP_EVENTS_ENABLE);
    CHECK(resp != NULL && resp->hdr.status == RBP_STATUS_OK);
    snap = find_frame_base(base, RBP_KIND_EVENT, RBP_OP_KEYS_STATE_EV);
    CHECK(snap != NULL && snap->payload[20] == RBP_KEYS_KIND_SNAPSHOT);
    if (snap) {
        uint64_t bits = 0;
        memcpy(&bits, snap->payload + 12, 8);
        CHECK(bits == 0x1);
    }

    server_free();
}

static void test_capture_limit(void)
{
    memset(&radio, 0, sizeof(radio));
    g_test_capture_ms = 300; /* short limit for the test */
    rbp_server_t *s = server_new();
    g_test_capture_ms = 120000;
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    rbp_server_on_voice_state(s, RBP_VOICE_READY, RBP_VI_HTT, 16000);
    rbp_server_on_link(s, RBP_LINK_READY, 900, 40);
    tick(s, 41);

    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 50, RBP_OP_VOICE_ENABLE, w.buf, w.len, 900);
    tick(s, 51);

    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 60, &ev);
    feed_silence(s, 61, 100);
    tick(s, 62);
    CHECK(find_frame_base(0, RBP_KIND_EVENT, RBP_OP_VOICE_STARTED_EV) != NULL);

    /* past the capture deadline the firmware stops the source itself */
    tick(s, 400);
    CHECK(radio.voice_stop_calls == 1);

    /* remote confirms with a (normal) END: reported as capture_limit */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_END;
    ev.u.end.reason = RBP_END_NORMAL;
    feed_voice(s, 410, &ev);
    size_t base = frame_count;
    tick(s, 411);
    tick(s, 412);
    frame_t *ended = find_frame_base(base, RBP_KIND_EVENT, RBP_OP_VOICE_ENDED_EV);
    CHECK(ended != NULL);
    if (ended) {
        uint8_t reason = 255;
        uint64_t delivered = 0;
        CHECK(parse_ended(ended, &reason, &delivered));
        CHECK(reason == RBP_END_CAPTURE_LIMIT);
        CHECK(delivered == 100);
    }

    server_free();
}

static void test_fast_restart_overrun(void)
{
    memset(&radio, 0, sizeof(radio));
    rbp_server_t *s = server_new();
    client_reset();
    reset_frames();
    tick(s, 10);
    client_hello(s, 11);
    tick(s, 12);
    rbp_server_info_t info;
    rbp_server_get_info(s, &info);
    g_session = info.session_id;
    rbp_peer_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.peer_id = 0x1234;
    strcpy(rec.name, "R");
    rbp_server_on_pair_done(s, RBP_STATUS_OK, true, &rec, 900);
    rbp_server_on_voice_state(s, RBP_VOICE_READY, RBP_VI_HTT, 16000);
    rbp_server_on_link(s, RBP_LINK_READY, 900, 40);
    tick(s, 41);

    rbp_tlv_writer_t w;
    rbp_tlv_writer_init(&w);
    rbp_tlv_put_bool(&w, 1, true);
    client_request_conn(s, 50, RBP_OP_VOICE_ENABLE, w.buf, w.len, 900);
    tick(s, 51);

    /* first utterance starts and delivers audio */
    rbp_voice_evt_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 60, &ev);
    feed_silence(s, 61, 100);
    tick(s, 62);
    frame_t *started = find_frame_base(0, RBP_KIND_EVENT, RBP_OP_VOICE_STARTED_EV);
    CHECK(started != NULL);
    uint32_t sid_a = 0;
    CHECK(parse_started(started, &sid_a));

    /* user presses again before the first END: old stream ends as
     * buffer_overrun, then the new STARTED follows - no lost utterance */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_START;
    ev.u.start.sample_rate = 16000;
    feed_voice(s, 70, &ev);
    size_t base = frame_count;
    tick(s, 71);
    int ended_idx = -1, started_idx = -1;
    for (size_t i = base; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_ENDED_EV && ended_idx < 0)
            ended_idx = (int)i;
        if (frames[i].hdr.opcode == RBP_OP_VOICE_STARTED_EV &&
            started_idx < 0) {
            uint32_t sid = 0;
            if (parse_started(&frames[i], &sid) && sid != sid_a)
                started_idx = (int)i;
        }
    }
    CHECK(ended_idx >= 0 && started_idx > ended_idx);
    if (ended_idx >= 0) {
        uint8_t reason = 255;
        uint64_t delivered = 0;
        CHECK(parse_ended(&frames[ended_idx], &reason, &delivered));
        CHECK(reason == RBP_END_BUFFER_OVERRUN);
        CHECK(delivered == 100);
        uint32_t end_sid = 0;
        rbp_tlv_reader_t r;
        rbp_tlv_reader_init(&r, frames[ended_idx].payload, frames[ended_idx].len);
        bool f1 = false;
        CHECK(rbp_tlv_get_u32(&r, 1, &end_sid, &f1) == RBP_TLV_OK && f1);
        CHECK(end_sid == sid_a);
    }

    /* subsequent audio belongs to the new stream only */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_ENCODED;
    ev.u.encoded.data = encoded_block;
    ev.u.encoded.len=25;ev.u.encoded.unit_size=25;ev.u.encoded.samples=50;
    feed_voice(s, 72, &ev);
    base = frame_count;
    tick(s, 73);
    uint32_t sid_b = 0;
    for (size_t i = base; i < frame_count; i++) {
        if (frames[i].hdr.opcode == RBP_OP_VOICE_DATA) {
            memcpy(&sid_b, frames[i].payload, 4);
            CHECK(sid_b != sid_a);
        }
    }
    CHECK(sid_b != 0 && sid_b != sid_a);

    /* normal end of the second utterance */
    memset(&ev, 0, sizeof(ev));
    ev.type = RBP_VOICE_EVT_END;
    ev.u.end.reason = RBP_END_NORMAL;
    feed_voice(s, 80, &ev);
    base = frame_count;
    tick(s, 81);
    tick(s, 82);
    frame_t *ended = find_frame_base(base, RBP_KIND_EVENT, RBP_OP_VOICE_ENDED_EV);
    CHECK(ended != NULL);
    if (ended) {
        uint8_t reason = 255;
        uint64_t delivered = 0;
        uint32_t end_sid = 0;
        CHECK(parse_ended(ended, &reason, &delivered));
        CHECK(reason == RBP_END_NORMAL);
        rbp_tlv_reader_t r;
        rbp_tlv_reader_init(&r, ended->payload, ended->len);
        bool f1 = false;
        CHECK(rbp_tlv_get_u32(&r, 1, &end_sid, &f1) == RBP_TLV_OK && f1);
        CHECK(end_sid == sid_b);
    }

    server_free();
}

static void test_voice_backpressure_boundaries(void)
{
    rbp_server_t*s=server_new();client_reset();reset_frames();
    client_hello(s,11);tick(s,12);rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
    rbp_server_on_link(s,RBP_LINK_READY,900,40);rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);tick(s,41);
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,true);
    client_request_conn(s,50,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);tick(s,51);
    sink_blocked=true;rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;
    feed_voice(s,60,&ev);tick(s,61);
    int16_t pcm[100];for(unsigned i=0;i<100;i++)pcm[i]=(int16_t)i;
    for(unsigned j=0;j<10;j++) {
        if(j==5||j==8){ev.type=RBP_VOICE_EVT_FORMAT;ev.u.format.sample_rate=j==5?8000:16000;feed_voice(s,62+j,&ev);}
        ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=(const uint8_t*)pcm;ev.u.encoded.len=50;ev.u.encoded.unit_size=50;ev.u.encoded.samples=100;feed_voice(s,62+j,&ev);tick(s,62+j);
    }
    ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=RBP_END_NORMAL;feed_voice(s,75,&ev);
    for(unsigned t=76;t<100;t++)tick(s,t); /* retries must consume no sequence */
    sink_blocked=false;for(unsigned t=100;t<110;t++)tick(s,t);
    uint64_t total=0;uint32_t audio_seq=0;unsigned formats=0,ended=0;
    for(size_t i=0;i<frame_count;i++) {
        frame_t*f=&frames[i];CHECK(f->crc_ok);
        if(i)CHECK(f->hdr.tx_seq==frames[i-1].hdr.tx_seq+1);
        if(f->hdr.opcode==RBP_OP_VOICE_DATA){uint32_t seq;uint64_t index;uint32_t n;memcpy(&seq,f->payload+4,4);memcpy(&index,f->payload+24,8);memcpy(&n,f->payload+32,4);CHECK(seq==++audio_seq&&index==total);total+=n;}
        if(f->hdr.opcode==RBP_OP_VOICE_FORMAT_EV){rbp_tlv_reader_t r;bool found;uint64_t boundary;rbp_tlv_reader_init(&r,f->payload,f->len);CHECK(rbp_tlv_get_u64(&r,9,&boundary,&found)==RBP_TLV_OK&&found);CHECK(boundary==total&&boundary==(formats?800:500));formats++;}
        if(f->hdr.opcode==RBP_OP_VOICE_ENDED_EV){uint8_t reason;uint64_t delivered;CHECK(parse_ended(f,&reason,&delivered));CHECK(reason==0&&delivered==total&&total==1000);ended++;}
    }
    CHECK(total==1000&&formats==2&&ended==1);server_free();
}

static void test_pair_teardown_once(void) {
    memset(&radio,0,sizeof radio);stored_valid=false;scan_submit_status=0;
    rbp_server_t *s=server_new();client_reset();reset_frames();
    client_hello(s,1);tick(s,2);
    rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_u16(&w,1,300);
    client_request(s,10,RBP_OP_FIND_START,w.buf,w.len);tick(s,11);
    frame_t *resp=find_frame(RBP_KIND_RESPONSE,RBP_OP_FIND_START);
    CHECK(resp && resp->hdr.status==RBP_STATUS_OK);
    if(!resp){server_free();return;}
    rbp_tlv_reader_t rd;rbp_tlv_reader_init(&rd,resp->payload,resp->len);
    uint32_t search=0;bool found=false;
    CHECK(rbp_tlv_get_u32(&rd,1,&search,&found)==RBP_TLV_OK && found);
    tick(s,400);
    rbp_tlv_writer_init(&w);rbp_tlv_put_u32(&w,1,search);rbp_tlv_put_u32(&w,2,0xC1);
    client_request(s,401,RBP_OP_PAIR_BEGIN,w.buf,w.len);tick(s,402);
    CHECK(radio.pair_begin_calls==1);
    rbp_server_on_pair_prompt(s,RBP_PROMPT_ENTER_PASSKEY,0,30000);
    /* Cancel twice while the asynchronous backend has not completed,
     * then destroy the session: all paths share one cancellation owner. */
    rbp_tlv_writer_init(&w);rbp_tlv_put_u32(&w,1,g_req_id);
    client_request(s,403,RBP_OP_PAIR_CANCEL,w.buf,w.len);
    client_request(s,404,RBP_OP_PAIR_CANCEL,w.buf,w.len);
    rbp_server_on_usb_gone(s,405);
    CHECK(radio.pair_cancel_calls==1); /* operation + prompt are one owner */
    rbp_server_on_usb_gone(s,404);CHECK(radio.pair_cancel_calls==1);
    server_free();
}

static void test_teardown_reentry(void) {
    memset(&radio,0,sizeof radio);
    rbp_server_t *s=server_new();client_reset();reset_frames();
    client_hello(s,1);tick(s,2);
    rbp_voice_evt_t begin={0};begin.type=RBP_VOICE_EVT_SOURCE_BEGIN;
    rbp_server_on_voice(s,&begin,3);
    teardown_reenter=true;
    rbp_server_on_usb_gone(s,10);
    CHECK(radio.voice_stop_calls==1);
    rbp_server_on_usb_gone(s,11);
    CHECK(radio.voice_stop_calls==1);
    server_free();
}

static void test_voice_burst_without_tick(void);
#include "test_audio_product.inc"
#include "../firmware/product/faults.h"
static void test_release_fault_rpc(void) {
    rbp_server_t*s=setup_audio_regression();
    rbp_fault_record(RBP_FAULT_SDK,0x86,0x12345678,0);
    client_request(s,52,RBP_OP_GET_STATS,NULL,0);tick(s,53);
    frame_t*f=find_frame(RBP_KIND_RESPONSE,RBP_OP_GET_STATS);CHECK(f!=NULL);
    if(f) {
        const uint8_t *data;uint16_t len;bool found;rbp_tlv_reader_t r;
        rbp_tlv_reader_init(&r,f->payload,f->len);
        CHECK(rbp_tlv_get_bytes(&r,7,&data,&len,&found)==RBP_TLV_OK&&found&&len>=24&&len%24==0);
        if(found&&len>=24) {
            const uint8_t *last=data+len-24;
            CHECK(last[4]==RBP_FAULT_SDK&&last[6]==0x86);
            CHECK(last[8]==0x78&&last[9]==0x56&&last[10]==0x34&&last[11]==0x12);
        }
    }
    server_free();
}

int main(void)
{
    test_release_fault_rpc();
    test_voice_burst_without_tick();
    test_voice_fragment_epoch_unknown();
    test_audio_fault_outcomes();
#ifdef RBP_BURST_TEST_ONLY
    if(failures)return 1;
    puts("Small-ring encoded burst: exact bytes, exact data/order, normal end, no timer dependency passed");
    return 0;
#endif
    test_pair_teardown_once();
    test_teardown_reentry();
    test_hello_ping();
    test_find_pair_keys();
    test_voice_delivery();
    test_voice_resume_intent();
    test_host_voice_start();
    test_voice_stop_and_disable();
    test_find_stop_user_done();
    test_events_disable_clears_queue();
    test_capture_limit();
    test_fast_restart_overrun();
    test_session_security();
    test_heartbeat_expiry();
    test_forget_flow();
    test_goodbye_and_store_fail();
    test_voice_backpressure_boundaries();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("product server C tests: all passed\n");
    return 0;
}

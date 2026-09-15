/* Remote Bridge host simulator.
 *
 * Emulates the CH582F bridge end-to-end on the host: product server +
 * RC003 adapter + fake remote + mock radio.  Two TCP ports:
 *
 *   data port    (default 45731): the "USB CDC" byte stream a client talks
 *                RBP/3.0 over;
 *   control port (default 45732): text commands to script the fake remote
 *                (press home / mic_on / battery 55 / drop_link / ...).
 *
 * Usage: sim_bridge [--data-port P] [--control-port P] [--speed F]
 *                   [--ticks N] [--quiet]
 *   --ticks N  run N virtual milliseconds then exit (used by tests)
 *   --speed F  virtual time acceleration (default 1.0 = real time)
 */
#include "sim_remote.h"
#include "device_model.h"
#include "rbp/frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define CLOSESOCK close
typedef int SOCKET;
#define SOCKET_ERROR (-1)
#define INVALID_SOCKET (-1)
#endif

/* ---------------- wiring ---------------- */

static int g_quiet = 0;

static sim_t g_sim;
static rbp_server_t *g_srv;

/* transport sink: accumulate into a dynamic buffer, flush to data socket */
#define TX_CAP (64 * 1024)
static uint8_t g_tx[TX_CAP];
static size_t g_tx_len;
static SOCKET g_data_sock = INVALID_SOCKET;

static size_t tx_sink(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    if (len > TX_CAP - g_tx_len) len = TX_CAP - g_tx_len;
    memcpy(g_tx + g_tx_len, data, len);
    g_tx_len += len;
    return len;
}

static void tx_flush(void)
{
    if (g_tx_len == 0) return;
    if (g_data_sock == INVALID_SOCKET) {
        g_tx_len = 0; /* no client: drop (like an unplugged USB) */
        return;
    }
    int n = send(g_data_sock, (const char *)g_tx, (int)g_tx_len, 0);
    if (n <= 0) {
        g_tx_len = 0;
        return;
    }
    memmove(g_tx, g_tx + n, g_tx_len - (size_t)n);
    g_tx_len -= (size_t)n;
}

static uint32_t g_rng_state = 0x12345678u;
static uint32_t sim_rng(void)
{
    /* xorshift32 - deterministic when SIM_SEED set */
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static bool store_load(void *user, rbp_peer_record_t *out)
{
    (void)user;
    FILE *f = fopen("sim_store.bin", "rb");
    if (!f) return false;
    bool ok = fread(out, 1, sizeof(*out), f) == sizeof(*out);
    fclose(f);
    if (ok && out->peer_id == 0) ok = false;
    return ok;
}

static bool store_save(void *user, const rbp_peer_record_t *rec)
{
    (void)user;
    FILE *f = fopen("sim_store.bin", "wb");
    if (!f) return false;
    size_t n = fwrite(rec, 1, sizeof(*rec), f);
    fclose(f);
    return n == sizeof(*rec);
}

static bool store_clear(void *user)
{
    (void)user;
    rbp_peer_record_t empty={0};
    return store_save(NULL,&empty);
}

uint32_t sim_allocate_peer_id(void) {
    uint32_t n=0;FILE *f=fopen("sim_counter.bin","rb");
    if(f){bool ok=fread(&n,1,4,f)==4;fclose(f);if(!ok)return 0;}
    if(n==UINT32_MAX)return 0;n++;
    f=fopen("sim_counter.bin","wb");if(!f)return 0;
    bool ok=fwrite(&n,1,4,f)==4;if(fclose(f)!=0)ok=false;
    return ok?n:0;
}

static void on_event(void *user, const char *line)
{
    (void)user;
    if (!g_quiet) printf("[sim] %s\n", line);
}



int main(int argc, char **argv)
{
    int data_port = 45731, control_port = 45732;
    double speed = 1.0;
    long long ticks = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data-port") && i + 1 < argc) data_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--control-port") && i + 1 < argc) control_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g_rng_state = (uint32_t)atoll(argv[++i]);
    }
    if (speed <= 0) speed = 1.0;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* product server */
    g_srv = (rbp_server_t *)malloc(rbp_server_object_size());
    rbp_server_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = *sim_radio_backend();
    cfg.store.load = store_load;
    cfg.store.save = store_save;
    cfg.store.clear = store_clear;
    cfg.out = tx_sink;
    cfg.rng = sim_rng;
    cfg.profile = &RBP_PROFILE_RC003;
    cfg.max_keys = 64;
    cfg.max_capture_ms = 120000;
    for (int i = 0; i < 16; i++) cfg.bridge_uid[i] = (uint8_t)(0xA0 + i);
    cfg.firmware_version = "sim 1.0.0";
#ifdef RBP_DEBUG
    cfg.firmware_version = "sim-debug 1.0.0";
#endif
    cfg.reset_reason = 1;
    rbp_server_init(g_srv, &cfg, NULL, 0);
    sim_init(&g_sim, g_srv);
    rbp_peer_record_t restored;
    g_sim.remote_bonded=store_load(NULL,&restored);
    g_sim.on_event = on_event;

    /* listen sockets */
    SOCKET data_listen = socket(AF_INET, SOCK_STREAM, 0);
    SOCKET ctrl_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int opt = 1;
#ifdef _WIN32
    setsockopt(data_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(data_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    addr.sin_port = htons((uint16_t)data_port);
    if (bind(data_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind data port failed\n");
        return 1;
    }
    listen(data_listen, 1);
    addr.sin_port = htons((uint16_t)control_port);
    if (bind(ctrl_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind control port failed\n");
        return 1;
    }
    listen(ctrl_listen, 1);

    SOCKET ctrl_sock = INVALID_SOCKET;
    char ctrl_buf[256];
    size_t ctrl_len = 0;

    if (!g_quiet) printf("[sim] ready: data=%d control=%d\n", data_port, control_port);

    uint32_t virt_ms = 0;
    double acc = 0;
    long long guard = 0;
    /* real wall-clock reference: select()'s timeout remainder is NOT elapsed
     * time, so measure actual deltas instead */
    FILETIME ft0;
    GetSystemTimeAsFileTime(&ft0);
    long long last_us = (long long)(((unsigned long long)ft0.dwHighDateTime << 32) |
                                    ft0.dwLowDateTime) / 10;

    while (ticks < 0 || virt_ms < (uint32_t)ticks) {
        if (++guard > 100000000LL) break;

        /* accept connections (non-blocking-ish via select with 1ms) */
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(data_listen, &rf);
        FD_SET(ctrl_listen, &rf);
        SOCKET mx = (data_listen > ctrl_listen ? data_listen : ctrl_listen);
        if (g_data_sock != INVALID_SOCKET) {
            FD_SET(g_data_sock, &rf);
            if (g_data_sock > mx) mx = g_data_sock;
        }
        if (ctrl_sock != INVALID_SOCKET) {
            FD_SET(ctrl_sock, &rf);
            if (ctrl_sock > mx) mx = ctrl_sock;
        }
        struct timeval tv = { 0, 1000 }; /* 1 ms */
        if (select((int)mx + 1, &rf, NULL, NULL, &tv) < 0) break;

        if (FD_ISSET(data_listen, &rf)) {
            SOCKET c = accept(data_listen, NULL, NULL);
            if (c != INVALID_SOCKET) {
                if (g_data_sock != INVALID_SOCKET) CLOSESOCK(g_data_sock);
                g_data_sock = c;
                if (!g_quiet) printf("[sim] data client connected\n");
            }
        }
        if (g_data_sock != INVALID_SOCKET && FD_ISSET(g_data_sock, &rf)) {
            uint8_t buf[512];
            int n = recv(g_data_sock, (char *)buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSESOCK(g_data_sock);
                g_data_sock = INVALID_SOCKET;
                rbp_server_on_usb_gone(g_srv, virt_ms);
                if (!g_quiet) printf("[sim] data client gone\n");
            } else {
                rbp_server_on_usb_rx(g_srv, buf, (size_t)n, virt_ms);
            }
        }
        if (FD_ISSET(ctrl_listen, &rf)) {
            SOCKET c = accept(ctrl_listen, NULL, NULL);
            if (c != INVALID_SOCKET) {
                if (ctrl_sock != INVALID_SOCKET) CLOSESOCK(ctrl_sock);
                ctrl_sock = c;
            }
        }
        if (ctrl_sock != INVALID_SOCKET && FD_ISSET(ctrl_sock, &rf)) {
            int n = recv(ctrl_sock, ctrl_buf + ctrl_len, (int)(sizeof(ctrl_buf) - ctrl_len - 1), 0);
            if (n <= 0) {
                CLOSESOCK(ctrl_sock);
                ctrl_sock = INVALID_SOCKET;
                ctrl_len = 0;
            } else {
                ctrl_len += (size_t)n;
                ctrl_buf[ctrl_len] = 0;
                char *nl;
                while ((nl = strchr(ctrl_buf, '\n')) != NULL) {
                    *nl = 0;
                    for (char *p = ctrl_buf; *p; p++)
                        if (*p == '\r') *p = 0;
                    if (ctrl_buf[0]) {
                        if (!g_quiet) printf("[sim] cmd: %s\n", ctrl_buf);
                        if (!sim_command(&g_sim, ctrl_buf))
                            fprintf(stderr, "[sim] bad cmd: %s\n", ctrl_buf);
                    }
                    size_t used = (size_t)(nl - ctrl_buf) + 1;
                    memmove(ctrl_buf, nl + 1, ctrl_len - used);
                    ctrl_len -= used;
                    ctrl_buf[ctrl_len] = 0;
                }
                if (ctrl_len == sizeof(ctrl_buf) - 1) ctrl_len = 0;
            }
        }

        /* advance virtual time by the real elapsed wall time */
        FILETIME ft1;
        GetSystemTimeAsFileTime(&ft1);
        long long now_us = (long long)(((unsigned long long)ft1.dwHighDateTime << 32) |
                                       ft1.dwLowDateTime) / 10;
        acc += (double)(now_us - last_us) / 1000.0 * speed;
        last_us = now_us;
        while (acc >= 1.0) {
            virt_ms++;
            acc -= 1.0;
            sim_tick(&g_sim, virt_ms);
            rbp_server_tick(g_srv, virt_ms);
            if (virt_ms % 4 == 0) tx_flush();
        }
    }

    tx_flush();
    if (!g_quiet) printf("[sim] done at %u ms\n", virt_ms);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

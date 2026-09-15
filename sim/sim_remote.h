/* Host simulator: fake RC003 remote + mock radio backend.
 *
 * The fake remote implements the ATT server subset the adapter uses and can
 * be scripted (key presses, ATVV streams, battery, link failures) through a
 * small command interface.  The radio mock implements rbp_radio_backend_t
 * against the product server.
 */
#ifndef SIM_REMOTE_H
#define SIM_REMOTE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rbp_server.h"
#include "rc003_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim sim_t;

struct sim {
    rbp_server_t *server;
    rc003_adapter_t adapter;

    /* fake remote state */
    bool remote_powered;
    bool remote_bonded;
    bool remote_connectable;   /* advertising */
    bool link_up;
    bool mic_on;

    /* pairing script */
    bool pairing_should_fail;
    bool pairing_requires_passkey;
    uint32_t pairing_passkey;  /* when requires_passkey */
    bool pair_in_progress;
    bool pair_initializing;
    uint32_t pair_peer_id;
    uint32_t pair_done_ms;     /* 0 = no scheduled completion */
    uint16_t pair_result;

    /* voice script */
    bool atvv_stream_active;
    uint8_t atvv_stream_id;
    uint16_t atvv_frame_counter;
    uint32_t atvv_next_frame_ms;
    uint32_t atvv_adpcm_timer;
    int16_t atvv_phase;        /* tone generator */
    bool atvv_drop_sync;       /* behavior flags for fault injection */
    bool atvv_send_sync;
    uint32_t atvv_sample_rate;
    uint32_t atvv_stop_ms;     /* scheduled auto stop */
    uint16_t atvv_frames_since_sync;
    int16_t atvv_phase_pred;
    uint8_t atvv_phase_step;
    bool battery_pending;

    /* link script */
    uint32_t drop_link_ms;
    uint32_t now_ms;

    /* ATT gattp state */
    uint8_t inq_buf[64];
    uint16_t kb_report_state; /* currently "pressed" keycode (0 = none) */
    uint8_t consumer_state[3];

    bool subscribed_kb, subscribed_consumer, subscribed_bat, subscribed_ctl,
        subscribed_audio;

    uint8_t battery_level;

    /* last "usb out" -> not needed here */

    /* user hooks for the harness */
    void (*on_event)(void *user, const char *line);
    void *event_user;
};

/* transport for server output; returns bytes accepted */
void sim_init(sim_t *s, rbp_server_t *server);
void sim_tick(sim_t *s, uint32_t now_ms);

/* scripted commands (text protocol, same as the control socket) */
bool sim_command(sim_t *s, const char *line);

/* feed raw ATT pdu from the "remote" side (i.e., adapter requests arrive
 * through the gatt client below); this is internal but exposed for tests */
void sim_att_inject(sim_t *s, const uint8_t *pdu, uint16_t len);

/* notification emission toward the adapter (internal) */
void sim_remote_notify(sim_t *s, uint16_t value_handle, const uint8_t *data, uint16_t len);

/* radio backend wiring for the product server */
const rbp_radio_backend_t *sim_radio_backend(void);

#ifdef __cplusplus
}
#endif
#endif

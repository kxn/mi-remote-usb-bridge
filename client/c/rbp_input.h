/* Host logical-key reducer. No transport/RPC ownership; supply KEY_CATALOG
 * in slot order once per source/revision. Callback data is borrowed.
 * Snapshots never invent physical presses. Serialize calls, do not reenter. */
#ifndef RBP_INPUT_H
#define RBP_INPUT_H
#include <stdint.h>
#include <stdbool.h>
#include "rbp/defs.h"
typedef struct {
    uint64_t receiver_id; /* application-assigned identity of this USB receiver */
    uint32_t session_id,peer_id,connection_id,catalog_revision;
    char model_id[41]; /* stable compatibility profile, UTF-8 with terminator */
} rbp_input_source_t;
enum {RBP_INPUT_DOWN=1,RBP_INPUT_UP,RBP_INPUT_SNAPSHOT,RBP_INPUT_RESET};
typedef struct {
    rbp_input_source_t source;
    uint64_t captured_us;
    uint32_t input_seq;
    uint16_t key_id;
    uint8_t kind,reason;
    bool synthetic;
} rbp_input_event_t;
typedef void (*rbp_input_emit_t)(void*,const rbp_input_event_t*);
typedef struct {
    rbp_input_source_t source;
    uint16_t key_ids[64];
    uint64_t bits;
    uint32_t sequence;
    uint8_t count;
    bool ready,have_sequence;
    rbp_input_emit_t emit;
    void *user;
} rbp_input_t;
void rbp_input_init(rbp_input_t*,rbp_input_emit_t,void*);
bool rbp_input_install(rbp_input_t*,const rbp_input_source_t*,const uint16_t *slot_ids,unsigned count);
bool rbp_input_feed(rbp_input_t*,uint32_t sequence,uint64_t captured_us,uint64_t bits,uint8_t kind,uint8_t reason);
void rbp_input_reset(rbp_input_t*,uint8_t reason);
bool rbp_input_has_key(const rbp_input_t*,uint16_t key_id);
#endif

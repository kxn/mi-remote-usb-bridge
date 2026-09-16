/* Optional host-only ICO decoder. See README.md for build and source provenance. */
#ifndef RBP_ICO_H
#define RBP_ICO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *ico_create(void);
void ico_destroy(void *state);
int ico_restart(void *state);
/* Exactly one 40-byte coding unit, output capacity >=320 int16 samples.
 * Returns 320 or -1 on invalid arguments. Preserve state between units;
 * restart for each new stream/epoch. Serialize calls to the reference codec. */
int ico_decode(void *state,const uint8_t *frame,int len,int16_t *pcm);
#ifdef __cplusplus
}
#endif
#endif

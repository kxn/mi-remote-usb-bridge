/* Small C ABI around the same implementation used by the offline demo. */
#define ICO_LIBRARY
#include "ico_decoder.c"
#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif
API void *ico_create(void) {
    IcoState *s = malloc(sizeof(*s));
    if (s) ico_reset(s);
    return s;
}
API void ico_destroy(void *s) { free(s); }
/* Caller-owned storage for the generic rbp_decoder API, no heap allocation. */
API int ico_init_buffer(void *s,unsigned bytes) {
    if(!s || bytes<sizeof(IcoState))return -1;
    ico_reset(s);return 0;
}
API int ico_restart(void *s) {
    if (!s) return -1;
    ico_reset(s);
    return 0;
}
API int ico_decode(void *s, const uint8_t *frame, int len, int16_t *pcm) {
    if (!s || !frame || !pcm || len != ICO_FRAME_BYTES) return -1;
    ico_decode_frame(s, frame, pcm);
    return 320;
}

/* Diagnostic-only observer for the SHA-pinned WCH archive. See
 * docs/sdk-rx-probe.md for the disassembly/ABI evidence and limits.
 * The real function receives the original four ABI arguments exactly once.
 * Do not change its result or read the received payload here.
 */
#include "sdk_rx_probe.h"
#include "../debug/trace.h"
#ifndef RBP_SDK_RX_PROBE
#error SDK RX observer must only be compiled in the explicit diagnostic build
#endif
extern volatile uint8_t gBleIPPara[]; /* SDK-owned; observer never writes it. */
extern uint32_t __real_ble_ll_chkcrc(void *, uint32_t, void *, void *);
extern uint32_t __real_AES_DevPktDec(uint32_t, void *);
extern volatile uint32_t *gptrAESReg;
extern volatile uint32_t *gptrBBReg;
static uint32_t checks, crc_failed;
/* These wrappers and poll run in cooperative task context in this central
 * build. BB/LLE ISRs do not call them. No logging/IRQ masking in the receive
 * check: save a bounded snapshot and emit later from the application task. */
typedef struct {
    uint32_t check, result, encrypted, before;
    uint32_t aes, bb, after, caller;
    uint32_t header, tail, arm, arm_state;
} rx_snapshot;
static rx_snapshot pending[4];
static uint32_t arms, arm_state, snapshot_dropped, reported_dropped;
static uint8_t head, count;
#ifdef __riscv
#define RX_OBSERVER_CODE __attribute__((section(".highcode"),noinline))
#else
#define RX_OBSERVER_CODE
#endif

RX_OBSERVER_CODE
uint32_t __wrap_AES_DevPktDec(uint32_t direction, void *connection)
{
    uint32_t result=__real_AES_DevPktDec(direction,connection);
    ++arms;
    arm_state=(gBleIPPara[6] & 255u) | ((direction & 255u)<<8);
    return result;
}

RX_OBSERVER_CODE
uint32_t __wrap_ble_ll_chkcrc(void *packet, uint32_t encrypted,
                              void *rssi, void *frequency)
{
    uint8_t before = gBleIPPara[6];
    uint32_t result = __real_ble_ll_chkcrc(packet, encrypted, rssi, frequency);
    ++checks;
    if (result & 1u) ++crc_failed;
    if (result & 2u) {
        if(count==4) { ++snapshot_dropped; return result; }
        rx_snapshot *s=&pending[(head+count)%4];
        s->check=checks;s->result=result;s->encrypted=encrypted;s->before=before;
        /* Post-return observations, NOT atomic pre-check hardware snapshots.
         * Both registers are repeatedly read by the SDK itself; no writes. */
        s->aes=gptrAESReg[0];s->bb=gptrBBReg[14];s->after=gBleIPPara[6];
        s->caller=(uint32_t)(uintptr_t)__builtin_return_address(0);
        s->header=0xffffffffu;s->tail=0xffffffffu;
        /* Original nonempty encrypted-packet checks already read these bytes.
         * Capture only the LL header/length and the checked status byte: never
         * key material, nonce, MIC bytes, or voice payload. */
        if(packet && encrypted) {
            const volatile uint8_t *p=packet;
            s->header=p[0]|((uint32_t)p[1]<<8);
            if(result==6 || result==14)s->tail=p[(uint32_t)p[1]+9];
        }
        s->arm=arms;s->arm_state=arm_state;
        ++count;
    }
    return result;
}

void sdk_rx_probe_poll(void)
{
    if(count) {
        const rx_snapshot *s=&pending[head];
        DT(DT_SYS,DT_ERROR,5,s->result,s->encrypted,s->before,s->check);
        DT(DT_SYS,DT_ERROR,6,s->check,s->aes,s->bb,s->after);
        DT(DT_SYS,DT_ERROR,7,s->check,s->header,s->tail,s->caller);
        DT(DT_SYS,DT_ERROR,8,s->check,s->arm,s->arm_state,crc_failed);
        head=(head+1)%4;--count;
    }
    if(reported_dropped!=snapshot_dropped) {
        DT(DT_SYS,DT_ERROR,9,snapshot_dropped,checks,crc_failed,arms);
        reported_dropped=snapshot_dropped;
    }
}

void sdk_rx_probe_counts(uint32_t *total, uint32_t *crc)
{
    *total = checks;
    *crc = crc_failed;
}

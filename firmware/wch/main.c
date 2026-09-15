#include "../product/faults.h"
/* RBP bridge firmware - CH582F entry point.
 *
 * Wires the RBP/3.0 product server to the USB CDC transport and the WCH
 * BLE central radio, and runs everything on TMOS tasks.  See
 * docs/firmware-bringup.md and firmware/Makefile for oscillator configuration.
 */
#include "CONFIG.h"
#include "HAL.h"
#include "board.h"
#include "usb_cdc.h"
#include "wch_central.h"
#include "wch_gatt.h"
#include "device_model.h"
#include "rbp_server.h"
#include <string.h>
#include "../debug/trace.h"
#ifdef RBP_SDK_RX_PROBE
#include "sdk_rx_probe.h"
#endif

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* MCU build: tighter server buffers than the host build (32 KiB RAM) */
extern rbp_server_t *rbp_server_firmware_instance(void);

static rbp_server_t *g_server;
static rc003_adapter_t g_adapter;
uint32_t wch_now_ms; /* shared task-context clock (ms since boot) */

#define SRV_TICK_EVT 0x0001
static uint8_t rbp_server_task_id;
#ifdef RBP_DEBUG
extern uint32_t _susrstack[];
static void debug_stack_paint(void) {
    uintptr_t sp;__asm__ volatile("mv %0, sp":"=r"(sp));
    for(volatile uint32_t *p=_susrstack;(uintptr_t)p+256<sp;p++)*p=0xa5a5a5a5;
}
static uint32_t debug_stack_unused(void) {
    volatile uint32_t *p=_susrstack;uint32_t n=0;
    while(n<4096 && *p++==0xa5a5a5a5)n+=4;
    return n;
}
#endif

/* server output -> USB TX ring */
static size_t srv_out(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    return usb_cdc_write(data, (uint16_t)len);
}

static uint32_t srv_rng(void)
{
    static uint32_t st = 0x9E3779B9;
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    st ^= (uint32_t)RTC_GetCycle32k();
    return st;
}

static uint16_t srv_event(uint8_t task_id, uint16_t events)
{
    (void)task_id;
    if (events & SRV_TICK_EVT) {
        static uint32_t prev_ticks, remainder;
        uint32_t ticks = TMOS_GetSystemClock();
        uint32_t delta = ticks - prev_ticks; prev_ticks = ticks;
        uint64_t elapsed = (uint64_t)delta * 625u + remainder;
        wch_now_ms += (uint32_t)(elapsed / 1000u); remainder = elapsed % 1000u;
        rbp_fault_set_time(wch_now_ms);
#ifdef RBP_DEBUG
        static uint32_t next_metrics;
        dt_time(wch_now_ms);
#ifdef RBP_SDK_RX_PROBE
        sdk_rx_probe_poll();
#endif
        if((int32_t)(wch_now_ms-next_metrics)>=0) {
            DT(DT_SYS,DT_INFO,2,debug_stack_unused(),delta,0,0);
            next_metrics=wch_now_ms+5000;
        }
#endif
        if (usb_cdc_take_lost()) rbp_server_on_usb_gone(g_server, wch_now_ms);
        uint8_t buf[64];
        /* usb_cdc_read re-arms OUT. A drain-until-empty loop can be kept
         * alive indefinitely by the host, starving cooperative BLE tasks.
         * Service at most one USB packet per turn; the controller NAKs
         * further input until the next 5 ms turn drains its 64-byte ring. */
        uint16_t n = usb_cdc_read(buf, sizeof(buf));
        if (n) rbp_server_on_usb_rx(g_server, buf, n, wch_now_ms);
        rbp_server_tick(g_server, wch_now_ms);
        rc003_adapter_tick(&g_adapter, wch_now_ms);
        usb_cdc_flush_tx();
        tmos_start_task(rbp_server_task_id, SRV_TICK_EVT, 8); /* 8 x 0.625 ms */
        return events ^ SRV_TICK_EVT;
    }
    return 0;
}

int main(void)
{
#ifdef RBP_DEBUG
    debug_stack_paint();
    DT(DT_SYS,DT_INFO,1,R8_RESET_STATUS,0,0,0);
#endif
    PWR_DCDCCfg(ENABLE);
    SetSysClock(CLK_SOURCE_PLL_60MHz);
    board_init();
    if(!board_unique_id()) {board_led_blink(20);while(1) {}}
    usb_cdc_init();

    CH58X_BLEInit();
    HAL_Init();
    GAPRole_CentralInit();

    g_server = rbp_server_firmware_instance();
    static rbp_server_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = *wch_central_backend();
    cfg.store = board_store;
    cfg.out = srv_out;
    cfg.rng = srv_rng;
    cfg.profile = &RBP_PROFILE_RC003;
    cfg.max_keys = 16;
    memcpy(cfg.bridge_uid, board_unique_id(), sizeof(cfg.bridge_uid));
    cfg.firmware_version = FW_VERSION;
    cfg.reset_reason = 0;
    rbp_server_init(g_server, &cfg, NULL, 0);

    rc003_adapter_init(&g_adapter, wch_gatt_client(), NULL, g_server);
    wch_central_init(g_server, &g_adapter);

    rbp_server_task_id = TMOS_ProcessEventRegister(srv_event);
    tmos_start_task(rbp_server_task_id, SRV_TICK_EVT, 8);

    while (1) {
        TMOS_SystemProcess();
    }
}

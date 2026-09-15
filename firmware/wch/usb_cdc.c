/* USB CDC-ACM serial link for the CH582F bridge (USB device controller).
 *
 * Layout: EP0 control (standard + CDC class), EP1 interrupt IN (CDC
 * notification), EP2 bulk IN/OUT (data).  DTR changes never reset the
 * bridge session; line coding is stored but ignored (native USB speed).
 *
 * The ISR copies EP2 OUT packets into a ring and re-arms reception; the
 * TMOS task pumps usb_cdc_task() which moves server output into EP2 IN.
 * EP0 control transfers follow the WCH COM example's DATA1 discipline
 * (R_TOG|T_TOG set before every response stage).
 */
#include "usb_cdc.h"
#include "CH58x_common.h"
#include "board.h"
#include <string.h>
#include "../debug/trace.h"

/* Test builds model the controller's W1C transition; production is one
 * volatile register store, with no runtime indirection. */
#ifndef USB_TRANSFER_ACK
#define USB_TRANSFER_ACK() (R8_USB_INT_FG=RB_UIF_TRANSFER)
#endif

/* ---------------- descriptors ---------------- */

static const uint8_t dev_desc[] = {
    0x12, 0x01, 0x10, 0x01, 0x02, 0x00, 0x00, 0x40,
    0x86, 0x1A,               /* VID 1A86 (WCH) - demo baseline, see docs */
    0x57, 0x58,               /* PID 5857 */
    0x01, 0x00, 0x01, 0x02, 0x03, 0x01
};

#define CFG_DESC_LEN (9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7)
static const uint8_t cfg_desc[CFG_DESC_LEN] = {
    0x09, 0x02, CFG_DESC_LEN & 0xFF, (CFG_DESC_LEN >> 8) & 0xFF, 0x02, 0x01,
    0x00, 0x80, 0x32,
    /* IAD: CDC control */
    0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x01, 0x00,
    /* interface 0: communication */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00,
    0x05, 0x24, 0x00, 0x10, 0x01, /* header */
    0x05, 0x24, 0x01, 0x00, 0x01, /* call management */
    0x04, 0x24, 0x02, 0x02,       /* ACM: line coding/control + SERIAL_STATE */
    0x05, 0x24, 0x06, 0x00, 0x01, /* union */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01, /* EP1 IN interrupt */
    /* interface 1: data */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00, /* EP2 OUT bulk */
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00, /* EP2 IN bulk */
};

static const uint8_t lang_desc[] = { 0x04, 0x03, 0x09, 0x04 };
static const uint8_t mfr_desc[] = { 0x0E, 0x03, 'R', 0, 'B', 0, 'P', 0,
                                    ' ', 0, 'B', 0, 'R', 0 };
static const uint8_t prod_desc[] = { 0x18, 0x03, 'B', 0, 'r', 0, 'i', 0,
                                     'd', 0, 'g', 0, 'e', 0, ' ', 0,
                                     'U', 0, 'S', 0, 'B', 0, 0, 0 };

/* Endpoint buffers are DMA aligned. All shared queue changes are made with
 * USB IRQ masked in task context; ISR never calls product/BLE code. */
__attribute__((aligned(4))) static uint8_t ep0_buf[64], ep1_buf[16], ep2_buf[128];
/* Host->board carries management only. One packet per 5ms task turn is
 * sufficient; NAK applies backpressure while the packet is waiting. */
#define RX_RING 64u
#define TX_RING 512u
static uint8_t rx_ring[RX_RING], tx_ring[TX_RING];
static volatile uint16_t rx_head, rx_tail, rx_len, tx_head, tx_tail, tx_len;
static volatile bool tx_inflight, tx_zlp, configured, suspended, dtr_on;
static bool halt_in, halt_out;
static volatile uint8_t lifecycle_lost;
static uint8_t line_coding[7] = {0x00,0xc2,0x01,0,0,0,8};
static uint8_t setup_req[8], serial_desc[34], small_reply[2];
static const uint8_t *ctrl_ptr;
static uint16_t ctrl_left;
static bool ctrl_zlp;
static uint8_t pending_address;
static bool address_pending;
enum { EP_IDLE, EP_DATA_IN, EP_STATUS_OUT, EP_STATUS_IN, EP_LINE_OUT };
static uint8_t ctrl_state;

static void rx_arm(void) {
    R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_R_RES) |
        (halt_out ? UEP_R_RES_STALL : (configured && !suspended && !lifecycle_lost && RX_RING-rx_len >= 64)
         ? UEP_R_RES_ACK : UEP_R_RES_NAK);
}
static void data_reset(bool reset_toggles) {
    uint8_t toggles=reset_toggles?0:(R8_UEP2_CTRL&(RB_UEP_R_TOG|RB_UEP_T_TOG));
    rx_head=rx_tail=rx_len=tx_head=tx_tail=tx_len=0;
    tx_inflight=tx_zlp=false;
    R8_UEP2_T_LEN=0;
    /* Software toggles, as in WCH's CDC example. Masking the USB IRQ does
     * not stop the controller: AUTO_TOG can otherwise change a toggle bit
     * between a task-side CTRL read and write (rx_arm/tx_kick). */
    R8_UEP2_CTRL=toggles|(halt_out?UEP_R_RES_STALL:UEP_R_RES_NAK) | (halt_in?UEP_T_RES_STALL:UEP_T_RES_NAK);
}
static void lose_transport(void) {
    DT(DT_USB,DT_INFO,1,configured,suspended,rx_len,tx_len);
    lifecycle_lost=1;
    data_reset(false);
}
/* Called with IRQ excluded, or from ISR. Completion immediately loads the
 * next packet, so throughput is not limited to one packet per TMOS tick. */
static void tx_kick(void) {
    if(tx_inflight || halt_in || !configured || suspended || lifecycle_lost) return;
    if(!tx_len && !tx_zlp) return;
    uint8_t n=0;
    while(n<64 && tx_len) {
        ep2_buf[64+n++]=tx_ring[tx_tail];
        tx_tail=(tx_tail+1)%TX_RING; --tx_len;
    }
    tx_zlp=(n==64);
    tx_inflight=true;
    R8_UEP2_T_LEN=n;
    R8_UEP2_CTRL=(R8_UEP2_CTRL & ~MASK_UEP_T_RES)|UEP_T_RES_ACK;
}
static void ctrl_stall(void) {
    ctrl_state=EP_IDLE;
    R8_UEP0_CTRL=RB_UEP_R_TOG|RB_UEP_T_TOG|UEP_R_RES_STALL|UEP_T_RES_STALL;
}
static void ctrl_next(void) {
    uint8_t n=ctrl_left>64?64:(uint8_t)ctrl_left;
    if(n) memcpy(ep0_buf,ctrl_ptr,n);
    ctrl_ptr+=n; ctrl_left-=n;
    if(!n) ctrl_zlp=false;
    R8_UEP0_T_LEN=n;
    R8_UEP0_CTRL=(R8_UEP0_CTRL & (RB_UEP_R_TOG|RB_UEP_T_TOG)) |
        UEP_R_RES_NAK|UEP_T_RES_ACK;
}
static void ctrl_send(const uint8_t *data,uint16_t actual,uint16_t requested) {
    ctrl_ptr=data; ctrl_left=actual<requested?actual:requested;
    ctrl_zlp=(actual<requested && (ctrl_left%64)==0);
    ctrl_state=EP_DATA_IN;
    R8_UEP0_CTRL=RB_UEP_R_TOG|RB_UEP_T_TOG;
    ctrl_next();
}
static void ctrl_status(void) {
    ctrl_state=EP_STATUS_IN; R8_UEP0_T_LEN=0;
    R8_UEP0_CTRL=RB_UEP_R_TOG|RB_UEP_T_TOG|UEP_R_RES_NAK|UEP_T_RES_ACK;
}
static void serial_state(void) {
    static const uint8_t notification[10]={0xa1,0x20,0,0,0,0,2,0,3,0};
    if(!configured)return;
    memcpy(ep1_buf,notification,sizeof notification);
    R8_UEP1_T_LEN=sizeof notification;
    R8_UEP1_CTRL=(R8_UEP1_CTRL&~MASK_UEP_T_RES)|UEP_T_RES_ACK;
}
/* Control request decoding is infrequent. Keep it in flash rather than
 * letting the optimizer inline the whole decoder into the RAM ISR. */
__attribute__((noinline)) static void setup(void) {
    uint8_t type=setup_req[0], req=setup_req[1];
    uint16_t value=setup_req[2]|(setup_req[3]<<8);
    uint16_t index=setup_req[4]|(setup_req[5]<<8);
    uint16_t len=setup_req[6]|(setup_req[7]<<8);
    ctrl_left=0; ctrl_zlp=false; address_pending=false;
    if(type==0x80 && req==6) {
        switch(value>>8) {
        case 1: if((value&255)==0) {ctrl_send(dev_desc,sizeof dev_desc,len);return;} break;
        case 2: if((value&255)==0) {ctrl_send(cfg_desc,sizeof cfg_desc,len);return;} break;
        case 3:
            switch(value&255) {
            case 0: ctrl_send(lang_desc,sizeof lang_desc,len);return;
            case 1: ctrl_send(mfr_desc,sizeof mfr_desc,len);return;
            case 2: ctrl_send(prod_desc,sizeof prod_desc,len);return;
            case 3: ctrl_send(serial_desc,sizeof serial_desc,len);return;
            }
        }
    } else if(type==0 && req==5 && value<128 && !index && !len) {
        pending_address=(uint8_t)value; address_pending=true; ctrl_status();return;
    } else if(type==0 && req==9 && value<=1 && !index && !len) {
        halt_in=halt_out=false;lose_transport();data_reset(true);configured=value!=0;
        R8_UEP1_CTRL=UEP_T_RES_NAK;
        if(configured)serial_state();
        ctrl_status();return;
    } else if(type==0x80 && req==8 && !value && !index && len==1) {
        small_reply[0]=configured;ctrl_send(small_reply,1,len);return;
    } else if(req==0 && !value && len==2 && (type==0x80 || type==0x81 || type==0x82)) {
        small_reply[0]=small_reply[1]=0;
        if(type==0x81 && index>1) {ctrl_stall();return;}
        if(type==0x82) {
            if(index==0x82) small_reply[0]=(R8_UEP2_CTRL&MASK_UEP_T_RES)==UEP_T_RES_STALL;
            else if(index==2) small_reply[0]=(R8_UEP2_CTRL&MASK_UEP_R_RES)==UEP_R_RES_STALL;
            else if(index==0x81)small_reply[0]=(R8_UEP1_CTRL&MASK_UEP_T_RES)==UEP_T_RES_STALL;
            else if(index!=0 && index!=0x80) {ctrl_stall();return;}
        }
        ctrl_send(small_reply,2,len);return;
    } else if(type==2 && (req==1 || req==3) && value==0 && len==0) {
        if(index==0x81) {
            R8_UEP1_CTRL=req==3?(R8_UEP1_CTRL&RB_UEP_T_TOG)|UEP_T_RES_STALL:UEP_T_RES_NAK;
            ctrl_status();return;
        }
        if(index!=0x82 && index!=2) {ctrl_stall();return;}
        lose_transport();
        if(index==0x82)halt_in=req==3;else halt_out=req==3;
        if(index==0x82) R8_UEP2_CTRL=(R8_UEP2_CTRL & ~(MASK_UEP_T_RES|(req==1?RB_UEP_T_TOG:0))) |
            (req==3?UEP_T_RES_STALL:UEP_T_RES_NAK);
        else if(index==2) R8_UEP2_CTRL=(R8_UEP2_CTRL & ~(MASK_UEP_R_RES|(req==1?RB_UEP_R_TOG:0))) |
            (req==3?UEP_R_RES_STALL:UEP_R_RES_NAK);
        else {ctrl_stall();return;}
        rx_arm();ctrl_status();return;
    } else if(type==0x81 && req==10 && !value && index<2 && len==1) {
        small_reply[0]=0;ctrl_send(small_reply,1,len);return;
    } else if(type==1 && req==11 && !value && index<2 && !len) {
        ctrl_status();return;
    } else if(type==0x21 && index==0) {
        if(req==0x20 && len==7 && !value) {
            ctrl_state=EP_LINE_OUT;
            R8_UEP0_CTRL=RB_UEP_R_TOG|RB_UEP_T_TOG|UEP_R_RES_ACK|UEP_T_RES_NAK;return;
        }
        if(req==0x22 && !len && value<=3) {dtr_on=(value&1)!=0;ctrl_status();return;}
    } else if(type==0xa1 && req==0x21 && !index && !value && len==7) {
        ctrl_send(line_coding,7,len);return;
    }
    ctrl_stall();
}
#ifdef __riscv
__attribute__((interrupt("WCH-Interrupt-fast")))
#endif
__HIGH_CODE void USB_IRQHandler(void) {
    uint8_t flags=R8_USB_INT_FG;
    if(flags & RB_UIF_BUS_RST) {
        DT(DT_USB,DT_INFO,2,flags,0,0,0);
        R8_USB_DEV_AD=0;configured=false;suspended=false;dtr_on=false;halt_in=halt_out=false;
        ctrl_state=EP_IDLE;address_pending=false;lose_transport();data_reset(true);
        R8_UEP0_CTRL=UEP_R_RES_ACK|UEP_T_RES_NAK;
        R8_UEP1_CTRL=UEP_T_RES_NAK;
        R8_USB_INT_FG=RB_UIF_BUS_RST|RB_UIF_TRANSFER|(flags&RB_UIF_SUSPEND);return;
    }
    if(flags & RB_UIF_TRANSFER) {
    uint8_t st=R8_USB_INT_ST;
    bool setup_pending=(st & RB_UIS_SETUP_ACT)!=0;
    /* SETUP is independent of the retained token. CH583DS1 requires
     * acknowledging that token first, then SETUP with a second W1C.
     * Save SETUP before any previous EP0 completion can reuse its DMA. */
    if(setup_pending) memcpy(setup_req,ep0_buf,8);
    if((st & MASK_UIS_TOKEN)!=MASK_UIS_TOKEN) {
    switch(st & (MASK_UIS_TOKEN|MASK_UIS_ENDP)) {
    case UIS_TOKEN_OUT|2:
        if(flags&RB_U_TOG_OK) {
            R8_UEP2_CTRL^=RB_UEP_R_TOG;
            uint8_t n=R8_USB_RX_LEN;
            DT(DT_USB,DT_INFO,5,n,st,rx_len,flags);
            if(n>64 || RX_RING-rx_len<n) {DT(DT_USB,DT_ERROR,4,n,rx_len,0,0);lose_transport();}
            else for(uint8_t i=0;i<n;i++) {rx_ring[rx_head]=ep2_buf[i];rx_head=(rx_head+1)%RX_RING;rx_len++;}
            rx_arm();
        } break;
    case UIS_TOKEN_IN|2:
        R8_UEP2_CTRL=((R8_UEP2_CTRL^RB_UEP_T_TOG)&~MASK_UEP_T_RES)|UEP_T_RES_NAK;
        tx_inflight=false;tx_kick();break;
    case UIS_TOKEN_IN|1:
        R8_UEP1_CTRL=((R8_UEP1_CTRL^RB_UEP_T_TOG)&~MASK_UEP_T_RES)|UEP_T_RES_NAK;break;
    case UIS_TOKEN_IN|0:
        if(ctrl_state==EP_DATA_IN && !setup_pending) {
            R8_UEP0_CTRL^=RB_UEP_T_TOG;
            if(ctrl_left || ctrl_zlp) ctrl_next();
            else {ctrl_state=EP_STATUS_OUT;R8_UEP0_CTRL=RB_UEP_R_TOG|UEP_R_RES_ACK|UEP_T_RES_NAK;}
        } else if(ctrl_state==EP_STATUS_IN) {
            if(address_pending) {R8_USB_DEV_AD=pending_address;address_pending=false;}
            ctrl_state=EP_IDLE;R8_UEP0_CTRL=UEP_R_RES_ACK|UEP_T_RES_NAK;
        } break;
    case UIS_TOKEN_OUT|0:
        /* SETUP has replaced the EP0 receive buffer and aborts the old
         * control data stage. Never interpret its bytes as line coding. */
        if(setup_pending || !(flags&RB_U_TOG_OK)) break;
        if(ctrl_state==EP_LINE_OUT && R8_USB_RX_LEN==7) {memcpy(line_coding,ep0_buf,7);ctrl_status();}
        else if(ctrl_state==EP_STATUS_OUT && R8_USB_RX_LEN==0) {
            ctrl_state=EP_IDLE;R8_UEP0_CTRL=UEP_R_RES_ACK|UEP_T_RES_NAK;
        } else ctrl_stall();
        break;
    }
    USB_TRANSFER_ACK();
    }
    /* Also catch SETUP arriving during token handling. It stays pending
     * across the first clear, as in the WCH COM reference ISR. */
    if(R8_USB_INT_ST & RB_UIS_SETUP_ACT) {
        /* Reinitialize EP0 before interpreting the request, as in WCH's
         * device examples. The previous transfer may have left OUT NAK or
         * STALL; SETUP always begins a new DATA1 control transfer. */
        R8_UEP0_CTRL=RB_UEP_R_TOG|RB_UEP_T_TOG|UEP_R_RES_ACK|UEP_T_RES_NAK;
        if(!setup_pending) memcpy(setup_req,ep0_buf,8);
        DT(DT_USB,DT_INFO,6,setup_req[0]|((uint32_t)setup_req[1]<<8),
           setup_req[2]|((uint32_t)setup_req[3]<<8),st,ctrl_state);
        setup();
        USB_TRANSFER_ACK();
    }
    }
    /* Account for completed tokens before suspend tears down queues.
     * Suspend is not a USB reset: DATA toggles survive it. */
    if(flags & RB_UIF_SUSPEND) {
        DT(DT_USB,DT_INFO,3,R8_USB_MIS_ST,flags,0,0);
        suspended=(R8_USB_MIS_ST&RB_UMS_SUSPEND)!=0;
        if(suspended) lose_transport();
        R8_USB_INT_FG=RB_UIF_SUSPEND;
    }
}
void usb_cdc_init(void) {
    const uint8_t *uid=board_unique_id();if(!uid)return;serial_desc[0]=sizeof serial_desc;serial_desc[1]=3;
    for(uint8_t k=0;k<16;k++) {uint8_t nib=(uid[k/2]>>((k&1)?0:4))&15;
        serial_desc[2+k*2]="0123456789ABCDEF"[nib];serial_desc[3+k*2]=0;}
    R8_USB_CTRL=0;
    R8_UEP4_1_MOD=RB_UEP1_TX_EN;
    R8_UEP2_3_MOD=RB_UEP2_RX_EN|RB_UEP2_TX_EN;
    R16_UEP0_DMA=(uint16_t)(uintptr_t)ep0_buf;
    R16_UEP1_DMA=(uint16_t)(uintptr_t)ep1_buf;
    R16_UEP2_DMA=(uint16_t)(uintptr_t)ep2_buf;
    data_reset(true);R8_UEP0_CTRL=UEP_R_RES_ACK|UEP_T_RES_NAK;
    R8_UEP1_CTRL=UEP_T_RES_NAK;
    R8_USB_DEV_AD=0;
    R8_USB_CTRL=RB_UC_DEV_PU_EN|RB_UC_INT_BUSY|RB_UC_DMA_EN;
    R16_PIN_ANALOG_IE|=RB_PIN_USB_IE|RB_PIN_USB_DP_PU;
    R8_USB_INT_FG=0xff;R8_UDEV_CTRL=RB_UD_PD_DIS|RB_UD_PORT_EN;
    R8_USB_INT_EN=RB_UIE_SUSPEND|RB_UIE_BUS_RST|RB_UIE_TRANSFER;
    /* PFIC bit7 is the pre-emption level (0 higher, 1 lower).
     * Keep BLE BB/LLE at the SDK's high level. A USB packet copy must
     * be pre-emptible by the time-sensitive radio receive handlers. */
    PFIC_SetPriority(USB_IRQn, 0x80);
    PFIC_EnableIRQ(USB_IRQn);
}
/* Task-only critical sections: preserve the caller's IRQ mask and keep
 * compiler memory operations inside it. Radio IRQs remain enabled. */
static uint32_t usb_lock(void) {
    uint32_t enabled=PFIC_GetStatusIRQ(USB_IRQn);
    PFIC_DisableIRQ(USB_IRQn);
    __asm__ volatile("":::"memory");
    return enabled;
}
static void usb_unlock(uint32_t enabled) {
    __asm__ volatile("":::"memory");
    if(enabled) PFIC_EnableIRQ(USB_IRQn);
}
bool usb_cdc_take_lost(void) {
    uint32_t irq=usb_lock();bool lost=lifecycle_lost!=0;lifecycle_lost=0;rx_arm();
    usb_unlock(irq);return lost;
}
uint16_t usb_cdc_read(uint8_t *buf,uint16_t cap) {
    uint32_t irq=usb_lock();uint16_t n=0;
    while(n<cap && rx_len) {buf[n++]=rx_ring[rx_tail];rx_tail=(rx_tail+1)%RX_RING;rx_len--;}
    rx_arm();usb_unlock(irq);return n;
}
uint16_t usb_cdc_write(const uint8_t *buf,uint16_t len) {
    uint32_t irq=usb_lock();uint16_t n=0;
    if(configured && !suspended && !lifecycle_lost)
        while(n<len && tx_len<TX_RING) {tx_ring[tx_head]=buf[n++];tx_head=(tx_head+1)%TX_RING;tx_len++;}
    tx_kick();usb_unlock(irq);return n;
}
void usb_cdc_flush_tx(void) {uint32_t irq=usb_lock();tx_kick();usb_unlock(irq);}
void usb_cdc_task(uint32_t now_ms) {(void)now_ms;usb_cdc_flush_tx();}
bool usb_cdc_dtr(void) {return dtr_on;}

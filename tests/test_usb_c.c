#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "CH58x_common.h"
#undef R8_UEP1_T_LEN
static volatile uint8_t R8_UEP1_T_LEN;
#undef R8_UEP2_CTRL
static volatile uint8_t R8_UEP2_CTRL;
#undef R8_UEP2_T_LEN
static volatile uint8_t R8_UEP2_T_LEN;
#undef R8_UEP0_CTRL
static volatile uint8_t R8_UEP0_CTRL;
#undef R8_UEP0_T_LEN
static volatile uint8_t R8_UEP0_T_LEN;
#undef R8_USB_INT_FG
static volatile uint8_t R8_USB_INT_FG;
#undef R8_USB_DEV_AD
static volatile uint8_t R8_USB_DEV_AD;
#undef R8_UEP1_CTRL
static volatile uint8_t R8_UEP1_CTRL;
#undef R8_USB_MIS_ST
static volatile uint8_t R8_USB_MIS_ST;
#undef R8_USB_INT_ST
static volatile uint8_t R8_USB_INT_ST;
#undef R8_USB_RX_LEN
static volatile uint8_t R8_USB_RX_LEN;
#undef R8_USB_CTRL
static volatile uint8_t R8_USB_CTRL;
#undef R8_UEP4_1_MOD
static volatile uint8_t R8_UEP4_1_MOD;
#undef R8_UEP2_3_MOD
static volatile uint8_t R8_UEP2_3_MOD;
#undef R16_UEP0_DMA
static volatile uint16_t R16_UEP0_DMA;
#undef R16_UEP1_DMA
static volatile uint16_t R16_UEP1_DMA;
#undef R16_UEP2_DMA
static volatile uint16_t R16_UEP2_DMA;
#undef R16_PIN_ANALOG_IE
static volatile uint16_t R16_PIN_ANALOG_IE;
#undef R8_UDEV_CTRL
static volatile uint8_t R8_UDEV_CTRL;
#undef R8_USB_INT_EN
static volatile uint8_t R8_USB_INT_EN;
#undef PFIC_SetPriority
#undef PFIC_EnableIRQ
#undef PFIC_DisableIRQ
#undef PFIC_GetStatusIRQ
static bool usb_irq_enabled;
#define PFIC_DisableIRQ(irq) (usb_irq_enabled=false)
#define PFIC_GetStatusIRQ(irq) (usb_irq_enabled)
static uint8_t usb_irq_priority;
static unsigned usb_priority_set;
static void mock_priority(unsigned irq,uint8_t priority){assert(irq==USB_IRQn);usb_irq_priority=priority;usb_priority_set++;}
static void mock_enable(unsigned irq){assert(irq==USB_IRQn);assert(usb_priority_set && usb_irq_priority==0x80);usb_irq_enabled=true;}
#define PFIC_SetPriority(irq,priority) mock_priority(irq,priority)
#define PFIC_EnableIRQ(irq) mock_enable(irq)
static unsigned ack_count,ack_action;
static void mock_transfer_ack(void);
#define USB_TRANSFER_ACK() mock_transfer_ack()
#include "../firmware/wch/usb_cdc.c"
/* Scripted register contract fixture, NOT a cycle-accurate SIE emulator.
 * ack_action exercises live-state changes; it is not evidence that the
 * physical controller generates that transition during the observed fault. */
static void mock_transfer_ack(void) {
    ack_count++;
    if((R8_USB_INT_ST&MASK_UIS_TOKEN)!=MASK_UIS_TOKEN)
        R8_USB_INT_ST|=MASK_UIS_TOKEN;
    else R8_USB_INT_ST&=~RB_UIS_SETUP_ACT;
    R8_USB_INT_FG&=~RB_UIF_TRANSFER;
    if(ack_action==1) R8_USB_INT_ST&=~RB_UIS_SETUP_ACT;
    if(ack_action==2) {
        const uint8_t d[]={0x80,8,0,0,0,0,1,0};memcpy(ep0_buf,d,8);
        R8_USB_INT_ST|=RB_UIS_SETUP_ACT;
    }
    ack_action=0;
}
const uint8_t *board_unique_id(void){static const uint8_t id[16]={1};return id;}
static void irq(uint8_t token){R8_USB_INT_ST=token;R8_USB_INT_FG=RB_UIF_TRANSFER|RB_U_TOG_OK;USB_IRQHandler();}
static void request(uint8_t type,uint8_t req,uint16_t value,uint16_t index,uint16_t len){
    uint8_t d[]={type,req,value,value>>8,index,index>>8,len,len>>8};memcpy(ep0_buf,d,8);irq(RB_UIS_SETUP_ACT|MASK_UIS_TOKEN);
}
int main(void){
    usb_cdc_init();assert(usb_priority_set==1 && usb_irq_priority==0x80);
    request(0x80,6,0x200,0,255);assert(R8_UEP0_T_LEN==64&&!memcmp(ep0_buf,cfg_desc,64));
    irq(UIS_TOKEN_IN);assert(R8_UEP0_T_LEN==11&&!memcmp(ep0_buf,cfg_desc+64,11));
    irq(UIS_TOKEN_IN);assert(ctrl_state==EP_STATUS_OUT);
    R8_USB_RX_LEN=0;irq(UIS_TOKEN_OUT);assert(ctrl_state==EP_IDLE);
    request(0,5,42,0,0);assert(R8_USB_DEV_AD==0);irq(UIS_TOKEN_IN);assert(R8_USB_DEV_AD==42);
    request(0x80,6,0x300,0,255);assert(R8_UEP0_T_LEN==4&&!memcmp(ep0_buf,lang_desc,4));
    request(0,9,1,0,0);irq(UIS_TOKEN_IN);assert(usb_cdc_take_lost());
    uint8_t sent[256],received[256];for(unsigned i=0;i<256;i++)sent[i]=i;
    ctrl_send(sent,128,255);assert(R8_UEP0_T_LEN==64);irq(UIS_TOKEN_IN);assert(R8_UEP0_T_LEN==64);
    irq(UIS_TOKEN_IN);assert(R8_UEP0_T_LEN==0&&ctrl_state==EP_DATA_IN);irq(UIS_TOKEN_IN);assert(ctrl_state==EP_STATUS_OUT);
    for(unsigned i=0;i<4;i++){
        uint8_t before=R8_UEP2_CTRL&RB_UEP_R_TOG;
        memcpy(ep2_buf,sent+64*i,64);R8_USB_RX_LEN=64;irq(UIS_TOKEN_OUT|2);
        assert((R8_UEP2_CTRL&RB_UEP_R_TOG)==(before^RB_UEP_R_TOG));
        /* A retransmitted DATA packet must not enqueue or toggle twice. */
        R8_USB_INT_ST=UIS_TOKEN_OUT|2;R8_USB_INT_FG=RB_UIF_TRANSFER;USB_IRQHandler();
        assert(rx_len==64&&(R8_UEP2_CTRL&RB_UEP_R_TOG)==(before^RB_UEP_R_TOG));
        assert((R8_UEP2_CTRL&MASK_UEP_R_RES)==UEP_R_RES_NAK);
        assert(usb_cdc_read(received+64*i,64)==64);
    }
    assert(!memcmp(sent,received,256));
    assert(usb_cdc_write(sent,256)==256);assert(R8_UEP2_T_LEN==64);
    assert(!(R8_UEP2_CTRL&RB_UEP_AUTO_TOG));
    for(unsigned i=0;i<4;i++){
        assert(!memcmp(ep2_buf+64,sent+i*64,64));
        uint8_t before=R8_UEP2_CTRL&RB_UEP_T_TOG;
        /* Task-side RX rearming must preserve the outstanding IN toggle. */
        rx_arm();assert((R8_UEP2_CTRL&RB_UEP_T_TOG)==before);
        irq(UIS_TOKEN_IN|2);assert((R8_UEP2_CTRL&RB_UEP_T_TOG)==(before^RB_UEP_T_TOG));
    }
    assert(tx_inflight&&R8_UEP2_T_LEN==0);irq(UIS_TOKEN_IN|2);
    assert(!tx_inflight&&(R8_UEP2_CTRL&MASK_UEP_T_RES)==UEP_T_RES_NAK);
    /* SETUP has an independent pending bit: it must not swallow the
     * previous bulk completion (CH583DS1 USB_INT_ST). */
    uint8_t get_config[]={0x80,8,0,0,0,0,1,0};
    memcpy(ep0_buf,get_config,8);memcpy(ep2_buf,sent,32);R8_USB_RX_LEN=32;
    uint8_t rx_toggle=R8_UEP2_CTRL&RB_UEP_R_TOG;
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_OUT|2);
    assert(usb_cdc_read(received,64)==32 && !memcmp(received,sent,32));
    assert((R8_UEP2_CTRL&RB_UEP_R_TOG)==(rx_toggle^RB_UEP_R_TOG));
    assert(R8_UEP0_T_LEN==1 && ep0_buf[0]==1);
    assert(usb_cdc_write(sent,100)==100);
    memcpy(ep0_buf,get_config,8);
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_IN|2);
    assert(tx_inflight && R8_UEP2_T_LEN==36 && !memcmp(ep2_buf+64,sent+64,36));
    assert(R8_UEP0_T_LEN==1 && ep0_buf[0]==1);
    irq(UIS_TOKEN_IN|2);assert(!tx_inflight);
    /* SETUP dispatch must use live status after the first W1C, not OR
     * the old snapshot back in. Also accept a newly pending SETUP there. */
    memcpy(ep0_buf,get_config,8);R8_USB_RX_LEN=0;
    unsigned ack_before=ack_count;ack_action=1;
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_OUT|2);
    assert(ack_count==ack_before+1); /* withdrawn status is not replayed */
    ack_before=ack_count;ack_action=2;
    irq(UIS_TOKEN_OUT|2);
    assert(ack_count==ack_before+2 && ep0_buf[0]==1);
    assert(!(R8_USB_INT_ST&RB_UIS_SETUP_ACT));
    /* A new SETUP supersedes EP0 data stages, but a completed address
     * status stage must commit before the next request is dispatched. */
    request(0,5,43,0,0);memcpy(ep0_buf,get_config,8);
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_IN);
    assert(R8_USB_DEV_AD==43 && ep0_buf[0]==1);
    request(0x80,6,0x200,0,255);memcpy(ep0_buf,get_config,8);
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_IN);
    assert(R8_UEP0_T_LEN==1 && ep0_buf[0]==1);
    uint8_t previous_line[7];memcpy(previous_line,line_coding,7);
    request(0x21,0x20,0,0,7);memcpy(ep0_buf,get_config,8);R8_USB_RX_LEN=7;
    irq(RB_UIS_SETUP_ACT|UIS_TOKEN_OUT);
    assert(!memcmp(line_coding,previous_line,7) && ep0_buf[0]==1);
    request(0x21,0x22,1,0,0);irq(UIS_TOKEN_IN);assert(usb_cdc_dtr()&&!usb_cdc_take_lost());
    request(2,3,0,0x81,0);irq(UIS_TOKEN_IN);assert(!(R8_UEP1_CTRL&RB_UEP_AUTO_TOG));
    request(2,1,0,0x81,0);irq(UIS_TOKEN_IN);assert(!(R8_UEP1_CTRL&(RB_UEP_AUTO_TOG|RB_UEP_T_TOG)));
    /* Endpoint halt and suspend must not reset the other DATA toggle. */
    R8_UEP2_CTRL|=RB_UEP_R_TOG|RB_UEP_T_TOG;
    request(2,3,0,0x82,0);irq(UIS_TOKEN_IN);assert(usb_cdc_take_lost());
    assert((R8_UEP2_CTRL&MASK_UEP_T_RES)==UEP_T_RES_STALL);
    request(2,1,0,0x82,0);irq(UIS_TOKEN_IN);assert(usb_cdc_take_lost());
    assert(R8_UEP2_CTRL&RB_UEP_R_TOG);
    assert(!(R8_UEP2_CTRL&RB_UEP_T_TOG));
    R8_UEP2_CTRL|=RB_UEP_T_TOG;
    assert(usb_cdc_write(sent,64)==64);
    R8_USB_MIS_ST=RB_UMS_SUSPEND;R8_USB_INT_FG=RB_UIF_SUSPEND;USB_IRQHandler();
    assert((R8_UEP2_CTRL&(RB_UEP_R_TOG|RB_UEP_T_TOG))==(RB_UEP_R_TOG|RB_UEP_T_TOG));
    assert(usb_cdc_take_lost()&&suspended&&usb_cdc_write(sent,64)==0);
    R8_USB_MIS_ST=0;R8_USB_INT_FG=RB_UIF_SUSPEND;USB_IRQHandler();assert(!suspended);
    /* Pending IN completion is accounted before suspend; bus reset wins
     * over both and initializes DATA0 on each direction. */
    assert(usb_cdc_write(sent,10)==10);
    R8_USB_MIS_ST=RB_UMS_SUSPEND;R8_USB_INT_ST=UIS_TOKEN_IN|2;
    R8_USB_INT_FG=RB_UIF_SUSPEND|RB_UIF_TRANSFER;USB_IRQHandler();
    assert(!(R8_UEP2_CTRL&RB_UEP_T_TOG));
    usb_irq_enabled=false;
    usb_cdc_take_lost();assert(!usb_irq_enabled);
    usb_cdc_read(received,64);assert(!usb_irq_enabled);
    usb_cdc_write(sent,10);assert(!usb_irq_enabled);
    usb_cdc_flush_tx();assert(!usb_irq_enabled);
    usb_irq_enabled=true;
    R8_USB_INT_FG=RB_UIF_BUS_RST|RB_UIF_TRANSFER|RB_UIF_SUSPEND;USB_IRQHandler();assert(usb_cdc_take_lost()&&!configured&&!tx_len);
    assert(!(R8_UEP2_CTRL&(RB_UEP_R_TOG|RB_UEP_T_TOG)));
    assert(!(R8_UEP1_CTRL&RB_UEP_AUTO_TOG));
    assert(!suspended);
    puts("USB: EP0 split/status/address, token routing, RX FIFO, TX burst/ZLP, halt/reset passed");
    return 0;
}

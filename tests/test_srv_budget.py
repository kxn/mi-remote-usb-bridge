"""Execute the actual MCU task body with a perpetually full USB input mock."""
from pathlib import Path
import subprocess
import os
import shutil
root=Path(__file__).resolve().parents[1]
source=(root/'firmware/wch/main.c').read_text()
body=source[source.index('static uint16_t srv_event('):source.index('\nint main(void)')]
prefix=r'''#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define SRV_TICK_EVT 1
static uint32_t wch_now_ms;
void rbp_fault_set_time(uint32_t now){assert(now==5);}
static uint8_t rbp_server_task_id=3;
static int g_server,g_adapter;
static unsigned reads,received,ticks,adapter_ticks,scheduled,flushed;
uint32_t TMOS_GetSystemClock(void){return 8;}
int usb_cdc_take_lost(void){return 0;}
void rbp_server_on_usb_gone(int s,uint32_t n){(void)s;(void)n;assert(0);}
uint16_t usb_cdc_read(uint8_t *p,uint16_t n){assert(++reads==1);assert(n==64);p[0]=42;return n;}
void rbp_server_on_usb_rx(int s,const uint8_t *p,uint16_t n,uint32_t now){(void)s;assert(p[0]==42 && now==5);received+=n;}
void rbp_server_tick(int s,uint32_t n){(void)s;assert(n==5);++ticks;}
void rc003_adapter_tick(int *a,uint32_t n){assert(a==&g_adapter && n==5);++adapter_ticks;}
void usb_cdc_flush_tx(void){++flushed;}
void tmos_start_task(uint8_t task,uint16_t event,uint32_t delay){assert(task==3 && event==1 && delay==8);++scheduled;}
'''
main=r'''
int main(void){
 assert(srv_event(3,5)==4);
 assert(reads==1 && received==64 && ticks==1 && adapter_ticks==1 && scheduled==1 && flushed==1);
 assert(srv_event(3,4)==0 && reads==1);
 puts("MCU task: continuously available USB input cannot prevent yielding; tick work and rearm preserved");
}
'''
build=root/'build/srv-budget';build.mkdir(parents=True,exist_ok=True)
unit=build/'actual_srv_event.c';unit.write_text(prefix+body+main)
exe=build/'test.exe'
subprocess.run([os.environ.get('CC') or shutil.which('clang') or 'C:/msys64/clang64/bin/clang.exe','-std=c11','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

#include <assert.h>
#include <stdio.h>
#include "report_map.h"
#include "rbp/defs.h"
#include "rc003_adapter.h"
int main(void){
    assert(rc003_adapter_match("Xiaomi RC003",12,true)==1);
    assert(rc003_adapter_match("RC003",5,false)==1);
    assert(rc003_adapter_match("Generic Keyboard",16,true)==0);
    assert(rc003_adapter_match("Xiaomi Keyboard",15,true)==0);
    assert(rc003_adapter_match("RC003fake",9,true)==0);
    const char observed_name[]="\xe5\xb0\x8f\xe7\xb1\xb3\xe8\x93\x9d\xe7\x89\x99\xe8\xaf\xad\xe9\x9f\xb3\xe9\x81\xa5\xe6\x8e\xa7\xe5\x99\xa8";
    assert(rc003_adapter_match(observed_name,sizeof observed_name-1,false)==1);
    assert(rc003_adapter_match(observed_name,sizeof observed_name-2,true)==0);
    /* Report ID 7, three bits of padding, then Up/Down variables. */
    const uint8_t desc[]={5,12,9,1,0xa1,1,0x85,7,0x75,3,0x95,1,0x81,1,
        0xa4,0x75,1,0x95,2,9,0x42,9,0x43,0x81,2,0xb4,0x81,1,0xc0};
    hogp_report_map_t map;uint64_t keys=0;uint8_t data=0x18;
    assert(hogp_report_map_parse(desc,sizeof desc,&map));
    assert(hogp_decode_report(&map,7,&data,1,&keys));
    assert(keys==((1ull<<RBP_KEY_UP)|(1ull<<RBP_KEY_DOWN)));
    assert(!hogp_decode_report(&map,0,&data,1,&keys));
    assert(!hogp_decode_report(&map,7,&data,2,&keys));
    data=0;assert(hogp_decode_report(&map,7,&data,1,&keys)&&keys==0);
    assert(!hogp_report_map_parse(desc,sizeof desc-1,&map));
    const uint8_t array[]={5,7,9,6,0xa1,1,0x15,0,0x26,255,0,0x19,0,0x2a,255,0,0x75,8,0x95,2,0x81,0,0xc0};
    assert(hogp_report_map_parse(array,sizeof array,&map));
    uint8_t two[]={0x52,0x51};assert(hogp_decode_report(&map,0,two,2,&keys));
    assert(keys==((1ull<<RBP_KEY_UP)|(1ull<<RBP_KEY_DOWN)));
    two[0]=1;assert(!hogp_decode_report(&map,0,two,2,&keys));
    const uint8_t bad[]={0xfe,255,0};assert(!hogp_report_map_parse(bad,sizeof bad,&map));
    /* Captured hardware Map, including three opaque 120-byte vendor reports. */
    FILE *fixture=fopen("tests/fixtures/rc003-hardware-report-map.hex","r");assert(fixture);
    uint8_t observed[86];unsigned byte;
    for(unsigned i=0;i<sizeof observed;i++){assert(fscanf(fixture,"%2x",&byte)==1);observed[i]=(uint8_t)byte;}
    fclose(fixture);
    assert(hogp_report_map_parse(observed,sizeof observed,&map));
    assert(map.input_count==1&&map.inputs[0].report_id==1&&map.inputs[0].bytes==6);
    uint8_t actual[]={0x52,0,0x4a,0,0x3e,0};
    assert(hogp_decode_report(&map,1,actual,sizeof actual,&keys));
    assert(keys==((1ull<<RBP_KEY_UP)|(1ull<<RBP_KEY_HOME)|(1ull<<RBP_KEY_VOICE)));
    assert(!hogp_decode_report(&map,6,actual,sizeof actual,&keys));
    /* Skipping a vendor field must preserve offsets within a shared ID. */
    const uint8_t mixed[]={0x06,0,0xff,0x85,1,0x75,8,0x95,2,0x81,0,
        5,7,0x19,0,0x29,0xfe,0x15,0,0x25,0xfe,0x95,1,0x81,0};
    uint8_t mixed_value[]={0xff,0xff,0x52};
    assert(hogp_report_map_parse(mixed,sizeof mixed,&map));
    assert(hogp_decode_report(&map,1,mixed_value,sizeof mixed_value,&keys)&&keys==(1ull<<RBP_KEY_UP));
    assert(!hogp_report_map_parse(mixed,11,&map)); /* vendor only */
    /* Unsubscribed ghost Input notifications cannot tear down the bridge.
     * A NULL server makes any accidental failure callback fail this test. */
    rc003_adapter_t adapter={0};
    adapter.char_count=1;adapter.chars[0].value_handle=0x123;
    adapter.chars[0].report_type=1;adapter.chars[0].report_id=3;
    rbp_gatt_evt_t notification={0};notification.type=RBP_GATT_EVT_NOTIFY;
    notification.handle=0x123;notification.value=actual;notification.len=sizeof actual;
    rc003_adapter_on_gatt(&adapter,&notification);
    puts("HOGP: padding, Push/Pop, report IDs, arrays, multi-key, malformed passed");return 0;
}

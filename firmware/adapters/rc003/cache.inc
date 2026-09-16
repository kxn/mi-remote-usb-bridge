/* Versioned wire representation, never a dump of pointers/runtime structs. */
#define CACHE_HEADER 48u
#define CACHE_CHAR 11u
static uint16_t cache_u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static void cache_put(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
size_t rc003_adapter_cache_export(const rc003_adapter_t *a,uint8_t *p,size_t cap) {
    size_t n=CACHE_HEADER+CACHE_CHAR*a->char_count+a->map_len;
    if(!a->cache_valid || !a->cache_safe || !a->cache_peer_id || !a->char_count ||
       a->char_count>RC003_MAX_REPORT_CHARS || a->map_len>HOGP_MAX_MAP_SIZE || n>cap)return 0;
    memset(p,0,n);memcpy(p,"GAC1",4);cache_put(p+4,(uint16_t)n);
    p[6]=(a->unicom.selected?1:0)|(a->rc003_boot_layout?2:0)|
         (a->voice_key_verified?4:0)|(a->atvv_present?8:0);p[7]=a->char_count;
    uint16_t h[]={a->protocol_mode_handle,a->hid_info_handle,a->map_value_handle,
        a->bat_value_handle,a->bat_cccd_handle,a->atvv_tx_handle,a->atvv_ctl_handle,
        a->atvv_ctl_cccd,a->atvv_audio_handle,a->atvv_audio_cccd,
        a->service_changed_handle,a->service_changed_cccd,a->unicom.fd,a->unicom.fd_cccd};
    for(unsigned i=0;i<14;i++)cache_put(p+8+2*i,h[i]);
    p[38]=a->hid_flags;p[39]=a->hid_info_valid;cache_put(p+40,a->map_len);
    for(unsigned i=0;i<a->char_count;i++) {
        const rc003_report_char_t *c=&a->chars[i];uint8_t *q=p+CACHE_HEADER+i*CACHE_CHAR;
        cache_put(q,c->value_handle);cache_put(q+2,c->cccd_handle);cache_put(q+4,c->ref_handle);
        q[6]=c->report_id;q[7]=c->report_type;q[8]=c->properties;q[9]=c->usage_page;q[10]=c->subscribed;
    }
    memcpy(p+CACHE_HEADER+CACHE_CHAR*a->char_count,a->map_buf,a->map_len);return n;
}
bool rc003_adapter_cache_import(rc003_adapter_t *a,uint32_t peer,const uint8_t *p,size_t n) {
    if(a->state!=ST_IDLE || !peer || n<CACHE_HEADER || memcmp(p,"GAC1",4) ||
       cache_u16(p+4)!=n || (p[6]&~15) || !p[7] || p[7]>RC003_MAX_REPORT_CHARS || p[39]>1)return false;
    uint16_t ml=cache_u16(p+40);
    if(ml>HOGP_MAX_MAP_SIZE || n!=CACHE_HEADER+CACHE_CHAR*p[7]+ml)return false;
    const uint8_t *map=p+CACHE_HEADER+CACHE_CHAR*p[7];
    bool unicom=ml==sizeof unicom_map && !memcmp(map,unicom_map,ml);
    bool xiaomi=ml==86 && rbp_crc32c(map,ml)==0x6bd7daad;
    if((p[6]&3)!=(unicom?1:xiaomi?2:0) || (!unicom&&!xiaomi))return false;
    for(unsigned i=0;i<p[7];i++) {
        const uint8_t *q=p+CACHE_HEADER+CACHE_CHAR*i;
        if(!cache_u16(q) || q[7]<1 || q[7]>3 || q[10]>1 ||
           (q[10] && !cache_u16(q+2)))return false;
        for(unsigned j=0;j<i;j++)if(cache_u16(q)==cache_u16(p+CACHE_HEADER+CACHE_CHAR*j))return false;
    }
    const rbp_gatt_client_t *g=a->gatt;void *u=a->gatt_user;rbp_server_t *s=a->server;
    rc003_adapter_init(a,g,u,s);
    a->unicom.selected=unicom;a->rc003_boot_layout=xiaomi;
    a->voice_key_verified=(p[6]&4)!=0;a->atvv_present=(p[6]&8)!=0;
    uint16_t *h[]={&a->protocol_mode_handle,&a->hid_info_handle,&a->map_value_handle,
        &a->bat_value_handle,&a->bat_cccd_handle,&a->atvv_tx_handle,&a->atvv_ctl_handle,
        &a->atvv_ctl_cccd,&a->atvv_audio_handle,&a->atvv_audio_cccd,
        &a->service_changed_handle,&a->service_changed_cccd,&a->unicom.fd,&a->unicom.fd_cccd};
    for(unsigned i=0;i<14;i++)*h[i]=cache_u16(p+8+2*i);
    a->hid_flags=p[38];a->hid_info_valid=p[39];a->char_count=p[7];a->map_len=ml;
    memcpy(a->map_buf,map,ml);
    for(unsigned i=0;i<a->char_count;i++) {
        rc003_report_char_t *c=&a->chars[i];const uint8_t *q=p+CACHE_HEADER+CACHE_CHAR*i;
        c->value_handle=cache_u16(q);c->cccd_handle=cache_u16(q+2);c->ref_handle=cache_u16(q+4);
        c->report_id=q[6];c->report_type=q[7];c->properties=q[8];c->usage_page=q[9];c->subscribed=q[10];
    }
    bool ok=unicom?(a->unicom.fd && a->unicom.fd_cccd && unicom_reports(a)):
        (hogp_report_map_parse(map,ml,&a->map) && a->atvv_present && a->atvv_tx_handle &&
         a->atvv_ctl_handle && a->atvv_ctl_cccd && a->atvv_audio_handle && a->atvv_audio_cccd);
    if(a->service_changed_handle && !a->service_changed_cccd)ok=false;
    if(!ok){rc003_adapter_init(a,g,u,s);return false;}
    a->atvv.profile_init_without_sync=xiaomi;
    a->cache_peer_id=peer;a->cache_safe=a->cache_valid=true;return true;
}

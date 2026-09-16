/* Shared GATT discovery, separate measured-profile runtime. */
static void unicom_end(rc003_adapter_t *a,uint8_t reason) {
    bool active=a->unicom.announced;
    a->unicom.active=false;a->unicom.started=false;a->unicom.closing=false;
    a->unicom.part=0;a->unicom.previous_valid=false;
    a->unicom.announced=false;a->unicom.pre_head=a->unicom.pre_count=0;
    if(active){rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=reason;
        rbp_server_on_voice(a->server,&ev,a->now_ms);}
}
static void unicom_bad(rc003_adapter_t *a,const char *why) {
    unicom_end(a,RBP_END_SOURCE_DATA_LOST);
    fail_initialization(a,why); /* close link; never try another vendor command */
}
static void unicom_stream_fault(rc003_adapter_t *a,const char *why) {
    unicom_end(a,RBP_END_SOURCE_DATA_LOST);
    fail_transport(a,why); /* transport/media loss is not a changed GATT database */
}
static bool unicom_reports(rc003_adapter_t *a) {
    static const uint8_t ids[]={1,3,0xfc,0xfb,0xf8,0xfa,0xf9,4};
    if(a->char_count!=sizeof ids)return false;
    for(unsigned j=0;j<sizeof ids;j++) {
        unsigned count=0;
        for(unsigned i=0;i<a->char_count;i++) {
            rc003_report_char_t *c=&a->chars[i];
            if(c->report_id!=ids[j])continue;
            count++;
            bool output=ids[j]==0xfb || ids[j]==0xfa;
            if(c->report_type!=(output?2:1) ||
               (output?!(c->properties&8):(!(c->properties&0x10)||!c->subscribed)))return false;
            if(ids[j]==0xfb)a->unicom.fb=c->value_handle;
            if(ids[j]==0xfc)a->unicom.fc=c->value_handle;
            if(ids[j]==0xf8)a->unicom.f8=c->value_handle;
        }
        if(count!=1)return false;
    }
    return true;
}
static void unicom_ready(rc003_adapter_t *a) {
    static const rbp_codec_t codecs[]={{RBP_CODEC_IFLYTEK_ICO,1}};
    static const rbp_audio_caps_t caps={codecs,40,1};
    a->state=ST_DONE;a->ready=true;a->voice_ready=a->unicom.hello;a->retry=0;
    a->cache_valid=a->cache_safe && a->cache_peer_id!=0 && a->voice_ready;
    a->restoring=false;
    rbp_server_set_profile(a->server,&RBP_PROFILE_UNICOM);
    rbp_server_set_voice_caps(a->server,&caps);
    rbp_server_on_voice_state(a->server,a->voice_ready?RBP_VOICE_READY:RBP_VOICE_UNSUPPORTED,
        a->voice_ready?RBP_VI_HTT:RBP_VI_UNAVAILABLE,a->voice_ready?16000:0);
    rbp_server_on_link(a->server,RBP_LINK_READY,0,a->now_ms);
}
static void unicom_stop(rc003_adapter_t *a) {
    if(!a->unicom.active || a->unicom.closing)return;
    a->unicom.closing=true;a->unicom.close_ms=a->now_ms;
    if(!a->unicom.announced)a->unicom.pre_head=a->unicom.pre_count=0;
    a->unicom.command=2; /* queued FB=00; cannot overlap an accepted write */
}
static void unicom_start_pending(rc003_adapter_t *a) {
    if(!a->unicom.pending_voice || !a->unicom.down || !a->ready || !a->voice_ready ||
       a->unicom.active)return;
    uint32_t owner=rbp_server_voice_capture_session(a->server);
    if(!owner || owner!=a->unicom.capture_session){a->unicom.pending_voice=false;return;}
    a->unicom.pending_voice=false;
    DT(DT_HID,DT_INFO,21,a->state,a->unicom.down,1,a->now_ms);
    a->unicom.active=true;a->unicom.started=false;a->unicom.closing=false;
    a->unicom.part=0;a->unicom.previous_valid=false;a->unicom.command=1;
    a->unicom.last_audio_ms=a->unicom.capture_ms=a->now_ms;
    a->unicom.pre_head=a->unicom.pre_count=0;
    a->unicom.announced=rbp_server_voice_wanted(a->server);
    if(a->unicom.announced){rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
        rbp_server_on_voice(a->server,&ev,a->now_ms);}

}
static bool unicom_emit(rc003_adapter_t *a,const uint8_t *p) {
    if(!rbp_server_voice_wanted(a->server) ||
       rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session)return false;
    if(!a->unicom.announced) {
        if(a->unicom.closing)return false;
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
        a->unicom.announced=true;rbp_server_on_voice(a->server,&ev,a->now_ms);
    }
    if(!a->unicom.started) {
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;
        ev.u.start=(rbp_audio_format_t){RBP_CODEC_IFLYTEK_ICO,16000,40,1,0,1,NULL};
        a->unicom.started=true;rbp_server_on_voice(a->server,&ev,a->now_ms);
    }
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_ENCODED;
    ev.u.encoded.data=p;ev.u.encoded.len=40;ev.u.encoded.unit_size=40;ev.u.encoded.samples=320;
    rbp_server_on_voice(a->server,&ev,a->now_ms);return true;
}
static void unicom_preroll_cancel(rc003_adapter_t *a,uint8_t reason) {
    DT(DT_HID,DT_INFO,23,reason,a->unicom.pre_count,a->now_ms-a->unicom.capture_ms,0);
    if(reason)rbp_fault_record(RBP_FAULT_ADAPTER,0x90,reason,a->unicom.pre_count);
    a->unicom.pre_count=0;unicom_stop(a);
}
static void unicom_tick(rc003_adapter_t *a) {
    if(a->state==ST_UNICOM_WAIT) {
        if(a->unicom.hello || (int32_t)(a->now_ms-a->deadline_ms)>=0)unicom_ready(a);
        return;
    }
    if(a->state!=ST_DONE || !a->voice_ready)return;
    unicom_start_pending(a);
    if(a->unicom.active) {
        if(rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session ||
           (a->unicom.announced && !rbp_server_voice_wanted(a->server)))unicom_preroll_cancel(a,0);
        if(!a->unicom.announced && !a->unicom.closing && rbp_server_voice_wanted(a->server)) {
            a->unicom.announced=true;rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
            rbp_server_on_voice(a->server,&ev,a->now_ms);
        }
        if(!a->unicom.announced && !a->unicom.closing &&
           a->now_ms-a->unicom.capture_ms>=250)unicom_preroll_cancel(a,RBP_END_CONSUMER_DISABLED);
        /* Drain at most one unit per 5ms turn; never burst eight into USB's
         * small media queue. Live FC appends behind retained units. */
        if(a->unicom.pre_count && unicom_emit(a,a->unicom.preroll+40*a->unicom.pre_head)) {
            a->unicom.pre_head=(a->unicom.pre_head+1)%8;a->unicom.pre_count--;
        }
    }
    if(a->unicom.closing && a->unicom.command && !a->unicom.in_flight && a->now_ms-a->unicom.close_ms>=2000) {
        unicom_stream_fault(a,"Unicom stop submission timeout");return;
    }
    if(a->unicom.in_flight) {
        if((int32_t)(a->now_ms-a->unicom.command_deadline)>=0)
            unicom_stream_fault(a,"Unicom command timeout");
        return;
    }
    if(a->unicom.active && a->unicom.closing && !a->unicom.command &&
       ((a->now_ms-a->unicom.last_audio_ms>=300 && a->now_ms-a->unicom.close_ms>=300) ||
        a->now_ms-a->unicom.close_ms>=1000)) {
        unicom_end(a,a->unicom.part?RBP_END_SOURCE_DATA_LOST:
            a->unicom.started?RBP_END_NORMAL:RBP_END_DEVICE_ERROR);return;
    }
    if(a->unicom.active && !a->unicom.closing) {
        if(a->now_ms-a->unicom.last_audio_ms>=2000){unicom_stream_fault(a,"Unicom audio timeout");return;}
        if(a->unicom.started && (int32_t)(a->now_ms-a->unicom.refresh_ms)>=0)
            a->unicom.command=1;
    }
    if(a->unicom.command) {
        uint8_t cmd=a->unicom.command,value=cmd==1?1:0;
        /* Install state before submission: simulated transports may complete inline. */
        a->unicom.in_flight=cmd;a->unicom.command=0;
        a->unicom.command_deadline=a->now_ms+ATT_RESPONSE_TIMEOUT_MS;
        int rc=a->gatt->write_value(a->gatt_user,a->unicom.fb,&value,1);
        if(rc==RBP_GATT_RETRY){a->unicom.in_flight=0;a->unicom.command=cmd;}
        else if(rc<0){a->unicom.in_flight=0;unicom_stream_fault(a,"Unicom command rejected");}
        else if(cmd==1)a->unicom.refresh_ms=a->now_ms+5000;
    }
}
static void unicom_keys(rc003_adapter_t *a,unsigned index,const uint8_t *p,unsigned n) {
    uint8_t id=a->chars[index].report_id;
    if((id==1 && (n!=8 || p[0] || p[1])) || (id==3 && n!=2)) {
        unicom_bad(a,"Unicom key layout");return;
    }
    for(unsigned k=id==1?2:0;k<(id==1?8:1);k++) {
        unsigned usage=id==1?p[k]:(p[0]|((uint16_t)p[1]<<8));
        bool known=usage==0;
        for(unsigned j=0;j<sizeof(unicom_keys_map)/sizeof(unicom_keys_map[0]);j++)
            if(unicom_keys_map[j].report==id && unicom_keys_map[j].usage==usage)known=true;
        if(!known){unicom_bad(a,"Unicom unknown key report");return;}
    }
    uint64_t bits=0;
    for(unsigned j=0;j<sizeof(unicom_keys_map)/sizeof(unicom_keys_map[0]);j++) {
        if(unicom_keys_map[j].report!=id)continue;
        bool found=false;
        if(id==1){for(unsigned k=2;k<8;k++)if(p[k]==unicom_keys_map[j].usage)found=true;}
        else found=(p[0]|((uint16_t)p[1]<<8))==unicom_keys_map[j].usage;
        if(found)bits|=(uint64_t)1<<j; /* slots exactly match measured catalog */
    }
    a->keys_by_report[index]=bits;
    bits=0;for(unsigned i=0;i<a->char_count;i++)bits|=a->keys_by_report[i];
    if(bits!=a->pressed_bits){a->pressed_bits=bits;key_state_send(a,bits);}
}
static void unicom_notify(rc003_adapter_t *a,const rbp_gatt_evt_t *e) {
    if(!e->value){unicom_bad(a,"Unicom empty notification");return;}
    if(e->handle==a->unicom.fd) {
        static const uint8_t hello[]={0x10,1,0x4b,0x2a,3,2,4,0,2,3};
        if(e->len!=sizeof hello || memcmp(e->value,hello,sizeof hello)) {
            unicom_bad(a,"Unicom initialization mismatch");return;
        }
        bool first=!a->unicom.hello;a->unicom.hello=true;
        if(first && (a->state==ST_UNICOM_WAIT || a->state==ST_DONE))unicom_ready(a);
        return; /* CCCD ACK remains owned by discovery */
    }
    if(e->handle==a->unicom.f8) {
        bool valid=e->len==20 && e->value[0]==0x82 && e->value[1]==3 && e->value[2]<=1;
        for(unsigned i=3;valid && i<20;i++)if(e->value[i])valid=false;
        if(!valid){unicom_bad(a,"Unicom F8 mismatch");return;}
        bool down=e->value[2]!=0,edge=down&&!a->unicom.down;a->unicom.down=down;
        DT(DT_HID,DT_INFO,20,down,a->state,rbp_server_voice_wanted(a->server),rbp_server_voice_capture_session(a->server));
        if(!down){a->unicom.pending_voice=false;unicom_stop(a);return;}
        /* Preserve a real down edge received while restoring subscriptions.
         * This includes READY before host VOICE_ENABLE; the product policy
         * distinguishes pending negotiation from explicit disable per session.
         * Release/detach cancels it; never infer a press from link wakeup. */
        if(edge && !a->unicom.active) {
            a->unicom.capture_session=rbp_server_voice_capture_session(a->server);
            a->unicom.pending_voice=a->unicom.capture_session!=0;
        }
        unicom_start_pending(a);
        return;
    }
    if(!a->ready)return;
    if(e->handle==a->unicom.fc) {
        if(!a->unicom.active)return;
        const uint8_t *p=e->value;
        if(e->len!=20){unicom_bad(a,"Unicom FC length");return;}
        uint16_t seq=p[0]|((uint16_t)p[1]<<8),part=p[2]|((uint16_t)p[3]<<8);
        if(part!=a->unicom.part || part>2 || (part && seq!=a->unicom.sequence) ||
            (!part && a->unicom.previous_valid && seq!=(uint16_t)(a->unicom.previous+1))) {
            /* Keep wire counters before stream cleanup destroys them. No audio
             * payload is logged. 0xffff means no previous unit at stream start. */
            uint16_t expected=a->unicom.part?a->unicom.sequence:
                a->unicom.previous_valid?(uint16_t)(a->unicom.previous+1):0xffffu;
            rbp_fault_record(RBP_FAULT_ADAPTER,0x91,
                (uint32_t)seq|((uint32_t)part<<16),
                (uint32_t)expected|((uint32_t)a->unicom.part<<16));
            DT(DT_HID,DT_ERROR,24,seq,part,expected,a->unicom.part);
            unicom_stream_fault(a,"Unicom FC continuity");return;
        }
        if(!part)a->unicom.sequence=seq;
        memcpy(a->unicom.body+16*part,p+4,16);a->unicom.part++;
        a->unicom.last_audio_ms=a->now_ms;
        if(part!=2)return;
        p=a->unicom.body;
        if((p[42]|((uint16_t)p[43]<<8))!=seq || memcmp(p+44,p+28,4)) {
            unicom_bad(a,"Unicom FC trailer");return;
        }
        a->unicom.part=0;a->unicom.previous=seq;a->unicom.previous_valid=true;
        if(!a->unicom.announced && a->unicom.closing)return;
        if(rbp_server_voice_capture_session(a->server)!=a->unicom.capture_session) {
            unicom_preroll_cancel(a,0);return;
        }
        if(!a->unicom.pre_count && unicom_emit(a,p))return;
        if(a->unicom.announced && !rbp_server_voice_wanted(a->server)){unicom_stop(a);return;}
        if(a->unicom.pre_count==8){
            if(a->unicom.announced)unicom_stream_fault(a,"Unicom buffered audio overrun");
            else unicom_preroll_cancel(a,RBP_END_BUFFER_OVERRUN);
            return;
        }
        memcpy(a->unicom.preroll+40*((a->unicom.pre_head+a->unicom.pre_count)%8),p,40);
        a->unicom.pre_count++;
        DT(DT_HID,DT_DETAIL,22,a->unicom.pre_count,seq,a->unicom.announced,0);
        return;
    }
    for(unsigned i=0;i<a->char_count;i++)if(a->chars[i].value_handle==e->handle) {
        unsigned id=a->chars[i].report_id;
        if(id==1 || id==3)unicom_keys(a,i,e->value,e->len);
        else if((id==0xf9 && e->len!=1) || (id==4 && e->len!=20))unicom_bad(a,"Unicom extension length");
        return;
    }
    if(e->handle==a->bat_value_handle && e->len==1 && e->value[0]<=100)
        rbp_server_on_battery(a->server,e->value[0],0);
}

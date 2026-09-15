/* Reconnect restores only an explicitly enabled session/bond intent. */
static void test_voice_resume_intent(void) {
    for(unsigned mode=0;mode<7;mode++) {
        rbp_server_t*s=server_new();client_reset();reset_frames();sink_blocked=false;
        client_hello(s,11);tick(s,12);rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
        rbp_peer_record_t peer={0};peer.peer_id=77;peer.auto_reconnect=true;rbp_server_on_peer_changed(s,&peer);
        rbp_server_on_link(s,RBP_LINK_READY,900,40);rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);
        rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,true);
        client_request_conn(s,50,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);tick(s,51);
        rbp_server_get_info(s,&info);CHECK(info.voice_enabled);
        if(mode==1){rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,false);client_request_conn(s,52,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);}
        rbp_server_on_link(s,RBP_LINK_DISCONNECTED,0,60);
        rbp_server_get_info(s,&info);CHECK(!info.voice_enabled);
        if(mode==2){rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,false);client_request_conn(s,62,RBP_OP_VOICE_ENABLE,w.buf,w.len,0);}
        if(mode==3)rbp_server_on_usb_gone(s,62);
        if(mode==4){peer.peer_id=78;rbp_server_on_peer_changed(s,&peer);}
        if(mode==5){static const rbp_codec_t c[]={{99,1}};static const rbp_audio_caps_t caps={c,512,1};rbp_server_set_voice_caps(s,&caps);}
        if(mode==6)client_request_conn(s,62,RBP_OP_DISCONNECT,NULL,0,0);
        rbp_server_on_link(s,RBP_LINK_INITIALIZING,901,70);
        rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);
        rbp_server_on_link(s,RBP_LINK_READY,0,71);
        rbp_server_get_info(s,&info);CHECK(info.voice_enabled==(mode==0));
        server_free();
    }
}

/* RBP3 source burst with no timer pacing: exact compressed bytes and END. */
static void test_voice_burst_without_tick(void)
{
    rbp_server_t*s=server_new();client_reset();reset_frames();sink_blocked=false;
    client_hello(s,11);tick(s,12);rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
    rbp_server_on_link(s,RBP_LINK_READY,900,40);rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);tick(s,41);
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,true);
    client_request_conn(s,50,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);tick(s,51);
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;feed_voice(s,60,&ev);
    for(unsigned packet=0;packet<40;packet++) {
        uint8_t bytes[64];for(unsigned i=0;i<64;i++)bytes[i]=(uint8_t)(packet*64+i);
        ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=bytes;ev.u.encoded.len=64;
        ev.u.encoded.unit_size=64;ev.u.encoded.offset=0;ev.u.encoded.samples=128;
        feed_voice(s,60,&ev);rbp_server_flush_output(s);
    }
    ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=RBP_END_NORMAL;feed_voice(s,60,&ev);
    parse_pending();for(unsigned i=0;i<8;i++)tick(s,61+i);
    uint64_t total=0;unsigned units=0,ended=0;
    for(size_t i=0;i<frame_count;i++) {
        frame_t*f=&frames[i];CHECK(f->crc_ok);
        if(f->hdr.opcode==RBP_OP_VOICE_DATA) {
            uint32_t seq,unit,offset;uint64_t index;
            memcpy(&seq,f->payload+4,4);memcpy(&unit,f->payload+12,4);
            memcpy(&offset,f->payload+20,4);memcpy(&index,f->payload+24,8);
            CHECK(seq==units+1&&unit==units+1&&offset==0&&index==total*2);
            for(unsigned j=40;j<f->len;j++)CHECK(f->payload[j]==(uint8_t)(total+j-40));
            total+=f->len-40;units++;
        }
        if(f->hdr.opcode==RBP_OP_VOICE_ENDED_EV) {
            uint8_t reason;uint64_t samples;CHECK(parse_ended(f,&reason,&samples));
            CHECK(reason==0&&samples==5120);ended++;
        }
    }
    CHECK(total==2560&&units==40&&ended==1);
    reset_frames();out_len=0;sink_blocked=true;
    ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;feed_voice(s,70,&ev);
    for(unsigned i=0;i<100;i++) {
        ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=encoded_block;ev.u.encoded.len=120;
        ev.u.encoded.unit_size=120;ev.u.encoded.offset=0;ev.u.encoded.samples=240;feed_voice(s,70,&ev);
    }
    sink_blocked=false;for(unsigned i=0;i<12;i++){rbp_server_flush_output(s);parse_pending();}
    ended=0;
    for(size_t i=0;i<frame_count;i++)if(frames[i].hdr.opcode==RBP_OP_VOICE_ENDED_EV) {
        uint8_t reason;uint64_t samples;CHECK(parse_ended(&frames[i],&reason,&samples));
        CHECK(reason==RBP_END_BUFFER_OVERRUN);ended++;
    }
    CHECK(ended==1);server_free();
}

static void test_voice_fragment_epoch_unknown(void) {
    rbp_server_t*s=server_new();client_reset();reset_frames();sink_blocked=false;
    client_hello(s,11);tick(s,12);rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
    rbp_server_on_link(s,RBP_LINK_READY,900,40);rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);tick(s,41);
    client_request_conn(s,42,RBP_OP_GET_VOICE_CAPS,NULL,0,900);tick(s,43);
    frame_t*cap=find_frame(RBP_KIND_RESPONSE,RBP_OP_GET_VOICE_CAPS);CHECK(cap&&cap->hdr.status==0);
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,true);
    client_request_conn(s,50,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);tick(s,51);
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;feed_voice(s,60,&ev);
    uint8_t bytes[512];for(unsigned i=0;i<512;i++)bytes[i]=(uint8_t)i;
    ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=bytes;ev.u.encoded.len=512;
    ev.u.encoded.unit_size=512;ev.u.encoded.offset=0;ev.u.encoded.samples=UINT32_MAX;feed_voice(s,61,&ev);tick(s,62);
    ev.type=RBP_VOICE_EVT_FORMAT;ev.u.format.sample_rate=16000;feed_voice(s,63,&ev);tick(s,64);
    ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=bytes;ev.u.encoded.len=5;
    ev.u.encoded.unit_size=10;ev.u.encoded.offset=0;ev.u.encoded.samples=20;feed_voice(s,65,&ev);tick(s,66);
    ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=RBP_END_NORMAL;feed_voice(s,67,&ev);
    for(unsigned i=68;i<78;i++)tick(s,i);
    unsigned parts=0,formats=0,ends=0;uint32_t expected_offset=0;
    for(size_t i=0;i<frame_count;i++) {
        frame_t*f=&frames[i];CHECK(f->crc_ok);
        if(f->hdr.opcode==RBP_OP_VOICE_DATA){
            uint32_t unit,offset;memcpy(&unit,f->payload+12,4);memcpy(&offset,f->payload+20,4);
            if(unit==1){CHECK(offset==expected_offset);for(unsigned j=40;j<f->len;j++)CHECK(f->payload[j]==(uint8_t)(offset+j-40));expected_offset+=f->len-40;parts++;}
        }
        if(f->hdr.opcode==RBP_OP_VOICE_FORMAT_EV){
            rbp_tlv_reader_t r;bool found;uint32_t epoch;uint64_t index;rbp_tlv_reader_init(&r,f->payload,f->len);
            CHECK(rbp_tlv_get_u32(&r,2,&epoch,&found)==RBP_TLV_OK&&found&&epoch==2);
            CHECK(rbp_tlv_get_u64(&r,9,&index,&found)==RBP_TLV_OK&&found&&index==UINT64_MAX);formats++;
        }
        if(f->hdr.opcode==RBP_OP_VOICE_ENDED_EV){uint8_t reason;uint64_t samples;CHECK(parse_ended(f,&reason,&samples));CHECK(reason==RBP_END_INVALID_ENCODED_DATA&&samples==UINT64_MAX);ends++;}
    }
    CHECK(expected_offset==512&&parts==2&&formats==1&&ends==1);server_free();
}

static rbp_server_t *setup_audio_regression(void) {
    out_len=0;sink_blocked=false;rbp_server_t*s=server_new();client_reset();reset_frames();
    client_hello(s,11);tick(s,12);rbp_server_info_t info;rbp_server_get_info(s,&info);g_session=info.session_id;
    rbp_server_on_link(s,RBP_LINK_READY,900,40);rbp_server_on_voice_state(s,RBP_VOICE_READY,RBP_VI_HTT,16000);tick(s,41);
    rbp_tlv_writer_t w;rbp_tlv_writer_init(&w);rbp_tlv_put_bool(&w,1,true);
    client_request_conn(s,50,RBP_OP_VOICE_ENABLE,w.buf,w.len,900);tick(s,51);reset_frames();
    return s;
}

static void test_audio_fault_outcomes(void) {
    /* A late first packet is allowed; total absence is a device timeout,
     * not evidence of malformed encoded bytes. Keep the host lease alive. */
    for(unsigned silence=0;silence<2;silence++) {
        rbp_server_t*s=setup_audio_regression();rbp_voice_evt_t ev={0};
        ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;feed_voice(s,60,&ev);
        tick(s,1062);CHECK(find_frame(RBP_KIND_EVENT,RBP_OP_VOICE_ENDED_EV)==NULL);
        client_request(s,4000,RBP_OP_GET_DEVICE,NULL,0);
        if(!silence) {
            ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=encoded_block;ev.u.encoded.len=120;
            ev.u.encoded.unit_size=120;ev.u.encoded.offset=0;ev.u.encoded.samples=240;feed_voice(s,4500,&ev);
        }
        tick(s,5060);frame_t*f=find_frame(RBP_KIND_EVENT,RBP_OP_VOICE_ENDED_EV);
        if(silence){uint8_t reason;uint64_t samples;CHECK(f&&parse_ended(f,&reason,&samples));if(f)CHECK(reason==RBP_END_DEVICE_ERROR&&samples==0);}
        else CHECK(f==NULL);
        server_free();
    }
    for(unsigned timeout=0;timeout<2;timeout++) {
        rbp_server_t*s=setup_audio_regression();rbp_voice_evt_t ev={0};
        ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;rbp_server_on_voice(s,&ev,60);
        if(!timeout){ev.type=RBP_VOICE_EVT_FAULT;ev.u.end.reason=RBP_END_INVALID_ENCODED_DATA;rbp_server_on_voice(s,&ev,61);}
        if(timeout) {
            tick(s,1062);rbp_server_info_t pending;rbp_server_get_info(s,&pending);
            CHECK(pending.voice_enabled&&pending.voice_state==RBP_VOICE_READY);
            client_request(s,4000,RBP_OP_GET_DEVICE,NULL,0);reset_frames();
        }
        tick(s,timeout?5060:62);rbp_server_info_t info;rbp_server_get_info(s,&info);
        CHECK(info.voice_state==RBP_VOICE_FAILED && info.session_active && !info.voice_enabled);
        frame_t *state=find_frame(RBP_KIND_EVENT,RBP_OP_DEVICE_STATE_EV);CHECK(state!=NULL);
        if(state){rbp_tlv_reader_t r;uint16_t reason=0;bool found=false;rbp_tlv_reader_init(&r,state->payload,state->len);CHECK(rbp_tlv_get_u16(&r,16,&reason,&found)==RBP_TLV_OK&&found&&reason==RBP_STATUS_DEVICE_ERROR);}
        CHECK(find_frame(RBP_KIND_EVENT,RBP_OP_VOICE_ENDED_EV)==NULL);server_free();
    }
    for(unsigned mode=0;mode<3;mode++) {
        g_test_capture_ms=1000;rbp_server_t*s=setup_audio_regression();g_test_capture_ms=120000;
        rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_START;ev.u.start.sample_rate=16000;feed_voice(s,60,&ev);tick(s,61);
        ev.type=RBP_VOICE_EVT_ENCODED;ev.u.encoded.data=encoded_block;ev.u.encoded.len=5;
        ev.u.encoded.unit_size=mode==2?5:10;ev.u.encoded.offset=0;ev.u.encoded.samples=mode==2?10:20;feed_voice(s,62,&ev);tick(s,63);
        tick(s,1060);
        if(mode==0){disconnect_sync=true;tick(s,3060);disconnect_sync=false;} /* synchronous backend also preserves error END */
        else {ev.type=RBP_VOICE_EVT_END;ev.u.end.reason=mode==1?RBP_END_DEVICE_ERROR:RBP_END_NORMAL;feed_voice(s,1061,&ev);tick(s,1062);}
        frame_t*end=find_frame(RBP_KIND_EVENT,RBP_OP_VOICE_ENDED_EV);CHECK(end!=NULL);
        if(end){uint8_t reason;uint64_t samples;CHECK(parse_ended(end,&reason,&samples));CHECK(reason==(mode==2?RBP_END_CAPTURE_LIMIT:RBP_END_DEVICE_ERROR));CHECK(samples==(mode==2?10:0));}
        server_free();
    }
}

static void test_host_voice_start(void) {
    rbp_server_t *s=setup_audio_regression();
    voice_start_calls=0;voice_start_status=RBP_STATUS_OK;
    client_request_conn(s,70,RBP_OP_VOICE_START,NULL,0,899);tick(s,71);
    CHECK(find_frame(RBP_KIND_RESPONSE,RBP_OP_VOICE_START)->hdr.status==RBP_STATUS_LINK_LOST);
    CHECK(!voice_start_calls);reset_frames();
    client_request_conn(s,72,RBP_OP_VOICE_START,NULL,0,900);tick(s,73);
    CHECK(find_frame(RBP_KIND_RESPONSE,RBP_OP_VOICE_START)->hdr.status==RBP_STATUS_OK);
    CHECK(voice_start_calls==1);reset_frames();
    voice_start_status=RBP_STATUS_BUSY;
    client_request_conn(s,74,RBP_OP_VOICE_START,NULL,0,900);tick(s,75);
    CHECK(find_frame(RBP_KIND_RESPONSE,RBP_OP_VOICE_START)->hdr.status==RBP_STATUS_BUSY);
    CHECK(voice_start_calls==2);reset_frames();
    rbp_voice_evt_t ev={0};ev.type=RBP_VOICE_EVT_SOURCE_BEGIN;
    rbp_server_on_voice(s,&ev,76);
    client_request_conn(s,77,RBP_OP_VOICE_START,NULL,0,900);tick(s,78);
    CHECK(find_frame(RBP_KIND_RESPONSE,RBP_OP_VOICE_START)->hdr.status==RBP_STATUS_BUSY);
    CHECK(voice_start_calls==2);
    server_free();voice_start_status=RBP_STATUS_OK;
}

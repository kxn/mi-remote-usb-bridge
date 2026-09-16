/* Optional GATT cache; separate from bond intent/key storage. Two 512-byte
 * slots in data EEPROM. CRC + final commit; zero-length newest is a tombstone. */
#define GAC_CRC 504u
#define GAC_COMMIT 508u
#define GAC_WORD 0x31434147u
static int gac_slot=-1;
static uint32_t gac_generation;
static bool gac_loaded, gac_io_failed;
static bool gac_valid(const uint8_t *p) {
    return !memcmp(p,"GCS1",4) && get32(p+GAC_COMMIT)==GAC_WORD &&
        get32(p+GAC_CRC)==rbp_crc32c(p,GAC_CRC) && get32(p+12)<=BOARD_CACHE_MAX;
}
static bool gac_read_latest(uint8_t gac_page[512]) {
    gac_io_failed=false;
    if(!gac_loaded) {
        gac_slot=-1;gac_generation=0;
        for(unsigned i=0;i<2;i++) {
            if(checked_read(BOARD_CACHE_BASE+i*512,gac_page,512)){gac_io_failed=true;return false;}
            if(gac_valid(gac_page) && (gac_slot<0 || get32(gac_page+4)>gac_generation)) {
                gac_slot=(int)i;gac_generation=get32(gac_page+4);
            }
        }
        gac_loaded=true;
    }
    if(gac_slot<0)return false;
    if(checked_read(BOARD_CACHE_BASE+(unsigned)gac_slot*512,gac_page,512)){gac_io_failed=true;return false;}
    return gac_valid(gac_page);
}
size_t board_cache_load(uint32_t peer,uint8_t *out,size_t cap) {
    uint8_t gac_page[512];
    load_slots();
    if(!peer || storage_state!=1 || get32(current+16)!=peer || !gac_read_latest(gac_page) ||
       get32(gac_page+8)!=peer || get32(gac_page+12)>cap)return 0;
    size_t n=get32(gac_page+12);memcpy(out,gac_page+16,n);return n;
}
bool board_cache_save(uint32_t peer,const uint8_t *data,size_t n) {
    uint8_t gac_page[512];
    load_slots();
    if(!peer || storage_state!=1 || get32(current+16)!=peer || n>BOARD_CACHE_MAX || (n&&!data))return false;
    bool have=gac_read_latest(gac_page);
    if(!gac_loaded || gac_io_failed)return false; /* read error, never erase an unknown slot */
    if(have && get32(gac_page+8)==peer && get32(gac_page+12)==n && (!n || !memcmp(gac_page+16,data,n)))return true;
    if(!have && !n)return true;
    if(gac_generation==UINT32_MAX)return false;
    memset(gac_page,255,sizeof gac_page);memcpy(gac_page,"GCS1",4);
    put32(gac_page+4,gac_generation+1);put32(gac_page+8,peer);put32(gac_page+12,(uint32_t)n);
    if(n)memcpy(gac_page+16,data,n);
    put32(gac_page+GAC_CRC,rbp_crc32c(gac_page,GAC_CRC));
    unsigned slot=gac_slot<0?0:1-(unsigned)gac_slot;uint32_t addr=BOARD_CACHE_BASE+slot*512;
    for(unsigned i=0;i<2;i++)if(checked_erase(addr+i*256,256))return false;
    if(checked_write(addr,gac_page,GAC_COMMIT))return false;
    uint8_t check[32];
    for(unsigned off=0;off<GAC_COMMIT;off+=sizeof check) {
        unsigned count=GAC_COMMIT-off;if(count>sizeof check)count=sizeof check;
        if(checked_read(addr+off,check,count) || checked_verify(gac_page+off,check,count))return false;
    }
    put32(gac_page+GAC_COMMIT,GAC_WORD);
    if(checked_write(addr+GAC_COMMIT,gac_page+GAC_COMMIT,4) ||
       checked_read(addr+GAC_COMMIT,check,4) || checked_verify(gac_page+GAC_COMMIT,check,4))return false;
    gac_slot=(int)slot;gac_generation++;return true;
}

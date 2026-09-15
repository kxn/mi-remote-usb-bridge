/* WCH V2.1 initiator identity-address erratum. See docs/wch-smp-identity-0.3.4.md.
 * The vendor archive is unchanged; --wrap redirects its SM dispatch callback.
 * Private offsets MUST NOT be reused with a different archive (build hash gate).
 */
#include "CH58xBLE_LIB.h"
#include "../debug/trace.h"
#include <stddef.h>
#include <string.h>

#if UINTPTR_MAX == UINT32_MAX
typedef char check_link_pairing_offset[(offsetof(linkDBItem_t,pPairingParams)==52)?1:-1];
#endif
enum { ID_ADDR_OPCODE=9, WAIT_ID_ADDR=0x25, PAIR_IDENTITY_OFFSET=120,
       IDENTITY_TYPE_OFFSET=22 };
extern uint8_t __real_smpInitiatorProcessIncoming(linkDBItem_t *,uint8_t,void *);

uint8_t __wrap_smpInitiatorProcessIncoming(linkDBItem_t *link,uint8_t opcode,void *parsed) {
    if(link && link->pPairingParams && parsed && opcode==ID_ADDR_OPCODE) {
        uint8_t *pair=link->pPairingParams;
        uint16_t handle;
        memcpy(&handle,pair,sizeof handle);
        if(handle==link->connectionHandle && pair[2] && pair[3]==WAIT_ID_ADDR) {
            uint8_t *id;
            memcpy(&id,pair+PAIR_IDENTITY_OFFSET,sizeof id);
            if(id) {
                uint8_t type=*(const uint8_t *)parsed;
                if(type>1)return SMP_PAIRING_FAILED_INVALID_PARAMERERS;
                /* Parsed Identity Address Information is {type, address[6]}.
                 * Correct BEFORE the real handler can finish pairing/persist.
                 * Never log the overwritten value: it is an IRK byte. */
                id[IDENTITY_TYPE_OFFSET]=type;
                DT(DT_BLE,DT_INFO,6,link->connectionHandle,type,0,0);
            }
        }
    }
    return __real_smpInitiatorProcessIncoming(link,opcode,parsed);
}

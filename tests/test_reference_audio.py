"""Execute the archived Telink Google decoder, not a rewritten test oracle.

Extract its original standalone function/tables into a temporary desktop TU;
retain its copyright header. No vendor firmware, radio or installer is run.
"""
from pathlib import Path
import hashlib
import re
import subprocess

root=Path(__file__).resolve().parents[1]
source=root/'references/sources/telink-atvv/application/audio/adpcm.c'
text=source.read_text()
tables=text[text.index('static const signed char idxtbl'):text.index('#define     NUM_OF_ORIG_SAMPLE')]
section=text[text.index('#elif (TL_AUDIO_MODE == TL_AUDIO_DONGLE_ADPCM_GATT_GOOGLE)'):]
function=section[section.index('_attribute_ram_code_ void adpcm_to_pcm'):section.index('#elif',10)]
header=text[:text.index('#include')]
test=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ima_decoder.h"
int main(void) {
    unsigned cases=0;
    const int16_t preds[]={-32768,-12345,0,12345,32767};
    for(unsigned index=0;index<89;index++)for(unsigned p=0;p<5;p++) {
        /* Original function reads a 6-byte v0.4 header for predictor/index;
         * decoded nibble algorithm and order are also used for v1.0 data.
         * Extra trailing padding accommodates its look-ahead byte load. */
        uint16_t packet[132]={0};uint8_t *bytes=(uint8_t*)packet;
        bytes[3]=(uint16_t)preds[p]>>8;bytes[4]=(uint8_t)preds[p];bytes[5]=index;
        for(unsigned b=0;b<256;b++)bytes[6+b]=(uint8_t)b;
        int16_t expected[512],actual[512];
        adpcm_to_pcm((short*)packet,expected,512);
        ima_state_t state;ima_state_reset(&state,preds[p],index);
        assert(ima_decode(&state,bytes+6,256,actual,512,NULL)==512);
        if(memcmp(expected,actual,sizeof actual)) {
            fprintf(stderr,"Telink mismatch at initial index=%u predictor=%d\n",index,preds[p]);return 1;
        }
        /* Cross-callback state is equivalent to one contiguous packet. */
        ima_state_reset(&state,preds[p],index);
        for(unsigned b=0;b<256;b+=16)ima_decode(&state,bytes+6+b,16,actual+b*2,32,&state);
        assert(!memcmp(expected,actual,sizeof actual));cases++;
    }
    printf("Telink original decoder: %u state/byte-sequence cases, 227840 PCM samples, fragmentation passed\n",cases);
}
'''
build=root/'build/reference-audio';build.mkdir(exist_ok=True)
unit=build/'telink_comparison.c'
unit.write_text(header+'\n#include <stdint.h>\ntypedef int16_t s16;typedef int8_t s8;\n#define _attribute_ram_code_\n#define NUM_OF_ORIG_SAMPLE 2\n'+tables+function+test)
subprocess.run(['C:/msys64/clang64/bin/clang.exe','-std=c11','-O2','-I'+str(root/'client/c'),str(unit),str(root/'client/c/ima_decoder.c'),'-o',str(build/'compare.exe')],check=True)
print('Original reference SHA256:',hashlib.sha256(source.read_bytes()).hexdigest(),flush=True)
subprocess.run([str(build/'compare.exe')],check=True)

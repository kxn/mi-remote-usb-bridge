# RBP/3 codec registry: iFLYTEK ICO, ID 2, revision 1

This profile describes the measured Unicom remote `XFRSD0D_U_R01_846_40091`.
It does not identify a device by brand, name or codec alone.

| Field | Value |
| --- | --- |
| codec_id / codec_revision | 2 / 1 |
| sample_rate / channels | 16000 / 1 |
| codec_config | empty |
| max_unit_bytes | 40 |
| one coding unit | exactly 40 original ICO bytes |
| samples per unit | 320 mono sample frames, 20 ms |
| host PCM | signed 16-bit little endian |

BLE FC carries three 20-byte values: a little-endian u16 group sequence and u16
fragment index (0, 1, 2), followed by 16 content bytes. Concatenate the contents;
validate the trailer's repeated sequence at byte 42 and repeated bytes 28..31 at
44..47. Deliver only content bytes 0..39 through RBP. The first group sequence
may be arbitrary, subsequent groups increment modulo 65536. No BLE headers,
Report ID or trailing metadata enter the coding unit. RBP unit/fragment counters
remain independent of BLE group counters.

Interpret ICO bytes as 20 little-endian u16 words. Reorder using
`[0,1,18,8,9,5,2,17,11,16,10,3,12,7,14,15,4,13,6,19]`, XOR each with `0x0416`,
then decode those words MSB-first using the fixed-point G.722.1 reference codec,
7 kHz mode, 14 regions, 320 bits per frame. Clear the two least significant bits
of each output sample. Emit all 320 samples; do not additionally crop decoder
delay or prepend silence.

Each START or FORMAT initializes zero coefficient/overlap history and four
noise-fill seeds `{1,1,1,1}`. Keep state between units. A repeated BLE FB=01
refresh does not create a new epoch or reset the decoder. Missing, duplicate,
out-of-order or malformed FC groups terminate the stream as source data loss;
do not insert concealed frames to pretend continuity. Transport continuity and
codec decoding are separate checks: receiving 40 bytes is not proof of speech.

Reference vectors: [ico-vectors.json](../protocol/v3/ico-vectors.json).
Decoder build/provenance: [host ICO decoder](../client/c/ico/README.md).
Applications using PCM helpers still receive PCM; raw RBP audio callbacks remain
explicitly encoded, as they were for IMA. Clients without ICO support must not
advertise acceptance of `(2,1)`; keys remain independent of voice negotiation.

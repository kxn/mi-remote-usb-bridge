# RBP/3.0 contract artifacts

These artifacts describe the implemented RBP/3.0 firmware (0.6.5) and host SDK. Hardware validation scope is recorded in docs/validation-plan.md. See ../../docs/wire-protocol.md and ../../docs/audio-wire-v3.md.

- schema.json: complete management/key schema plus the revised audio contract.
- codec-vectors.json: exact IMA decoder initialization/reseed input-output examples.
- vectors.json: v3 wire frames for enabling IMA, initial config, a fragmented 600-byte unit, a same-rate decoder reset, next unit, and exact END counts.
- build_contract.py: deterministic generator importing the current Python runtime schema; no RBP/2 dependency.
- check_contract.py: independent framing/CRC, state-boundary and negative checks; supports the initial IMA profile only, not a production generic client.

Run with Python 3:

    python protocol/v3/build_contract.py
    python protocol/v3/check_contract.py

These independent checks complement the C/Python integration tests; they do not demonstrate hardware performance. New codecs require a registered profile and reference vectors, not an ad-hoc numeric identifier.

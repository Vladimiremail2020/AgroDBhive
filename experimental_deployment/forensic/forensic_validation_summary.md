# Corrected forensic validation summary

The V2.2 LittleFS filesystem was inspected using a dedicated read-only
forensic decoder.

## Result

- Offline state checksum: PASS
- Offline event journal: PASS
- Pending FIFO structure: VALID
- Pending FIFO SHA-256: PASS
- Complete offline journal records: 17
- Generated offline records recorded by the journal: 213
- Total duration represented by the 17 offline-event intervals: 14778 s (4.1050 h)

## Important checksum correction

The original forensic decoder produced false FAIL results because it hashed
the stored checksum field. The corrected decoder reproduces the firmware
algorithm: bytes 48..51 are zeroed before FNV-1a is calculated over bytes
0..51.

## Interpretation

The corrected PASS result establishes internal consistency of the stored V2.2
offline-event journal. Gateway synchronization is evaluated separately from
gateway/ESP32 replay evidence.

The deployment-session duration reported in the experimental record is 8-9 h.
The 17 offline events cover only the journaled communication-outage intervals;
their summed duration is not the total deployment duration.

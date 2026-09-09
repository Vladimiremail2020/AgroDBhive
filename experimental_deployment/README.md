# Experimental Deployment — AgroDBhive V2.2

This directory contains reproducibility artifacts for the physical
ESP32-to-Raspberry-Pi 4 validation of AgroDBhive.

## Scope

The experiment evaluates the complete edge data path under intermittent
connectivity:

1. physical sensor acquisition;
2. 64-bit telemetry payload generation;
3. SHA-256 hash chaining;
4. persistent LittleFS buffering;
5. FIFO replay after connectivity restoration;
6. Raspberry Pi 4 gateway persistence;
7. post-deployment forensic inspection.

The experiment is a physical proof-of-concept validation, not a statistically
powered field trial.

## Experimental configuration

- ESP32 edge node
- Raspberry Pi 4 gateway
- ADS1115 ADC
- BME280
- BH1750
- DS18B20
- LittleFS
- HTTP communication
- UTC/NTP time reference
- 35 s deep-sleep interval
- 64-bit payload
- SHA-256 over an 84-byte cryptographic input

## Repository-safe firmware

`firmware/AgroDBhive_V2.2_Intermittent_Cache_Replay_UTC_Journal_REDACTED.ino`
is a GitHub-safe copy of the deployed V2.2 firmware. Wi-Fi credentials have
been redacted. The algorithmic and storage logic is preserved.

Do not publish private Wi-Fi credentials, tokens, or passwords.

## Forensic validation

`firmware/AgroDBhive_V2.2_Forensic_ReadOnly_Corrected.ino` is the corrected
read-only forensic decoder.

Safety properties:

- no Wi-Fi;
- no HTTP;
- no telemetry;
- no file writes;
- no file deletion;
- no LittleFS formatting;
- `LittleFS.begin(false)`.

The corrected decoder validates the offline-event checksum by zeroing bytes
48..51 before calculating FNV-1a over bytes 0..51.

## Main evidence

`logs/forensic_readonly_corrected.log`
contains the complete corrected forensic Serial Monitor output.

`logs/gateway_replay_evidence.log`
contains a concise excerpt of gateway/ESP32 replay evidence showing failed
replay attempts followed by successful HTTP 200 commits and queue drainage.

## Offline-event data

`data/offline_events.csv` contains the 17 complete journal records.

The corrected journal reports:

- 17/17 valid records;
- 213 generated offline records;
- 14,778 s represented by the 17 outage intervals.

The 14,778 s value is NOT the total deployment duration. The deployment
session itself is reported as 8-9 h.

## Key result

The physical validation provides evidence that the deployed edge node can
continue generating and persistently buffering telemetry during communication
loss and subsequently replay the buffered FIFO records after connectivity
restoration. The corrected forensic inspection confirms internal consistency
of the stored offline-event journal and the inspected pending record.

## Reproducibility limitation

The public folder contains a credential-redacted firmware copy and processed
evidence. The original private deployment firmware should be retained by the
authors as the immutable experimental source.

This experiment should be interpreted as physical proof-of-concept validation,
not as a general performance characterization of all ESP32 or Raspberry Pi
deployments.

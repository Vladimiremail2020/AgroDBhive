/*
====================================================================
 AGRODBHIVE V2.2
 ESP32 EXPERIMENTAL PHYSICAL DEPLOYMENT
====================================================================

 SENSORS
 -------
 ADS1115  @ 0x48
 BME280   @ 0x76
 BH1750   @ 0x23
 DS18B20  @ GPIO27

 I2C
 ---
 SDA = GPIO21
 SCL = GPIO22
 Clock = 100 kHz

 DS18B20
 --------
 DATA = GPIO27
 4.7 kOhm pull-up -> 3.3 V


====================================================================
 64-BIT EXPERIMENTAL PAYLOAD
====================================================================

 63..52 : soil moisture       12 bits
 51..40 : soil temperature    12 bits
 39..30 : air temperature     10 bits
 29..20 : humidity            10 bits
 19..6  : pressure            14 bits
  5..0  : light                6 bits

 TOTAL = 64 bits


====================================================================
 CRYPTOGRAPHIC INPUT
====================================================================

 8 bytes  timestamp
 4 bytes  node_id
64 bytes  previous_hash
 8 bytes  payload
-------------------
84 bytes total

 SHA-256 is calculated over EXACTLY 84 bytes.

 Little-endian:
 timestamp = <Q
 node_id   = <i
 payload   = <Q


====================================================================
 BLOCKCHAIN STATE
====================================================================

 /agro_state.bin

 State is committed ONLY after HTTP 200.

 Gateway /status is used to synchronize the current blockchain tail.

====================================================================
*/

#include <Arduino.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_ADS1X15.h>

#include <BH1750.h>

#include <LittleFS.h>

#include <mbedtls/md.h>

#include "esp_sleep.h"
#include <time.h>


// ====================================================================
// TELEMETRY BLOCK
// ====================================================================

struct TelemetryBlock
{
    uint32_t sequence;

    uint64_t timestamp;

    uint64_t payload;

    char hash_signature[65];
};

// ====================================================================
// PENDING RECORD
// ====================================================================

struct PendingRecord
{
    uint32_t magic;
    TelemetryBlock block;
    char previous_hash[65];
};

// ====================================================================
// HARDWARE
// ====================================================================

#define I2C_SDA       21
#define I2C_SCL       22
#define PIN_DS18B20   27

#define ADS1115_ADDR  0x48
#define BME280_ADDR   0x76
#define BH1750_ADDR   0x23


// ====================================================================
// NODE
// ====================================================================

const int32_t NODE_ID = 1;


// ====================================================================
// WIFI / GATEWAY
// ====================================================================

const char* SSID_WIFI = "REDACTED_WIFI_SSID";
const char* PASS_WIFI = "REDACTED_WIFI_PASSWORD";

const char* GATEWAY_URL =
    "http://10.224.232.147:8000/ingest";

const char* STATUS_URL =
    "http://10.224.232.147:8000/status";


// ====================================================================
// POWER
// ====================================================================

const uint32_t SLEEP_SECONDS = 35;


// ====================================================================
// GENESIS
// ====================================================================

const char* GENESIS_HASH =
    "0000000000000000000000000000000000000000000000000000000000000000";


// ====================================================================
// SENSOR OBJECTS
// ====================================================================

Adafruit_ADS1115 ads;
Adafruit_BME280 bme;
BH1750 lightMeter;

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);


// ====================================================================
// SENSOR FLAGS
// ====================================================================

bool adsOK = false;
bool bmeOK = false;
bool bhOK  = false;
bool dsOK  = false;


// ====================================================================
// SENSOR DATA
// ====================================================================

struct SensorData
{
    int16_t soil_raw;

    float soil_voltage;
    float soil_moisture_pct;

    float soil_temperature;

    float air_temperature;
    float air_humidity;
    float pressure_hpa;

    float light_lux;

    bool soil_valid;
    bool soil_temperature_valid;
    bool bme_valid;
    bool bh_valid;
};


// ====================================================================
// PERSISTENT BLOCKCHAIN STATE
// ====================================================================

#define STATE_MAGIC   0x41475232UL
#define STATE_VERSION 3

struct PersistentState
{
    uint32_t magic;
    uint32_t version;

    uint32_t sequence;

    uint64_t next_timestamp;

    char last_hash[65];

    // Number of locally cached blocks waiting for gateway replay.
    uint32_t pending_count;

    // Byte offset of the first pending record in /agro_pending.bin.
    uint32_t pending_head;

    uint32_t checksum;
};

PersistentState state;

const char* PENDING_FILE = "/agro_pending.bin";
const uint32_t PENDING_MAGIC = 0x50454E44UL; // "PEND"

// ====================================================================
// V2.2 UTC TIME + PERSISTENT OFFLINE-EVENT JOURNAL
// ====================================================================
// These files are separate from the existing blockchain state and FIFO
// cache. The 64-bit payload, 84-byte SHA-256 input and cache format are
// intentionally unchanged.

const char* OFFLINE_STATE_FILE = "/agro_offline_state.bin";
const char* OFFLINE_JOURNAL_FILE = "/agro_offline_events.bin";

const uint32_t OFFLINE_STATE_MAGIC = 0x4F465332UL;   // "OFS2"
const uint32_t OFFLINE_EVENT_MAGIC = 0x4F464556UL;   // "OFEV"
const uint32_t OFFLINE_JOURNAL_VERSION = 1;

struct OfflineJournalState
{
    uint32_t magic;
    uint32_t version;
    uint32_t active;
    uint32_t event_id;
    uint64_t start_timestamp;
    uint32_t generated_count;
    uint32_t checksum;
};

struct OfflineEventRecord
{
    uint32_t magic;
    uint32_t version;
    uint32_t event_id;
    uint64_t start_timestamp;
    uint64_t end_timestamp;
    uint32_t generated_count;
    uint64_t duration_seconds;
    uint32_t checksum;
};

OfflineJournalState offlineJournalState{};



String uint64ToString(uint64_t value);
bool saveState();

// ====================================================================
// OFFLINE JOURNAL CHECKSUMS
// ====================================================================

uint32_t calculateOfflineStateChecksum(
    const OfflineJournalState& input
)
{
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(&input);

    const size_t length =
        sizeof(OfflineJournalState) - sizeof(uint32_t);

    uint32_t checksum = 2166136261UL;

    for (size_t i = 0; i < length; i++)
    {
        checksum ^= bytes[i];
        checksum *= 16777619UL;
    }

    return checksum;
}

uint32_t calculateOfflineEventChecksum(
    const OfflineEventRecord& input
)
{
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(&input);

    const size_t length =
        sizeof(OfflineEventRecord) - sizeof(uint32_t);

    uint32_t checksum = 2166136261UL;

    for (size_t i = 0; i < length; i++)
    {
        checksum ^= bytes[i];
        checksum *= 16777619UL;
    }

    return checksum;
}

// ====================================================================
// UTC CLOCK VALIDATION
// ====================================================================

bool utcTimeValid(uint64_t epoch)
{
    // 2023-11-14 is deliberately conservative; all real deployments
    // in this project are later than this date.
    return epoch >= 1700000000ULL;
}

uint64_t currentUtcEpoch()
{
    time_t now = time(nullptr);

    if (now <= 0)
        return 0;

    uint64_t epoch = (uint64_t)now;

    return utcTimeValid(epoch) ? epoch : 0;
}

// ====================================================================
// NTP / UTC SYNCHRONIZATION
// ====================================================================

bool synchronizeUtcClock()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[TIME] NTP skipped: Wi-Fi not connected.");
        return false;
    }

    Serial.println("[TIME] Synchronizing UTC clock with NTP...");

    // UTC: no local timezone offset and no DST offset.
    configTime(
        0,
        0,
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com"
    );

    struct tm timeinfo{};

    bool valid = getLocalTime(&timeinfo, 8000);

    uint64_t now = currentUtcEpoch();

    if (!valid || !utcTimeValid(now))
    {
        Serial.println("[TIME] NTP synchronization FAILED or invalid.");
        return false;
    }

    Serial.print("[TIME] UTC epoch: ");
    Serial.println(uint64ToString(now));

    char iso[32];
    strftime(
        iso,
        sizeof(iso),
        "%Y-%m-%dT%H:%M:%SZ",
        &timeinfo
    );

    Serial.print("[TIME] UTC ISO-8601: ");
    Serial.println(iso);

    // Never move the persistent blockchain clock backwards. When the
    // system has been offline, the cached timestamp continuation remains
    // authoritative for the already-created FIFO blocks. After replay,
    // the next online block is allowed to catch up to real UTC.
    if (state.next_timestamp == 0 || state.next_timestamp < now)
    {
        state.next_timestamp = now;

        if (!saveState())
        {
            Serial.println("[TIME] WARNING: UTC time could not be persisted.");
        }
    }

    Serial.println("[TIME] UTC reference is VALID.");

    return true;
}

// ====================================================================
// OFFLINE JOURNAL STATE
// ====================================================================

bool saveOfflineJournalState()
{
    offlineJournalState.checksum =
        calculateOfflineStateChecksum(offlineJournalState);

    File file = LittleFS.open(
        OFFLINE_STATE_FILE,
        FILE_WRITE
    );

    if (!file)
    {
        Serial.println("[OFFLINE] ERROR: cannot open journal state file.");
        return false;
    }

    size_t written = file.write(
        reinterpret_cast<const uint8_t*>(&offlineJournalState),
        sizeof(offlineJournalState)
    );

    file.close();

    if (written != sizeof(offlineJournalState))
    {
        Serial.println("[OFFLINE] ERROR: incomplete journal state write.");
        return false;
    }

    return true;
}

bool loadOfflineJournalState()
{
    if (!LittleFS.exists(OFFLINE_STATE_FILE))
    {
        memset(&offlineJournalState, 0, sizeof(offlineJournalState));
        offlineJournalState.magic = OFFLINE_STATE_MAGIC;
        offlineJournalState.version = OFFLINE_JOURNAL_VERSION;
        return saveOfflineJournalState();
    }

    File file = LittleFS.open(
        OFFLINE_STATE_FILE,
        FILE_READ
    );

    if (!file)
        return false;

    size_t readBytes = file.read(
        reinterpret_cast<uint8_t*>(&offlineJournalState),
        sizeof(offlineJournalState)
    );

    file.close();

    if (readBytes != sizeof(offlineJournalState) ||
        offlineJournalState.magic != OFFLINE_STATE_MAGIC ||
        offlineJournalState.version != OFFLINE_JOURNAL_VERSION ||
        offlineJournalState.checksum !=
            calculateOfflineStateChecksum(offlineJournalState))
    {
        Serial.println("[OFFLINE] Journal state invalid; resetting journal state.");

        memset(&offlineJournalState, 0, sizeof(offlineJournalState));
        offlineJournalState.magic = OFFLINE_STATE_MAGIC;
        offlineJournalState.version = OFFLINE_JOURNAL_VERSION;

        return saveOfflineJournalState();
    }

    Serial.print("[OFFLINE] Journal event active: ");
    Serial.println(offlineJournalState.active ? "YES" : "NO");

    if (offlineJournalState.active)
    {
        Serial.print("[OFFLINE] Active event ID: ");
        Serial.println(offlineJournalState.event_id);

        Serial.print("[OFFLINE] Active start UTC epoch: ");
        Serial.println(uint64ToString(offlineJournalState.start_timestamp));

        Serial.print("[OFFLINE] Generated offline records: ");
        Serial.println(offlineJournalState.generated_count);
    }

    return true;
}

bool appendOfflineEventRecord(
    const OfflineEventRecord& input
)
{
    File file = LittleFS.open(
        OFFLINE_JOURNAL_FILE,
        FILE_APPEND
    );

    if (!file)
    {
        Serial.println("[OFFLINE] ERROR: cannot open offline event journal.");
        return false;
    }

    size_t written = file.write(
        reinterpret_cast<const uint8_t*>(&input),
        sizeof(input)
    );

    file.close();

    if (written != sizeof(input))
    {
        Serial.println("[OFFLINE] ERROR: incomplete offline event journal write.");
        return false;
    }

    return true;
}

void printOfflineJournalStart(uint64_t timestamp)
{
    Serial.println();
    Serial.println("[OFFLINE] ==================================================");
    Serial.println("[OFFLINE] CONNECTIVITY LOSS EVENT START");
    Serial.println("[OFFLINE] ==================================================");
    Serial.print("[OFFLINE] Event ID: ");
    Serial.println(offlineJournalState.event_id);
    Serial.print("[OFFLINE] Start UTC epoch: ");
    Serial.println(uint64ToString(timestamp));
}

bool offlineJournalRecordGenerated(uint64_t blockTimestamp)
{
    if (!offlineJournalState.active)
    {
        offlineJournalState.active = 1;
        offlineJournalState.event_id++;
        offlineJournalState.start_timestamp = blockTimestamp;
        offlineJournalState.generated_count = 0;

        if (!saveOfflineJournalState())
            return false;

        printOfflineJournalStart(blockTimestamp);
    }

    offlineJournalState.generated_count++;

    if (!saveOfflineJournalState())
        return false;

    Serial.print("[OFFLINE] Event ");
    Serial.print(offlineJournalState.event_id);
    Serial.print(" generated records: ");
    Serial.println(offlineJournalState.generated_count);

    return true;
}

bool offlineJournalCloseIfActive()
{
    if (!offlineJournalState.active)
        return true;

    uint64_t endTimestamp = currentUtcEpoch();

    if (!utcTimeValid(endTimestamp))
    {
        Serial.println("[OFFLINE] Cannot close event: UTC clock is not valid.");
        return false;
    }

    if (endTimestamp < offlineJournalState.start_timestamp)
    {
        Serial.println("[OFFLINE] Cannot close event: UTC time moved backwards.");
        return false;
    }

    OfflineEventRecord event{};
    event.magic = OFFLINE_EVENT_MAGIC;
    event.version = OFFLINE_JOURNAL_VERSION;
    event.event_id = offlineJournalState.event_id;
    event.start_timestamp = offlineJournalState.start_timestamp;
    event.end_timestamp = endTimestamp;
    event.generated_count = offlineJournalState.generated_count;
    event.duration_seconds =
        endTimestamp - offlineJournalState.start_timestamp;
    event.checksum = calculateOfflineEventChecksum(event);

    if (!appendOfflineEventRecord(event))
        return false;

    Serial.println();
    Serial.println("[OFFLINE] ==================================================");
    Serial.println("[OFFLINE] CONNECTIVITY RESTORED / EVENT CLOSED");
    Serial.println("[OFFLINE] ==================================================");
    Serial.print("[OFFLINE] Event ID: ");
    Serial.println(event.event_id);
    Serial.print("[OFFLINE] Start UTC epoch: ");
    Serial.println(uint64ToString(event.start_timestamp));
    Serial.print("[OFFLINE] End UTC epoch: ");
    Serial.println(uint64ToString(event.end_timestamp));
    Serial.print("[OFFLINE] Duration seconds: ");
    Serial.println(uint64ToString(event.duration_seconds));
    Serial.print("[OFFLINE] Records generated offline: ");
    Serial.println(event.generated_count);
    Serial.println("[OFFLINE] Persistent event record committed.");

    offlineJournalState.active = 0;
    offlineJournalState.start_timestamp = 0;
    offlineJournalState.generated_count = 0;

    return saveOfflineJournalState();
}

// ====================================================================
// UINT64 -> STRING
// ====================================================================

String uint64ToString(uint64_t value)
{
    char buffer[32];

    snprintf(
        buffer,
        sizeof(buffer),
        "%llu",
        (unsigned long long)value
    );

    return String(buffer);
}


// ====================================================================
// UINT64 -> HEX
// ====================================================================

String uint64ToHex(uint64_t value)
{
    char buffer[32];

    snprintf(
        buffer,
        sizeof(buffer),
        "%016llX",
        (unsigned long long)value
    );

    return String(buffer);
}


// ====================================================================
// CLAMP
// ====================================================================

float clampValue(
    float value,
    float minimum,
    float maximum
)
{
    if (value < minimum)
        return minimum;

    if (value > maximum)
        return maximum;

    return value;
}


// ====================================================================
// STATE CHECKSUM
// ====================================================================

uint32_t calculateStateChecksum(
    const PersistentState& input
)
{
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(&input);

    const size_t length =
        sizeof(PersistentState) -
        sizeof(uint32_t);

    uint32_t checksum =
        2166136261UL;

    for (size_t i = 0; i < length; i++)
    {
        checksum ^= bytes[i];
        checksum *= 16777619UL;
    }

    return checksum;
}


// ====================================================================
// DEFAULT STATE
// ====================================================================

void initializeDefaultState()
{
    memset(
        &state,
        0,
        sizeof(state)
    );

    state.magic =
        STATE_MAGIC;

    state.version =
        STATE_VERSION;

    state.sequence =
        0;

    state.next_timestamp =
        0ULL;

    strncpy(
        state.last_hash,
        GENESIS_HASH,
        64
    );

    state.last_hash[64] =
        '\0';

    state.pending_count = 0;
    state.pending_head = 0;

    state.checksum =
        calculateStateChecksum(state);
}


// ====================================================================
// SAVE STATE
// ====================================================================

bool saveState()
{
    state.checksum =
        calculateStateChecksum(state);

    File file =
        LittleFS.open(
            "/agro_state.bin",
            FILE_WRITE
        );

    if (!file)
    {
        Serial.println(
            "[STATE] ERROR: cannot open state file."
        );

        return false;
    }

    size_t written =
        file.write(
            reinterpret_cast<const uint8_t*>(&state),
            sizeof(state)
        );

    file.close();

    if (written != sizeof(state))
    {
        Serial.println(
            "[STATE] ERROR: incomplete state write."
        );

        return false;
    }

    Serial.println(
        "[STATE] Persistent state saved."
    );

    return true;
}


// ====================================================================
// LOAD STATE
// ====================================================================

bool loadState()
{
    if (
        !LittleFS.exists(
            "/agro_state.bin"
        )
    )
    {
        Serial.println(
            "[STATE] No existing state."
        );

        initializeDefaultState();

        return saveState();
    }

    File file =
        LittleFS.open(
            "/agro_state.bin",
            FILE_READ
        );

    if (!file)
    {
        initializeDefaultState();

        return saveState();
    }

    size_t readBytes =
        file.read(
            reinterpret_cast<uint8_t*>(&state),
            sizeof(state)
        );

    file.close();

    if (
        readBytes !=
        sizeof(state)
    )
    {
        Serial.println(
            "[STATE] Invalid state size."
        );

        initializeDefaultState();

        return saveState();
    }

    if (
        state.magic !=
        STATE_MAGIC
    )
    {
        Serial.println(
            "[STATE] Invalid state magic."
        );

        initializeDefaultState();

        return saveState();
    }

    if (
        state.version !=
        STATE_VERSION
    )
    {
        Serial.println(
            "[STATE] State version mismatch."
        );

        // Firmware state format changed for intermittent caching.
        // Start a clean experiment state and remove any incompatible queue.
        LittleFS.remove(PENDING_FILE);

        initializeDefaultState();

        return saveState();
    }

    uint32_t checksum =
        calculateStateChecksum(state);

    if (
        checksum !=
        state.checksum
    )
    {
        Serial.println(
            "[STATE] State checksum failure."
        );

        initializeDefaultState();

        return saveState();
    }

    Serial.println(
        "[STATE] Persistent state loaded."
    );

    Serial.print(
        "[STATE] Sequence: "
    );

    Serial.println(
        state.sequence
    );

    Serial.print(
        "[STATE] Next timestamp: "
    );

    Serial.println(
        uint64ToString(
            state.next_timestamp
        )
    );

    Serial.print(
        "[STATE] Last hash: "
    );

    Serial.println(
        state.last_hash
    );

    Serial.print(
        "[STATE] Pending blocks: "
    );

    Serial.println(
        state.pending_count
    );

    return true;
}


// ====================================================================
// INTERMITTENT TELEMETRY CACHE
// ====================================================================
//
// Offline blocks are stored in FIFO order in LittleFS.
// The cached block contains the exact cryptographic block plus the
// previous hash used to create it. This makes offline generation and
// later replay auditable and deterministic.
//
// IMPORTANT:
//   - A cached block advances LOCAL blockchain state immediately.
//   - Gateway state is NOT advanced until replay receives HTTP 200.
//   - During replay, state is not modified because it already contains
//     the post-cache local tail.
// ====================================================================

int sendBlock(const TelemetryBlock& block);



bool pendingFileValid()
{
    if (!LittleFS.exists(PENDING_FILE))
        return false;

    File file = LittleFS.open(PENDING_FILE, FILE_READ);

    if (!file)
        return false;

    size_t size = file.size();
    file.close();

    return size % sizeof(PendingRecord) == 0;
}

bool appendPendingBlock(
    const TelemetryBlock& block,
    const char* previousHash
)
{
    File file =
        LittleFS.open(
            PENDING_FILE,
            FILE_APPEND
        );

    if (!file)
    {
        Serial.println(
            "[CACHE] ERROR: cannot open pending cache."
        );

        return false;
    }

    PendingRecord record{};
    record.magic = PENDING_MAGIC;
    record.block = block;

    strncpy(
        record.previous_hash,
        previousHash,
        64
    );

    record.previous_hash[64] = '\0';

    size_t written =
        file.write(
            reinterpret_cast<const uint8_t*>(&record),
            sizeof(record)
        );

    file.close();

    if (written != sizeof(record))
    {
        Serial.println(
            "[CACHE] ERROR: incomplete cache write."
        );

        return false;
    }

    state.pending_count++;

    Serial.print(
        "[CACHE] Block stored locally. Pending blocks: "
    );

    Serial.println(
        state.pending_count
    );

    return true;
}

bool readPendingBlock(
    uint32_t index,
    PendingRecord& record
)
{
    if (index >= state.pending_count)
        return false;

    File file =
        LittleFS.open(
            PENDING_FILE,
            FILE_READ
        );

    if (!file)
        return false;

    size_t offset =
        (size_t)state.pending_head +
        ((size_t)index * sizeof(PendingRecord));

    if (!file.seek(offset))
    {
        file.close();
        return false;
    }

    size_t readBytes =
        file.read(
            reinterpret_cast<uint8_t*>(&record),
            sizeof(record)
        );

    file.close();

    if (readBytes != sizeof(record))
        return false;

    if (record.magic != PENDING_MAGIC)
        return false;

    return true;
}

bool removeFirstPendingBlock()
{
    if (state.pending_count == 0)
        return true;

    state.pending_count--;

    state.pending_head += sizeof(PendingRecord);

    if (state.pending_count == 0)
    {
        LittleFS.remove(PENDING_FILE);
        state.pending_head = 0;
    }

    return saveState();
}

void printPendingSummary()
{
    Serial.println();
    Serial.println(
        "================================================"
    );
    Serial.println(
        " OFFLINE TELEMETRY CACHE"
    );
    Serial.println(
        "================================================"
    );

    Serial.print(
        "[CACHE] Pending blocks: "
    );

    Serial.println(
        state.pending_count
    );

    Serial.print(
        "[CACHE] Queue head offset: "
    );

    Serial.println(
        state.pending_head
    );
}

bool cacheBlockAndAdvanceState(
    const TelemetryBlock& block,
    const char* previousHash
)
{
    if (
        !appendPendingBlock(
            block,
            previousHash
        )
    )
    {
        return false;
    }

    // Record the offline event only after the exact telemetry block
    // has been durably appended to the existing FIFO cache.
    if (!offlineJournalRecordGenerated(block.timestamp))
    {
        Serial.println("[OFFLINE] WARNING: event journal update failed.");
    }

    // The block now exists persistently in the FIFO.
    // Advance local blockchain state so the next offline block
    // continues from this cached block's hash.
    state.sequence =
        block.sequence + 1;

    state.next_timestamp =
        block.timestamp +
        SLEEP_SECONDS;

    strncpy(
        state.last_hash,
        block.hash_signature,
        64
    );

    state.last_hash[64] = '\0';

    if (!saveState())
    {
        Serial.println(
            "[CACHE] WARNING: state save failed after caching."
        );

        return false;
    }

    Serial.println(
        "[CACHE] Local blockchain state advanced."
    );

    return true;
}

bool replayPendingBlocks()
{
    if (state.pending_count == 0)
    {
        Serial.println(
            "[REPLAY] No pending blocks."
        );

        return true;
    }

    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        Serial.println(
            "[REPLAY] Wi-Fi unavailable."
        );

        return false;
    }

    printPendingSummary();

    while (state.pending_count > 0)
    {
        PendingRecord record{};

        if (
            !readPendingBlock(
                0,
                record
            )
        )
        {
            Serial.println(
                "[REPLAY] ERROR: cannot read pending block."
            );

            return false;
        }

        Serial.println();
        Serial.println(
            "[REPLAY] Replaying cached block..."
        );

        Serial.print(
            "[REPLAY] Sequence: "
        );

        Serial.println(
            record.block.sequence
        );

        Serial.print(
            "[REPLAY] Timestamp: "
        );

        Serial.println(
            uint64ToString(
                record.block.timestamp
            )
        );

        Serial.print(
            "[REPLAY] Previous hash: "
        );

        Serial.println(
            record.previous_hash
        );

        Serial.print(
            "[REPLAY] Hash: "
        );

        Serial.println(
            record.block.hash_signature
        );

        int replayCode =
            sendBlock(
                record.block
            );

        if (replayCode == 200)
        {
            Serial.println(
                "[REPLAY] HTTP 200."
            );

            Serial.println(
                "[REPLAY] Cached block committed by gateway."
            );

            if (
                !removeFirstPendingBlock()
            )
            {
                Serial.println(
                    "[REPLAY] ERROR: cache state update failed."
                );

                return false;
            }

            Serial.print(
                "[REPLAY] Remaining pending blocks: "
            );

            Serial.println(
                state.pending_count
            );

            continue;
        }

        Serial.print(
            "[REPLAY] Gateway did not accept cached block. HTTP: "
        );

        Serial.println(
            replayCode
        );

        Serial.println(
            "[REPLAY] Pending block retained for next cycle."
        );

        return false;
    }

    Serial.println(
        "[REPLAY] SUCCESS: offline queue is empty."
    );

    return true;
}

// ====================================================================
// LITTLEFS
// ====================================================================

bool initializeLittleFS()
{
    if (
        !LittleFS.begin(true)
    )
    {
        Serial.println(
            "[FATAL] LittleFS initialization failed."
        );

        return false;
    }

    Serial.println(
        "[OK] LittleFS mounted."
    );

    if (!loadOfflineJournalState())
    {
        Serial.println("[OFFLINE] WARNING: journal state could not be loaded.");
    }

    if (
        state.pending_count > 0 &&
        !pendingFileValid()
    )
    {
        Serial.println(
            "[CACHE] WARNING: pending cache is invalid. Clearing queue metadata."
        );

        state.pending_count = 0;
        state.pending_head = 0;
        saveState();
    }

    return true;
}


// ====================================================================
// QUANTIZATION
// ====================================================================

uint16_t quantizeSoilMoisture(
    float value
)
{
    value =
        clampValue(
            value,
            0.0f,
            100.0f
        );

    return (uint16_t)roundf(
        (
            value /
            100.0f
        ) *
        4095.0f
    );
}


uint16_t quantizeSoilTemperature(
    float value
)
{
    value =
        clampValue(
            value,
            -40.0f,
            85.0f
        );

    return (uint16_t)roundf(
        (
            (
                value +
                40.0f
            )
            /
            125.0f
        )
        *
        4095.0f
    );
}


uint16_t quantizeAirTemperature(
    float value
)
{
    value =
        clampValue(
            value,
            -40.0f,
            85.0f
        );

    return (uint16_t)roundf(
        (
            (
                value +
                40.0f
            )
            /
            125.0f
        )
        *
        1023.0f
    );
}


uint16_t quantizeHumidity(
    float value
)
{
    value =
        clampValue(
            value,
            0.0f,
            100.0f
        );

    return (uint16_t)roundf(
        (
            value /
            100.0f
        )
        *
        1023.0f
    );
}


uint16_t quantizePressure(
    float value
)
{
    value =
        clampValue(
            value,
            300.0f,
            1100.0f
        );

    return (uint16_t)roundf(
        (
            (
                value -
                300.0f
            )
            /
            800.0f
        )
        *
        16383.0f
    );
}


uint8_t quantizeLight(
    float lux
)
{
    lux =
        clampValue(
            lux,
            0.0f,
            200000.0f
        );

    if (lux <= 0.0f)
        return 0;

    float normalized =
        log10f(1.0f + lux) /
        log10f(200001.0f);

    normalized =
        clampValue(
            normalized,
            0.0f,
            1.0f
        );

    return (uint8_t)roundf(
        normalized *
        63.0f
    );
}


// ====================================================================
// PACK 64-BIT PAYLOAD
// ====================================================================

uint64_t packExperimentalPayload(
    const SensorData& data
)
{
    uint16_t soil =
        quantizeSoilMoisture(
            data.soil_moisture_pct
        );

    uint16_t soilTemp =
        quantizeSoilTemperature(
            data.soil_temperature
        );

    uint16_t airTemp =
        quantizeAirTemperature(
            data.air_temperature
        );

    uint16_t humidity =
        quantizeHumidity(
            data.air_humidity
        );

    uint16_t pressure =
        quantizePressure(
            data.pressure_hpa
        );

    uint8_t light =
        quantizeLight(
            data.light_lux
        );

    uint64_t payload = 0;

    payload |=
        ((uint64_t)(soil & 0x0FFF))
        << 52;

    payload |=
        ((uint64_t)(soilTemp & 0x0FFF))
        << 40;

    payload |=
        ((uint64_t)(airTemp & 0x03FF))
        << 30;

    payload |=
        ((uint64_t)(humidity & 0x03FF))
        << 20;

    payload |=
        ((uint64_t)(pressure & 0x3FFF))
        << 6;

    payload |=
        ((uint64_t)(light & 0x3F));

    return payload;
}


// ====================================================================
// PRINT PAYLOAD
// ====================================================================

void printPayload(
    const SensorData& data,
    uint64_t payload
)
{
    Serial.println();

    Serial.println(
        "================================================"
    );

    Serial.println(
        " EXPERIMENTAL 64-BIT PAYLOAD"
    );

    Serial.println(
        "================================================"
    );

    Serial.printf(
        "Soil moisture      : %.2f %% -> %u\n",
        data.soil_moisture_pct,
        quantizeSoilMoisture(
            data.soil_moisture_pct
        )
    );

    Serial.printf(
        "Soil temperature   : %.2f C -> %u\n",
        data.soil_temperature,
        quantizeSoilTemperature(
            data.soil_temperature
        )
    );

    Serial.printf(
        "Air temperature    : %.2f C -> %u\n",
        data.air_temperature,
        quantizeAirTemperature(
            data.air_temperature
        )
    );

    Serial.printf(
        "Humidity           : %.2f %% -> %u\n",
        data.air_humidity,
        quantizeHumidity(
            data.air_humidity
        )
    );

    Serial.printf(
        "Pressure           : %.2f hPa -> %u\n",
        data.pressure_hpa,
        quantizePressure(
            data.pressure_hpa
        )
    );

    Serial.printf(
        "Light              : %.2f lux -> %u\n",
        data.light_lux,
        quantizeLight(
            data.light_lux
        )
    );

    Serial.println();

    Serial.println(
        "Bit allocation:"
    );

    Serial.println(
        "63..52 : soil moisture       [12 bits]"
    );

    Serial.println(
        "51..40 : soil temperature    [12 bits]"
    );

    Serial.println(
        "39..30 : air temperature     [10 bits]"
    );

    Serial.println(
        "29..20 : humidity             [10 bits]"
    );

    Serial.println(
        "19..6  : pressure             [14 bits]"
    );

    Serial.println(
        "5..0   : light                 [6 bits]"
    );

    Serial.println(
        "TOTAL  :                       [64 bits]"
    );

    Serial.println();

    Serial.print(
        "PAYLOAD DECIMAL : "
    );

    Serial.println(
        uint64ToString(payload)
    );

    Serial.print(
        "PAYLOAD HEX     : 0x"
    );

    Serial.println(
        uint64ToHex(payload)
    );
}


// ====================================================================
// SENSOR INITIALIZATION
// ====================================================================

void initializeSensors()
{
    Serial.println();

    Serial.println(
        "================================================"
    );

    Serial.println(
        " EXPERIMENTAL SENSOR INITIALIZATION"
    );

    Serial.println(
        "================================================"
    );

    Wire.begin(
        I2C_SDA,
        I2C_SCL
    );

    Wire.setClock(
        100000
    );

    delay(100);

    Serial.println(
        "[OK] I2C: SDA GPIO21 / SCL GPIO22"
    );

    Serial.println(
        "[OK] I2C clock: 100 kHz"
    );


    // ADS1115
    adsOK =
        ads.begin(
            ADS1115_ADDR,
            &Wire
        );

    if (adsOK)
    {
        ads.setGain(GAIN_ONE);

        Serial.println(
            "[OK] ADS1115 @ 0x48"
        );
    }
    else
    {
        Serial.println(
            "[ERROR] ADS1115 @ 0x48"
        );
    }


    // BME280
    bmeOK =
        bme.begin(
            BME280_ADDR,
            &Wire
        );

    if (bmeOK)
    {
        Serial.println(
            "[OK] BME280 @ 0x76"
        );
    }
    else
    {
        Serial.println(
            "[ERROR] BME280 @ 0x76"
        );
    }


    // BH1750
    bhOK =
        lightMeter.begin(
            BH1750::CONTINUOUS_HIGH_RES_MODE,
            BH1750_ADDR,
            &Wire
        );

    if (bhOK)
    {
        Serial.println(
            "[OK] BH1750 @ 0x23"
        );
    }
    else
    {
        Serial.println(
            "[ERROR] BH1750 @ 0x23"
        );
    }


    // DS18B20
    ds18b20.begin();

    int deviceCount =
        ds18b20.getDeviceCount();

    dsOK =
        deviceCount > 0;

    if (dsOK)
    {
        ds18b20.setResolution(12);

        Serial.print(
            "[OK] DS18B20 @ GPIO27 | devices: "
        );

        Serial.println(
            deviceCount
        );
    }
    else
    {
        Serial.println(
            "[ERROR] DS18B20 @ GPIO27"
        );
    }


    Serial.println();

    Serial.println(
        "------------------------------------------------"
    );

    Serial.println(
        " SENSOR INITIALIZATION SUMMARY"
    );

    Serial.println(
        "------------------------------------------------"
    );

    Serial.print("ADS1115 : ");
    Serial.println(adsOK ? "OK" : "ERROR");

    Serial.print("BME280  : ");
    Serial.println(bmeOK ? "OK" : "ERROR");

    Serial.print("BH1750  : ");
    Serial.println(bhOK ? "OK" : "ERROR");

    Serial.print("DS18B20 : ");
    Serial.println(dsOK ? "OK" : "ERROR");
}


// ====================================================================
// SOIL MOISTURE
// ====================================================================

bool readSoilMoisture(
    SensorData& data
)
{
    if (!adsOK)
        return false;

    int16_t raw =
        ads.readADC_SingleEnded(0);

    float voltage =
        ads.computeVolts(raw);

    data.soil_raw =
        raw;

    data.soil_voltage =
        voltage;


    /*
    Current experimental conversion.

    This is NOT presented as laboratory calibration.
    */

    const float SOIL_DRY_VOLTAGE =
        3.30f;

    const float SOIL_WET_VOLTAGE =
        1.10f;

    float percentage =
        (
            SOIL_DRY_VOLTAGE -
            voltage
        )
        /
        (
            SOIL_DRY_VOLTAGE -
            SOIL_WET_VOLTAGE
        )
        *
        100.0f;

    percentage =
        clampValue(
            percentage,
            0.0f,
            100.0f
        );

    data.soil_moisture_pct =
        percentage;

    data.soil_valid =
        (
            voltage >= 0.0f &&
            voltage <= 3.3f
        );

    return data.soil_valid;
}


// ====================================================================
// DS18B20
// ====================================================================

bool readSoilTemperature(
    SensorData& data
)
{
    if (!dsOK)
        return false;

    ds18b20.requestTemperatures();

    float temperature =
        ds18b20.getTempCByIndex(0);

    if (
        temperature ==
        DEVICE_DISCONNECTED_C
    )
    {
        data.soil_temperature_valid =
            false;

        return false;
    }

    if (
        temperature < -55.0f ||
        temperature > 125.0f
    )
    {
        data.soil_temperature_valid =
            false;

        return false;
    }

    data.soil_temperature =
        temperature;

    data.soil_temperature_valid =
        true;

    return true;
}


// ====================================================================
// BME280
// ====================================================================

bool readBME(
    SensorData& data
)
{
    if (!bmeOK)
        return false;

    data.air_temperature =
        bme.readTemperature();

    data.air_humidity =
        bme.readHumidity();

    data.pressure_hpa =
        bme.readPressure()
        /
        100.0f;

    if (
        isnan(data.air_temperature) ||
        isnan(data.air_humidity) ||
        isnan(data.pressure_hpa)
    )
    {
        data.bme_valid =
            false;

        return false;
    }

    data.bme_valid =
        (
            data.air_temperature >= -40.0f &&
            data.air_temperature <= 85.0f &&
            data.air_humidity >= 0.0f &&
            data.air_humidity <= 100.0f &&
            data.pressure_hpa >= 300.0f &&
            data.pressure_hpa <= 1100.0f
        );

    return data.bme_valid;
}


// ====================================================================
// BH1750
// ====================================================================

bool readBH(
    SensorData& data
)
{
    if (!bhOK)
        return false;

    float lux =
        lightMeter.readLightLevel();

    if (
        isnan(lux) ||
        lux < 0.0f
    )
    {
        data.bh_valid =
            false;

        return false;
    }

    data.light_lux =
        lux;

    data.bh_valid =
        true;

    return true;
}


// ====================================================================
// ACQUIRE
// ====================================================================

bool acquireSensors(
    SensorData& data
)
{
    memset(
        &data,
        0,
        sizeof(data)
    );

    Serial.println();

    Serial.println(
        "================================================"
    );

    Serial.println(
        " EXPERIMENTAL SENSOR ACQUISITION"
    );

    Serial.println(
        "================================================"
    );


    // Soil
    if (
        readSoilMoisture(data)
    )
    {
        Serial.println(
            "[SOIL MOISTURE]"
        );

        Serial.printf(
            "ADC raw      : %d\n",
            data.soil_raw
        );

        Serial.printf(
            "Voltage      : %.4f V\n",
            data.soil_voltage
        );

        Serial.printf(
            "Moisture     : %.2f %%\n",
            data.soil_moisture_pct
        );

        Serial.println(
            "STATUS       : VALID"
        );
    }
    else
    {
        Serial.println(
            "[SOIL MOISTURE] ERROR"
        );
    }


    // DS18B20
    if (
        readSoilTemperature(data)
    )
    {
        Serial.println(
            "[DS18B20 SOIL TEMPERATURE]"
        );

        Serial.printf(
            "Temperature  : %.2f C\n",
            data.soil_temperature
        );

        Serial.println(
            "STATUS       : VALID"
        );
    }
    else
    {
        Serial.println(
            "[DS18B20] ERROR / DISCONNECTED"
        );
    }


    // BME280
    if (
        readBME(data)
    )
    {
        Serial.println(
            "[BME280]"
        );

        Serial.printf(
            "Air temp     : %.2f C\n",
            data.air_temperature
        );

        Serial.printf(
            "Humidity     : %.2f %%\n",
            data.air_humidity
        );

        Serial.printf(
            "Pressure     : %.2f hPa\n",
            data.pressure_hpa
        );

        Serial.println(
            "STATUS       : VALID"
        );
    }
    else
    {
        Serial.println(
            "[BME280] ERROR"
        );
    }


    // BH1750
    if (
        readBH(data)
    )
    {
        Serial.println(
            "[BH1750]"
        );

        Serial.printf(
            "Light        : %.2f lux\n",
            data.light_lux
        );

        Serial.println(
            "STATUS       : VALID"
        );
    }
    else
    {
        Serial.println(
            "[BH1750] ERROR"
        );
    }


    bool allValid =
        data.soil_valid &&
        data.soil_temperature_valid &&
        data.bme_valid &&
        data.bh_valid;


    Serial.println();

    Serial.print(
        "[SENSORS] "
    );

    Serial.println(
        allValid
        ? "ALL VALID."
        : "INVALID MEASUREMENT."
    );


    return allValid;
}


// ====================================================================
// WIFI
// ====================================================================

bool connectWiFi()
{
    Serial.println();

    Serial.println(
        "[NETWORK] Connecting..."
    );

    WiFi.mode(
        WIFI_STA
    );

    WiFi.begin(
        SSID_WIFI,
        PASS_WIFI
    );

    for (
        int attempt = 0;
        attempt < 20;
        attempt++
    )
    {
        if (
            WiFi.status() ==
            WL_CONNECTED
        )
        {
            Serial.println();

            Serial.println(
                "[NETWORK] Wi-Fi connected."
            );

            Serial.print(
                "[NETWORK] IP: "
            );

            Serial.println(
                WiFi.localIP()
            );

            Serial.print(
                "[NETWORK] RSSI: "
            );

            Serial.print(
                WiFi.RSSI()
            );

            Serial.println(
                " dBm"
            );

            return true;
        }

        Serial.print(".");
        delay(500);
    }

    Serial.println();

    Serial.println(
        "[NETWORK] Wi-Fi connection failed."
    );

    return false;
}


// ====================================================================
// EXTRACT NODE SECTION
// ====================================================================

bool extractNodeSection(
    const String& json,
    int32_t nodeId,
    String& section
)
{
    String nodeToken =
        "\"node_id\":" +
        String(nodeId);

    int position =
        json.indexOf(nodeToken);

    if (position < 0)
        return false;

    int end =
        json.indexOf(
            "}",
            position
        );

    if (end < 0)
        return false;

    section =
        json.substring(
            position,
            end + 1
        );

    return true;
}


// ====================================================================
// EXTRACT STRING
// ====================================================================

bool extractStringValue(
    const String& json,
    const String& key,
    String& value
)
{
    String token =
        "\"" +
        key +
        "\":\"";

    int position =
        json.indexOf(token);

    if (position < 0)
        return false;

    position +=
        token.length();

    int end =
        json.indexOf(
            "\"",
            position
        );

    if (end < 0)
        return false;

    value =
        json.substring(
            position,
            end
        );

    return true;
}


// ====================================================================
// EXTRACT UNSIGNED 64-BIT
// ====================================================================

bool extractUnsigned64(
    const String& json,
    const String& key,
    uint64_t& value
)
{
    String token =
        "\"" +
        key +
        "\":";

    int position =
        json.indexOf(token);

    if (position < 0)
        return false;

    position +=
        token.length();

    while (
        position < json.length() &&
        (
            json[position] == ' ' ||
            json[position] == '"'
        )
    )
    {
        position++;
    }

    String number;

    while (
        position < json.length() &&
        isDigit(json[position])
    )
    {
        number +=
            json[position];

        position++;
    }

    if (
        number.length() == 0
    )
    {
        return false;
    }

    value = 0;

    for (
        size_t i = 0;
        i < number.length();
        i++
    )
    {
        uint8_t digit =
            number[i] - '0';

        value =
            value * 10ULL +
            digit;
    }

    return true;
}


// ====================================================================
// SYNCHRONIZE WITH RPI4
// ====================================================================

bool synchronizeWithGateway()
{
    if (state.pending_count > 0)
    {
        Serial.println(
            "[SYNC] Pending offline blocks exist. Synchronization deferred until replay."
        );

        return false;
    }

    Serial.println();

    Serial.println(
        "================================================"
    );

    Serial.println(
        " BLOCKCHAIN STATE SYNCHRONIZATION"
    );

    Serial.println(
        "================================================"
    );


    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        Serial.println(
            "[SYNC] Wi-Fi unavailable."
        );

        return false;
    }


    HTTPClient http;

    http.setTimeout(5000);


    if (
        !http.begin(
            STATUS_URL
        )
    )
    {
        Serial.println(
            "[SYNC] HTTP initialization failed."
        );

        return false;
    }


    int code =
        http.GET();


    Serial.print(
        "[SYNC] HTTP code: "
    );

    Serial.println(
        code
    );


    if (
        code != 200
    )
    {
        http.end();

        return false;
    }


    String response =
        http.getString();

    http.end();


    Serial.println(
        "[SYNC] Gateway status received."
    );


    String section;


    if (
        !extractNodeSection(
            response,
            NODE_ID,
            section
        )
    )
    {
        Serial.println(
            "[SYNC] Node not yet present in gateway."
        );

        strncpy(
            state.last_hash,
            GENESIS_HASH,
            64
        );

        state.last_hash[64] =
            '\0';

        saveState();

        return true;
    }


    String gatewayHash;


    if (
        !extractStringValue(
            section,
            "current_tail_hash",
            gatewayHash
        )
    )
    {
        Serial.println(
            "[SYNC] Cannot extract current tail."
        );

        return false;
    }


    if (
        gatewayHash.length() != 64
    )
    {
        Serial.println(
            "[SYNC] Invalid gateway hash length."
        );

        return false;
    }


    uint64_t latestTimestamp = 0;


    if (
        !extractUnsigned64(
            section,
            "latest_record_ts",
            latestTimestamp
        )
    )
    {
        Serial.println(
            "[SYNC] Cannot extract latest timestamp."
        );

        return false;
    }


    Serial.print(
        "[SYNC] Gateway latest timestamp: "
    );

    Serial.println(
        uint64ToString(
            latestTimestamp
        )
    );


    Serial.print(
        "[SYNC] Gateway current tail: "
    );

    Serial.println(
        gatewayHash
    );


    strncpy(
        state.last_hash,
        gatewayHash.c_str(),
        64
    );

    state.last_hash[64] =
        '\0';


    uint64_t nextTimestamp =
        latestTimestamp +
        SLEEP_SECONDS;


    if (
        state.next_timestamp >
        nextTimestamp
    )
    {
        nextTimestamp =
            state.next_timestamp;
    }


    if (
        nextTimestamp <=
        latestTimestamp
    )
    {
        nextTimestamp =
            latestTimestamp +
            SLEEP_SECONDS;
    }


    state.next_timestamp =
        nextTimestamp;


    saveState();


    Serial.print(
        "[SYNC] Next timestamp: "
    );

    Serial.println(
        uint64ToString(
            state.next_timestamp
        )
    );


    Serial.print(
        "[SYNC] Previous hash for next block: "
    );

    Serial.println(
        state.last_hash
    );


    Serial.println(
        "[SYNC] SUCCESS."
    );


    return true;
}


// ====================================================================
// SHA-256 OVER EXACTLY 84 BYTES
// ====================================================================

bool generateHash(
    const TelemetryBlock& block,
    const char* previousHash,
    char* outputHash
)
{
    uint8_t raw[84];

    int offset = 0;


    // ================================================================
    // 8 bytes timestamp - little endian
    // ================================================================

    uint64_t timestamp =
        block.timestamp;


    for (
        int i = 0;
        i < 8;
        i++
    )
    {
        raw[offset++] =
            (
                timestamp >>
                (8 * i)
            ) &
            0xFF;
    }


    // ================================================================
    // 4 bytes node_id - little endian
    // ================================================================

    int32_t nodeId =
        NODE_ID;

    uint32_t nodeUnsigned =
        (uint32_t)nodeId;


    for (
        int i = 0;
        i < 4;
        i++
    )
    {
        raw[offset++] =
            (
                nodeUnsigned >>
                (8 * i)
            ) &
            0xFF;
    }


    // ================================================================
    // 64 bytes previous hash
    // ================================================================

    if (
        strlen(previousHash) != 64
    )
    {
        Serial.println(
            "[CRYPTO] Invalid previous hash."
        );

        return false;
    }


    memcpy(
        &raw[offset],
        previousHash,
        64
    );

    offset += 64;


    // ================================================================
    // 8 bytes payload - little endian
    // ================================================================

    uint64_t payload =
        block.payload;


    for (
        int i = 0;
        i < 8;
        i++
    )
    {
        raw[offset++] =
            (
                payload >>
                (8 * i)
            ) &
            0xFF;
    }


    // ================================================================
    // MUST BE 84
    // ================================================================

    if (
        offset != 84
    )
    {
        Serial.println(
            "[CRYPTO] ERROR: input is not 84 bytes."
        );

        return false;
    }


    // ================================================================
    // SHA-256
    // ================================================================

    mbedtls_md_context_t ctx;

    mbedtls_md_init(&ctx);


    const mbedtls_md_info_t* info =
        mbedtls_md_info_from_type(
            MBEDTLS_MD_SHA256
        );


    if (!info)
    {
        mbedtls_md_free(&ctx);

        return false;
    }


    if (
        mbedtls_md_setup(
            &ctx,
            info,
            0
        ) != 0
    )
    {
        mbedtls_md_free(&ctx);

        return false;
    }


    mbedtls_md_starts(
        &ctx
    );

    mbedtls_md_update(
        &ctx,
        raw,
        84
    );


    uint8_t digest[32];


    mbedtls_md_finish(
        &ctx,
        digest
    );


    mbedtls_md_free(
        &ctx
    );


    for (
        int i = 0;
        i < 32;
        i++
    )
    {
        sprintf(
            &outputHash[i * 2],
            "%02x",
            digest[i]
        );
    }


    outputHash[64] =
        '\0';


    return true;
}


// ====================================================================
// PRINT CRYPTO BLOCK
// ====================================================================

void printCryptoBlock(
    const TelemetryBlock& block,
    const char* previousHash
)
{
    Serial.println();

    Serial.println(
        "================================================"
    );

    Serial.println(
        " EXPERIMENTAL CRYPTOGRAPHIC BLOCK"
    );

    Serial.println(
        "================================================"
    );

    Serial.print(
        "Sequence       : "
    );

    Serial.println(
        block.sequence
    );

    Serial.print(
        "Timestamp      : "
    );

    Serial.println(
        uint64ToString(
            block.timestamp
        )
    );

    Serial.print(
        "Node ID        : "
    );

    Serial.println(
        NODE_ID
    );

    Serial.print(
        "Previous hash  : "
    );

    Serial.println(
        previousHash
    );

    Serial.print(
        "64-bit payload : "
    );

    Serial.println(
        uint64ToString(
            block.payload
        )
    );

    Serial.print(
        "Payload HEX    : 0x"
    );

    Serial.println(
        uint64ToHex(
            block.payload
        )
    );

    Serial.print(
        "SHA-256        : "
    );

    Serial.println(
        block.hash_signature
    );

    Serial.println(
        "Hash input     : 84 bytes"
    );

    Serial.println(
        "================================================"
    );
}


// ====================================================================
// SEND BLOCK
// ====================================================================

int sendBlock(
    const TelemetryBlock& block
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        return -1;
    }


    HTTPClient http;

    http.setTimeout(7000);


    if (
        !http.begin(
            GATEWAY_URL
        )
    )
    {
        return -2;
    }


    http.addHeader(
        "Content-Type",
        "application/json"
    );


    String json =
        "{";

    json +=
        "\"timestamp\":" +
        uint64ToString(
            block.timestamp
        ) +
        ",";

    json +=
        "\"node_id\":" +
        String(NODE_ID) +
        ",";

    json +=
        "\"payload\":\"" +
        uint64ToString(
            block.payload
        ) +
        "\",";

    json +=
        "\"hash_signature\":\"" +
        String(
            block.hash_signature
        ) +
        "\",";

    json +=
        "\"execution_ms\":" +
        String(millis()) +
        ",";

    json +=
        "\"free_heap_bytes\":" +
        String(
            ESP.getFreeHeap()
        ) +
        ",";

    json +=
        "\"wifi_rssi_dbm\":" +
        String(
            WiFi.RSSI()
        );

    json +=
        "}";


    Serial.println();

    Serial.println(
        "[NETWORK] JSON payload:"
    );

    Serial.println(
        json
    );


    int code =
        http.POST(json);


    Serial.print(
        "[NETWORK] HTTP code: "
    );

    Serial.println(
        code
    );


    if (
        code > 0
    )
    {
        String response =
            http.getString();

        Serial.println(
            "[NETWORK] Gateway response:"
        );

        Serial.println(
            response
        );
    }


    http.end();


    return code;
}


// ====================================================================
// COMMIT ACCEPTED BLOCK
// ====================================================================

bool commitAcceptedBlock(
    const TelemetryBlock& block
)
{
    /*
    IMPORTANT:

    State is changed only after HTTP 200.
    */

    state.sequence =
        block.sequence + 1;

    state.next_timestamp =
        block.timestamp +
        SLEEP_SECONDS;

    strncpy(
        state.last_hash,
        block.hash_signature,
        64
    );

    state.last_hash[64] =
        '\0';


    Serial.println();

    Serial.println(
        "[STATE] Committing accepted block..."
    );


    return saveState();
}


// ====================================================================
// DEEP SLEEP
// ====================================================================

void goToDeepSleep()
{
    Serial.println();

    Serial.println(
        "[POWER] Preparing deep sleep..."
    );

    Serial.print(
        "[POWER] Sleep duration: "
    );

    Serial.print(
        SLEEP_SECONDS
    );

    Serial.println(
        " seconds"
    );

    Serial.flush();


    WiFi.disconnect(
        true
    );

    WiFi.mode(
        WIFI_OFF
    );


    /*
    ---------------------------------------------------------------
    FIXED C++ CAST

    Correct:
        (uint64_t)SLEEP_SECONDS

    ---------------------------------------------------------------
    */

    uint64_t sleepTimeUs =
        (uint64_t)SLEEP_SECONDS *
        1000000ULL;


    esp_sleep_enable_timer_wakeup(
        sleepTimeUs
    );


    esp_deep_sleep_start();
}


// ====================================================================
// SETUP
// ====================================================================

void setup()
{
    Serial.begin(
        115200
    );

    delay(
        1000
    );


    Serial.println();
    Serial.println();

    Serial.println(
        "################################################"
    );

    Serial.println(
        "# AGRODBHIVE V2.2                            #"
    );

    Serial.println(
        "# INTERMITTENT TELEMETRY CACHE EXPERIMENT    #"
    );

    Serial.println(
        "# PERSISTENT BLOCKCHAIN STATE + FIFO REPLAY  #"
    );

    Serial.println(
        "################################################"
    );


    // ================================================================
    // LITTLEFS
    // ================================================================

    if (
        !initializeLittleFS()
    )
    {
        return;
    }


    // ================================================================
    // LOAD STATE
    // ================================================================

    loadState();

    printPendingSummary();


    // ================================================================
    // SENSORS
    // ================================================================

    initializeSensors();


    // ================================================================
    // WIFI
    // ================================================================

    bool wifiConnected =
        connectWiFi();

    // ================================================================
    // V2.2 UTC REFERENCE
    // ================================================================
    // NTP is used only to establish/correct absolute UTC. Existing
    // blockchain state is never moved backwards. Offline blocks keep
    // their deterministic 35-s timestamp continuation.

    if (wifiConnected)
    {
        bool ntpValid = synchronizeUtcClock();

        if (!ntpValid && !utcTimeValid(state.next_timestamp))
        {
            Serial.println("[TIME] ERROR: NTP failed and no persisted UTC reference exists.");
            goToDeepSleep();
            return;
        }
    }
    else if (!utcTimeValid(state.next_timestamp))
    {
        Serial.println("[TIME] ERROR: no valid UTC reference is available.");
        Serial.println("[TIME] First connect the ESP32 to the gateway/NTP once before an offline run.");
        goToDeepSleep();
        return;
    }


    // ================================================================
    // REPLAY OFFLINE QUEUE FIRST
    // ================================================================
    //
    // Do NOT call synchronizeWithGateway while pending blocks exist.
    // The local state already contains the post-cache tail and the
    // cached blocks must be replayed in exact FIFO order.
    // ================================================================

    if (wifiConnected)
    {
        if (
            state.pending_count > 0
        )
        {
            Serial.println();
            Serial.println(
                "[INTERMITTENT] Connectivity restored."
            );

            Serial.println(
                "[INTERMITTENT] Replaying offline telemetry queue."
            );

            if (
                !replayPendingBlocks()
            )
            {
                Serial.println(
                    "[INTERMITTENT] Replay incomplete."
                );

                goToDeepSleep();
                return;
            }
        }

        // A successful replay means the connectivity-loss interval has
        // ended. Close the persistent journal only after the FIFO is empty.
        if (offlineJournalState.active && state.pending_count == 0)
        {
            offlineJournalCloseIfActive();
        }
    }


    // ================================================================
    // ACQUISITION
    // ================================================================

    SensorData data;

    if (
        !acquireSensors(data)
    )
    {
        Serial.println();
        Serial.println(
            "[FATAL] Invalid sensor measurement."
        );

        Serial.println(
            "[FATAL] Telemetry will NOT be generated."
        );

        goToDeepSleep();
        return;
    }


    // ================================================================
    // PAYLOAD
    // ================================================================

    uint64_t payload =
        packExperimentalPayload(
            data
        );


    printPayload(
        data,
        payload
    );


    // ================================================================
    // IF NO WIFI -> CREATE AND CACHE EXACT BLOCK
    // ================================================================

    if (!wifiConnected)
    {
        Serial.println();
        Serial.println(
            "[OFFLINE] Network unavailable."
        );

        Serial.println(
            "[OFFLINE] Generating persistent telemetry block."
        );

        TelemetryBlock block{};

        block.sequence =
            state.sequence;

        block.timestamp =
            state.next_timestamp;

        block.payload =
            payload;


        if (
            !generateHash(
                block,
                state.last_hash,
                block.hash_signature
            )
        )
        {
            Serial.println(
                "[CRYPTO] Hash generation failed."
            );

            goToDeepSleep();
            return;
        }


        printCryptoBlock(
            block,
            state.last_hash
        );


        Serial.println(
            "[CACHE] OFFLINE BLOCK"
        );

        if (
            cacheBlockAndAdvanceState(
                block,
                state.last_hash
            )
        )
        {
            Serial.println(
                "[CACHE] OFFLINE BLOCK PERSISTED SUCCESSFULLY."
            );

            Serial.print(
                "[CACHE] Pending blocks: "
            );

            Serial.println(
                state.pending_count
            );
        }
        else
        {
            Serial.println(
                "[CACHE] ERROR: offline block was not fully persisted."
            );
        }

        goToDeepSleep();
        return;
    }


    // ================================================================
    // GATEWAY SYNCHRONIZATION
    // ================================================================

    // No pending queue exists here, so synchronization is safe.
    bool synchronized =
        synchronizeWithGateway();


    if (!synchronized)
    {
        Serial.println(
            "[SYNC] Gateway synchronization failed."
        );

        Serial.println(
            "[SYNC] Local persistent state will be used."
        );
    }


    // ================================================================
    // CREATE CURRENT BLOCK
    // ================================================================

    TelemetryBlock block{};

    block.sequence =
        state.sequence;

    block.timestamp =
        state.next_timestamp;

    block.payload =
        payload;


    // ================================================================
    // GENERATE HASH
    // ================================================================

    if (
        !generateHash(
            block,
            state.last_hash,
            block.hash_signature
        )
    )
    {
        Serial.println(
            "[CRYPTO] Hash generation failed."
        );

        goToDeepSleep();
        return;
    }


    printCryptoBlock(
        block,
        state.last_hash
    );


    // ================================================================
    // SEND
    // ================================================================

    int httpCode =
        sendBlock(
            block
        );


    // ================================================================
    // SUCCESS
    // ================================================================

    if (
        httpCode == 200
    )
    {
        Serial.println();
        Serial.println(
            "[+] GATEWAY ACCEPTED BLOCK."
        );

        if (
            commitAcceptedBlock(
                block
            )
        )
        {
            Serial.println(
                "[STATE] Blockchain state committed."
            );
        }
        else
        {
            Serial.println(
                "[STATE] WARNING: persistent commit failed."
            );
        }

        goToDeepSleep();
        return;
    }


    // ================================================================
    // RECOVERABLE CRYPTO / TIMESTAMP ERROR
    // ================================================================

    if (
        httpCode == 401 ||
        httpCode == 409
    )
    {
        Serial.println();
        Serial.println(
            "[NETWORK] Gateway rejected block."
        );

        Serial.println(
            "[NETWORK] Performing blockchain resynchronization..."
        );


        if (
            synchronizeWithGateway()
        )
        {
            block.sequence =
                state.sequence;

            block.timestamp =
                state.next_timestamp;

            block.payload =
                payload;

            memset(
                block.hash_signature,
                0,
                sizeof(block.hash_signature)
            );


            if (
                generateHash(
                    block,
                    state.last_hash,
                    block.hash_signature
                )
            )
            {
                Serial.println();
                Serial.println(
                    "[NETWORK] RETRY AFTER SYNCHRONIZATION"
                );

                printCryptoBlock(
                    block,
                    state.last_hash
                );


                int retryCode =
                    sendBlock(
                        block
                    );


                if (
                    retryCode == 200
                )
                {
                    Serial.println();
                    Serial.println(
                        "[+] SYNCHRONIZED BLOCK ACCEPTED."
                    );

                    if (
                        commitAcceptedBlock(
                            block
                        )
                    )
                    {
                        Serial.println(
                            "[STATE] Blockchain state committed."
                        );
                    }

                    goToDeepSleep();
                    return;
                }

                Serial.print(
                    "[-] Retry failed. HTTP: "
                );

                Serial.println(
                    retryCode
                );
            }
        }
    }


    // ================================================================
    // FAILURE -> PERSISTENT OFFLINE CACHE
    // ================================================================

    Serial.println();
    Serial.print(
        "[-] GATEWAY REJECTED BLOCK. HTTP: "
    );

    Serial.println(
        httpCode
    );

    Serial.println(
        "[INTERMITTENT] Treating transmission failure as offline event."
    );

    Serial.println(
        "[CACHE] Persisting exact unsent block for later replay."
    );


    // IMPORTANT:
    // The exact block and exact previous hash are cached.
    // State advances only after the cache write succeeds.

    if (
        cacheBlockAndAdvanceState(
            block,
            state.last_hash
        )
    )
    {
        Serial.println(
            "[CACHE] FAILED TRANSMISSION STORED SUCCESSFULLY."
        );

        Serial.print(
            "[CACHE] Pending blocks: "
        );

        Serial.println(
            state.pending_count
        );
    }
    else
    {
        Serial.println(
            "[CACHE] ERROR: failed transmission could not be persisted."
        );
    }


    goToDeepSleep();
}


// ====================================================================
// LOOP
// ====================================================================

void loop()
{
    /*
    All work is performed in setup().
    ESP32 enters deep sleep afterwards.
    */
}
/*
====================================================================
 AGRODBHIVE V2.2 FORENSIC READ-ONLY DECODER — CORRECTED
====================================================================

PURPOSE
-------
Read-only forensic inspection of the existing V2.2 LittleFS files.

IMPORTANT
---------
- Does NOT write to LittleFS.
- Does NOT delete or rename any file.
- Does NOT call LittleFS.begin(true).
- Does NOT use Wi-Fi.
- Does NOT send telemetry.
- Does NOT modify deployment state.

FILES INSPECTED
---------------
/agro_offline_state.bin
/agro_offline_events.bin
/agro_pending.bin

CRITICAL CHECKSUM FIX
---------------------
The original V2.2 firmware calculates OfflineEventRecord checksum
with:

    sizeof(OfflineEventRecord) - sizeof(uint32_t)

On ESP32 the structure is 56 bytes because of alignment/padding.
The checksum field is at byte offset 48.

Therefore the firmware hashes bytes 0..51 while the checksum field
(bytes 48..51) is still ZERO, and then stores the resulting checksum
at bytes 48..51.

The previous forensic decoder reconstructed the record with the
stored checksum already present and consequently hashed the stored
checksum itself. That produced false FAIL results.

This decoder reproduces the firmware's ACTUAL algorithm:
    - read the 56-byte record exactly as stored
    - copy it to a temporary buffer
    - zero bytes 48..51
    - FNV-1a over bytes 0..51
    - compare with stored checksum at bytes 48..51

This is a decoder correction only. The original V2.2 data is untouched.
*/

#include <Arduino.h>
#include <LittleFS.h>
#include <mbedtls/md.h>
#include <time.h>

static const char* OFFLINE_STATE_FILE  = "/agro_offline_state.bin";
static const char* OFFLINE_EVENTS_FILE = "/agro_offline_events.bin";
static const char* PENDING_FILE        = "/agro_pending.bin";

static const uint32_t OFFLINE_STATE_MAGIC = 0x4F465332UL; // OFS2
static const uint32_t OFFLINE_EVENT_MAGIC = 0x4F464556UL; // OFEV
static const uint32_t PENDING_MAGIC       = 0x50454E44UL; // PEND
static const uint32_t JOURNAL_VERSION     = 1;

static const size_t OFFLINE_STATE_SIZE = 32;
static const size_t OFFLINE_EVENT_SIZE = 56;
static const size_t PENDING_SIZE       = 176;

static uint32_t readU32LE(const uint8_t* p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t readU64LE(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

static void printU64(uint64_t v)
{
    Serial.print((unsigned long long)v);
}

static uint32_t fnv1a(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261UL;

    for (size_t i = 0; i < len; ++i)
    {
        h ^= data[i];
        h *= 16777619UL;
    }

    return h;
}

static String hexU32(uint32_t v)
{
    char b[11];
    snprintf(b, sizeof(b), "0x%08lX", (unsigned long)v);
    return String(b);
}

static String utcString(uint64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tmv;

    gmtime_r(&t, &tmv);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return String(buf);
}

static String ascii64(const uint8_t* p)
{
    char s[65];
    memcpy(s, p, 64);
    s[64] = '\0';
    return String(s);
}

static void printHexBytes(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (i % 16 == 0)
        {
            Serial.println();
            Serial.printf("%04u: ", (unsigned)i);
        }

        Serial.printf("%02X ", p[i]);
    }

    Serial.println();
}

/*
--------------------------------------------------------------------
STATE
--------------------------------------------------------------------
Firmware structure:

uint32 magic       offset 0
uint32 version     offset 4
uint32 active      offset 8
uint32 event_id    offset 12
uint64 start       offset 16
uint32 generated   offset 24
uint32 checksum    offset 28

Firmware checksum length = sizeof(state) - 4 = 28 bytes.
Therefore hash bytes 0..27.
--------------------------------------------------------------------
*/
static bool inspectOfflineState()
{
    Serial.println();
    Serial.println("============================================================");
    Serial.println(" OFFLINE JOURNAL STATE — READ ONLY");
    Serial.println("============================================================");

    if (!LittleFS.exists(OFFLINE_STATE_FILE))
    {
        Serial.println("[STATE] File does not exist.");
        return false;
    }

    File f = LittleFS.open(OFFLINE_STATE_FILE, FILE_READ);

    if (!f)
    {
        Serial.println("[STATE] ERROR: cannot open file.");
        return false;
    }

    size_t sz = f.size();

    Serial.printf("[STATE] File size: %u bytes\n", (unsigned)sz);

    if (sz != OFFLINE_STATE_SIZE)
    {
        Serial.println("[STATE] STRUCTURE: INVALID SIZE");
        f.close();
        return false;
    }

    uint8_t b[OFFLINE_STATE_SIZE];

    size_t n = f.read(b, sizeof(b));
    f.close();

    if (n != sizeof(b))
    {
        Serial.println("[STATE] ERROR: incomplete read.");
        return false;
    }

    uint32_t magic     = readU32LE(b + 0);
    uint32_t version   = readU32LE(b + 4);
    uint32_t active    = readU32LE(b + 8);
    uint32_t eventId   = readU32LE(b + 12);
    uint64_t startTs   = readU64LE(b + 16);
    uint32_t generated = readU32LE(b + 24);
    uint32_t stored    = readU32LE(b + 28);

    uint32_t calculated = fnv1a(b, 28);

    Serial.printf("[STATE] Magic            : 0x%08lX\n",
                  (unsigned long)magic);
    Serial.printf("[STATE] Version          : %lu\n",
                  (unsigned long)version);
    Serial.printf("[STATE] Active           : %lu\n",
                  (unsigned long)active);
    Serial.printf("[STATE] Event ID         : %lu\n",
                  (unsigned long)eventId);

    Serial.print("[STATE] Start timestamp  : ");
    printU64(startTs);
    Serial.println();

    if (startTs > 0)
    {
        Serial.print("[STATE] Start UTC        : ");
        Serial.println(utcString(startTs));
    }

    Serial.printf("[STATE] Generated count  : %lu\n",
                  (unsigned long)generated);

    Serial.printf("[STATE] Stored checksum  : %s\n",
                  hexU32(stored).c_str());

    Serial.printf("[STATE] Calculated       : %s\n",
                  hexU32(calculated).c_str());

    bool ok =
        magic == OFFLINE_STATE_MAGIC &&
        version == JOURNAL_VERSION &&
        stored == calculated;

    Serial.print("[STATE] INTEGRITY        : ");
    Serial.println(ok ? "PASS" : "FAIL");

    return ok;
}

/*
--------------------------------------------------------------------
OFFLINE EVENT RECORD
--------------------------------------------------------------------

Actual ESP32 layout from the V2.2 C++ structure:

offset  0 : magic              uint32
offset  4 : version            uint32
offset  8 : event_id           uint32
offset 12 : alignment padding  4 bytes
offset 16 : start_timestamp    uint64
offset 24 : end_timestamp      uint64
offset 32 : generated_count    uint32
offset 36 : alignment padding  4 bytes
offset 40 : duration_seconds   uint64
offset 48 : checksum           uint32
offset 52 : trailing padding   4 bytes

sizeof(OfflineEventRecord) = 56.

Firmware checksum:
    length = 56 - 4 = 52
    bytes 0..51 are hashed
BUT at checksum-generation time bytes 48..51 are still zero,
because event{} zero-initializes the structure before the checksum
is assigned.

The corrected forensic validation therefore:
    temp[48..51] = 0
    calculated = FNV1a(temp[0..51])
--------------------------------------------------------------------
*/
static bool inspectOfflineEvents()
{
    Serial.println();
    Serial.println("============================================================");
    Serial.println(" OFFLINE EVENT JOURNAL — CORRECTED CHECKSUM VALIDATION");
    Serial.println("============================================================");

    if (!LittleFS.exists(OFFLINE_EVENTS_FILE))
    {
        Serial.println("[JOURNAL] File does not exist.");
        return false;
    }

    File f = LittleFS.open(OFFLINE_EVENTS_FILE, FILE_READ);

    if (!f)
    {
        Serial.println("[JOURNAL] ERROR: cannot open file.");
        return false;
    }

    size_t fileSize = f.size();

    Serial.printf("[JOURNAL] File size: %u bytes\n",
                  (unsigned)fileSize);

    if (fileSize % OFFLINE_EVENT_SIZE != 0)
    {
        Serial.println("[JOURNAL] STRUCTURE: INVALID — not divisible by 56.");
        f.close();
        return false;
    }

    uint32_t count = fileSize / OFFLINE_EVENT_SIZE;

    Serial.printf("[JOURNAL] Record size: %u bytes\n",
                  (unsigned)OFFLINE_EVENT_SIZE);
    Serial.printf("[JOURNAL] Complete records: %lu\n",
                  (unsigned long)count);

    uint32_t valid = 0;
    uint32_t invalid = 0;
    uint64_t generatedTotal = 0;
    uint64_t offlineTimeTotal = 0;

    uint64_t previousEnd = 0;
    uint32_t previousId = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t b[OFFLINE_EVENT_SIZE];

        size_t n = f.read(b, sizeof(b));

        if (n != sizeof(b))
        {
            Serial.printf("[EVENT] %lu: incomplete record.\n",
                          (unsigned long)(i + 1));
            invalid++;
            break;
        }

        uint32_t magic     = readU32LE(b + 0);
        uint32_t version   = readU32LE(b + 4);
        uint32_t eventId   = readU32LE(b + 8);
        uint64_t startTs   = readU64LE(b + 16);
        uint64_t endTs     = readU64LE(b + 24);
        uint32_t generated = readU32LE(b + 32);
        uint64_t duration  = readU64LE(b + 40);
        uint32_t stored    = readU32LE(b + 48);

        /*
         * Exact reproduction of firmware checksum generation.
         */
        uint8_t temp[OFFLINE_EVENT_SIZE];
        memcpy(temp, b, sizeof(temp));

        temp[48] = 0;
        temp[49] = 0;
        temp[50] = 0;
        temp[51] = 0;

        uint32_t calculated = fnv1a(temp, 52);

        bool checksumOK = (stored == calculated);
        bool structureOK =
            magic == OFFLINE_EVENT_MAGIC &&
            version == JOURNAL_VERSION &&
            endTs >= startTs &&
            duration == (endTs - startTs);

        bool sequenceOK =
            (i == 0) || (eventId == previousId + 1);

        bool ok =
            checksumOK &&
            structureOK &&
            sequenceOK;

        Serial.println();
        Serial.println("------------------------------------------------------------");
        Serial.printf("[EVENT] ID                 : %lu\n",
                      (unsigned long)eventId);

        Serial.print("[EVENT] Start epoch        : ");
        printU64(startTs);
        Serial.println();

        Serial.print("[EVENT] Start UTC          : ");
        Serial.println(utcString(startTs));

        Serial.print("[EVENT] End epoch          : ");
        printU64(endTs);
        Serial.println();

        Serial.print("[EVENT] End UTC            : ");
        Serial.println(utcString(endTs));

        Serial.print("[EVENT] Duration           : ");
        printU64(duration);
        Serial.println(" seconds");

        Serial.printf("[EVENT] Generated records  : %lu\n",
                      (unsigned long)generated);

        Serial.printf("[EVENT] Stored checksum    : %s\n",
                      hexU32(stored).c_str());

        Serial.printf("[EVENT] Calculated checksum: %s\n",
                      hexU32(calculated).c_str());

        Serial.print("[EVENT] Checksum           : ");
        Serial.println(checksumOK ? "PASS" : "FAIL");

        Serial.print("[EVENT] Structure          : ");
        Serial.println(structureOK ? "PASS" : "FAIL");

        Serial.print("[EVENT] Event sequence     : ");
        Serial.println(sequenceOK ? "PASS" : "FAIL");

        Serial.print("[EVENT] INTEGRITY          : ");
        Serial.println(ok ? "PASS" : "FAIL");

        if (ok)
            valid++;
        else
            invalid++;

        generatedTotal += generated;
        offlineTimeTotal += duration;

        previousEnd = endTs;
        previousId = eventId;
    }

    f.close();

    Serial.println();
    Serial.println("============================================================");
    Serial.println(" JOURNAL SUMMARY");
    Serial.println("============================================================");

    Serial.printf("Complete journal records : %lu\n",
                  (unsigned long)count);
    Serial.printf("Valid journal records    : %lu\n",
                  (unsigned long)valid);
    Serial.printf("Invalid journal records  : %lu\n",
                  (unsigned long)invalid);

    Serial.printf("Generated offline total  : %llu\n",
                  (unsigned long long)generatedTotal);

    Serial.printf("Offline time total       : %llu seconds\n",
                  (unsigned long long)offlineTimeTotal);

    Serial.print("Journal integrity result : ");
    Serial.println(invalid == 0 ? "PASS" : "FAIL");

    return invalid == 0;
}

/*
--------------------------------------------------------------------
PENDING FIFO CACHE
--------------------------------------------------------------------

Actual V2.2 PendingRecord layout:

offset  0 : magic
offset  4 : alignment padding
offset  8 : TelemetryBlock
           sequence  8
           padding 12..15
           timestamp 16..23
           payload   24..31
           hash      32..96 (65 bytes)
           padding 97..103
offset 104 : previous_hash 65 bytes
offset 169 : trailing padding
total = 176 bytes

No checksum exists for PendingRecord itself.

We therefore perform:
1. structural validation
2. sequence extraction
3. timestamp extraction
4. previous hash extraction
5. block hash extraction
6. independent SHA-256 validation over the exact 84-byte input
--------------------------------------------------------------------
*/
static bool sha256Block(
    uint64_t timestamp,
    uint32_t nodeId,
    const uint8_t* previousHash64,
    uint64_t payload,
    char* outHex
)
{
    uint8_t raw[84];
    size_t off = 0;

    for (int i = 0; i < 8; ++i)
        raw[off++] = (timestamp >> (8 * i)) & 0xFF;

    for (int i = 0; i < 4; ++i)
        raw[off++] = (nodeId >> (8 * i)) & 0xFF;

    memcpy(raw + off, previousHash64, 64);
    off += 64;

    for (int i = 0; i < 8; ++i)
        raw[off++] = (payload >> (8 * i)) & 0xFF;

    if (off != 84)
        return false;

    unsigned char digest[32];

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    const mbedtls_md_info_t* info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (!info)
    {
        mbedtls_md_free(&ctx);
        return false;
    }

    if (mbedtls_md_setup(&ctx, info, 0) != 0)
    {
        mbedtls_md_free(&ctx);
        return false;
    }

    if (mbedtls_md_starts(&ctx) != 0 ||
        mbedtls_md_update(&ctx, raw, sizeof(raw)) != 0 ||
        mbedtls_md_finish(&ctx, digest) != 0)
    {
        mbedtls_md_free(&ctx);
        return false;
    }

    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32; ++i)
        sprintf(outHex + (i * 2), "%02x", digest[i]);

    outHex[64] = '\0';

    return true;
}

static bool inspectPending()
{
    Serial.println();
    Serial.println("============================================================");
    Serial.println(" PENDING FIFO CACHE — READ ONLY");
    Serial.println("============================================================");

    if (!LittleFS.exists(PENDING_FILE))
    {
        Serial.println("[FIFO] File does not exist.");
        Serial.println("[FIFO] This is not by itself evidence of failure.");
        Serial.println("[FIFO] It may mean the queue was completely replayed.");
        return true;
    }

    File f = LittleFS.open(PENDING_FILE, FILE_READ);

    if (!f)
    {
        Serial.println("[FIFO] ERROR: cannot open file.");
        return false;
    }

    size_t fileSize = f.size();

    Serial.printf("[FIFO] File size       : %u bytes\n",
                  (unsigned)fileSize);

    Serial.printf("[FIFO] Record size     : %u bytes\n",
                  (unsigned)PENDING_SIZE);

    if (fileSize % PENDING_SIZE != 0)
    {
        Serial.println("[FIFO] STRUCTURE       : INVALID SIZE");
        f.close();
        return false;
    }

    uint32_t count = fileSize / PENDING_SIZE;

    Serial.printf("[FIFO] Complete records: %lu\n",
                  (unsigned long)count);

    uint32_t validStructure = 0;
    uint32_t validHash = 0;

    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t b[PENDING_SIZE];

        size_t n = f.read(b, sizeof(b));

        if (n != sizeof(b))
        {
            Serial.println("[FIFO] ERROR: incomplete record.");
            break;
        }

        uint32_t magic    = readU32LE(b + 0);
        uint32_t sequence = readU32LE(b + 8);
        uint64_t timestamp = readU64LE(b + 16);
        uint64_t payload   = readU64LE(b + 24);

        String blockHash = ascii64(b + 32);
        String prevHash  = ascii64(b + 104);

        bool structureOK =
            magic == PENDING_MAGIC &&
            blockHash.length() == 64 &&
            prevHash.length() == 64;

        char calculatedHash[65];
        bool hashCalcOK =
            sha256Block(
                timestamp,
                1,
                b + 104,
                payload,
                calculatedHash
            );

        bool hashOK =
            hashCalcOK &&
            blockHash.equalsIgnoreCase(calculatedHash);

        Serial.println();
        Serial.println("------------------------------------------------------------");
        Serial.printf("[FIFO] Record index    : %lu\n",
                      (unsigned long)i);
        Serial.printf("[FIFO] Magic            : 0x%08lX\n",
                      (unsigned long)magic);
        Serial.printf("[FIFO] Sequence         : %lu\n",
                      (unsigned long)sequence);

        Serial.print("[FIFO] Timestamp        : ");
        printU64(timestamp);
        Serial.println();

        Serial.print("[FIFO] Timestamp UTC    : ");
        Serial.println(utcString(timestamp));

        Serial.print("[FIFO] Payload          : ");
        printU64(payload);
        Serial.println();

        Serial.print("[FIFO] Payload HEX      : 0x");
        Serial.printf("%016llX\n",
                      (unsigned long long)payload);

        Serial.print("[FIFO] Previous hash    : ");
        Serial.println(prevHash);

        Serial.print("[FIFO] Block hash       : ");
        Serial.println(blockHash);

        Serial.print("[FIFO] STRUCTURE        : ");
        Serial.println(structureOK ? "VALID" : "INVALID");

        if (hashCalcOK)
        {
            Serial.print("[FIFO] SHA-256          : ");
            Serial.println(hashOK ? "PASS" : "FAIL");

            if (hashOK)
                validHash++;
        }
        else
        {
            Serial.println("[FIFO] SHA-256          : CALCULATION ERROR");
        }

        if (structureOK)
            validStructure++;
    }

    f.close();

    Serial.println();
    Serial.println("============================================================");
    Serial.println(" FIFO SUMMARY");
    Serial.println("============================================================");

    Serial.printf("Complete records : %lu\n",
                  (unsigned long)count);

    Serial.printf("Valid structures : %lu\n",
                  (unsigned long)validStructure);

    Serial.printf("Valid SHA-256     : %lu\n",
                  (unsigned long)validHash);

    Serial.println();
    Serial.println("NOTE:");
    Serial.println("If this file is absent, it may simply mean that all");
    Serial.println("cached records were already replayed and removed.");
    Serial.println("Therefore FIFO absence alone cannot prove that no");
    Serial.println("offline caching occurred.");

    return true;
}

static void printRawPreview(const char* path, size_t maxBytes)
{
    Serial.println();
    Serial.println("============================================================");
    Serial.print(" RAW PREVIEW: ");
    Serial.println(path);
    Serial.println("============================================================");

    if (!LittleFS.exists(path))
    {
        Serial.println("FILE NOT FOUND");
        return;
    }

    File f = LittleFS.open(path, FILE_READ);

    if (!f)
    {
        Serial.println("OPEN FAILED");
        return;
    }

    size_t total = f.size();
    size_t n = total < maxBytes ? total : maxBytes;

    uint8_t buf[256];

    size_t done = 0;

    while (done < n)
    {
        size_t chunk = n - done;
        if (chunk > sizeof(buf))
            chunk = sizeof(buf);

        size_t got = f.read(buf, chunk);

        if (got == 0)
            break;

        Serial.printf("%04X: ", (unsigned)done);

        for (size_t i = 0; i < got; ++i)
            Serial.printf("%02X ", buf[i]);

        Serial.println();

        done += got;
    }

    f.close();

    Serial.printf("Previewed %u / %u bytes.\n",
                  (unsigned)done,
                  (unsigned)total);
}

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("################################################################");
    Serial.println("# AGRODBHIVE V2.2 CORRECTED FORENSIC READ-ONLY DECODER        #");
    Serial.println("################################################################");
    Serial.println();
    Serial.println("READ-ONLY MODE.");
    Serial.println("NO Wi-Fi.");
    Serial.println("NO HTTP.");
    Serial.println("NO TELEMETRY.");
    Serial.println("NO LittleFS writes.");
    Serial.println("NO LittleFS erase/format.");
    Serial.println();

    /*
     * CRITICAL:
     * false = never format the filesystem automatically.
     */
    if (!LittleFS.begin(false))
    {
        Serial.println("[FATAL] LittleFS mount failed.");
        Serial.println("[FATAL] No filesystem modification was attempted.");
        return;
    }

    Serial.println("[OK] LittleFS mounted READ-ONLY by this sketch.");

    bool stateOK  = inspectOfflineState();
    bool journalOK = inspectOfflineEvents();
    bool fifoOK   = inspectPending();

    printRawPreview(OFFLINE_STATE_FILE, 64);
    printRawPreview(OFFLINE_EVENTS_FILE, 256);
    printRawPreview(PENDING_FILE, 256);

    Serial.println();
    Serial.println("################################################################");
    Serial.println("# FINAL FORENSIC SUMMARY                                      #");
    Serial.println("################################################################");

    Serial.print("Offline state checksum : ");
    Serial.println(stateOK ? "PASS" : "FAIL");

    Serial.print("Offline journal        : ");
    Serial.println(journalOK ? "PASS" : "FAIL");

    Serial.print("Pending FIFO read      : ");
    Serial.println(fifoOK ? "PASS" : "FAIL");

    Serial.println();
    Serial.println("IMPORTANT INTERPRETATION:");
    Serial.println("A corrected journal PASS means the stored V2.2 event records");
    Serial.println("are internally consistent according to the firmware checksum.");
    Serial.println("It does NOT by itself prove gateway synchronization.");
    Serial.println("That must be cross-checked against RPi4/DuckDB records.");
    Serial.println();
    Serial.println("Save the COMPLETE Serial Monitor output.");
    Serial.println("Do not upload the original deployment firmware again until");
    Serial.println("this forensic output has been saved.");

    Serial.println();
    Serial.println("################################################################");
    Serial.println("# FORENSIC READ-ONLY COMPLETE                                #");
    Serial.println("################################################################");
}

void loop()
{
    // Intentionally empty.
    // This sketch performs one read-only forensic pass only.
}

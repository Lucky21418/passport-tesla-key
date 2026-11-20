#include "diag_log.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_app_desc.h"

#include <string.h>

#define DIAG_MAGIC       0x44494132u /* "DIA2": layout v2 (512 ring, split sectors) */
#define DIAG_CAP         512
#define DIAG_RAW_CAP     8
/* A dedicated data partition keeps the diagnostic ring out of NVS, so it
 * survives pairing resets and does not compete with key storage. */
#define DIAG_PART_LABEL  "bblog"
#define DIAG_PART_SUBTYPE 0x40
#define DIAG_RING_OFF    0u
#define DIAG_RAW_OFF     0x3000u   /* after the 3-sector ring */
#define DIAG_BUILD_OFF   0x5000u   /* build stamp; a ring from another build is dropped */
#define DIAG_SECTOR      0x1000u
#define DIAG_RAW_LEN  200

typedef struct {
    uint8_t  dir;
    uint8_t  domain;
    uint8_t  action;
    uint8_t  len;
    uint8_t  data[DIAG_RAW_LEN];
} diag_raw_rec_t;

typedef struct {
    uint32_t       magic;
    uint16_t       head;
    uint16_t       count;
    diag_raw_rec_t rec[DIAG_RAW_CAP];
} diag_raw_blob_t;

typedef struct {
    uint32_t seq;
    uint32_t uptime_ms;
    uint8_t  type;
    uint8_t  domain;
    uint8_t  action;
    uint8_t  code;
    int16_t  aux;
    int16_t  pad;
} diag_rec_t;

typedef struct {
    uint32_t   magic;
    uint16_t   head;
    uint16_t   count;
    diag_rec_t rec[DIAG_CAP];
} diag_blob_t;

typedef struct {
    uint32_t magic;
    char     version[32];
    uint8_t  sha[32];
} diag_build_t;

/* The ring must not write into the raw/build sectors: the old 256-record ring
 * was 4112 B, so its last record landed in the raw sector without an erase and
 * a later ring flush destroyed the raw header (field windows decoded to 0 raw
 * frames). Keep the arithmetic honest at compile time. */
_Static_assert((DIAG_RING_OFF % DIAG_SECTOR) == 0u, "ring offset must be sector aligned");
_Static_assert((DIAG_RAW_OFF % DIAG_SECTOR) == 0u, "raw offset must be sector aligned");
_Static_assert((DIAG_BUILD_OFF % DIAG_SECTOR) == 0u, "build offset must be sector aligned");
_Static_assert(DIAG_RING_OFF +
                       ((sizeof(diag_blob_t) + DIAG_SECTOR - 1u) / DIAG_SECTOR) * DIAG_SECTOR <=
                   DIAG_RAW_OFF,
               "ring must not overlap the raw sector");
_Static_assert(sizeof(diag_raw_blob_t) <= DIAG_SECTOR, "raw window must fit one sector");
_Static_assert(DIAG_BUILD_OFF >= DIAG_RAW_OFF + DIAG_SECTOR, "raw must not overlap the build stamp");

static const char *TAG = "diag";
static const esp_partition_t *s_part;
static diag_blob_t s_blob;
static diag_raw_blob_t s_raw;
static bool s_raw_dirty;
static bool s_dirty;
static bool s_urgent;
static int64_t s_last_flush_us;
static uint32_t s_seq;

static void blob_reset(void)
{
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.magic = DIAG_MAGIC;
}

static void part_write(size_t off, const void *data, size_t len)
{
    size_t erase_len;

    if (!s_part || len == 0) {
        return;
    }
    erase_len = ((len + DIAG_SECTOR - 1u) / DIAG_SECTOR) * DIAG_SECTOR;
    if (esp_partition_erase_range(s_part, off, erase_len) != ESP_OK) {
        return;
    }
    (void)esp_partition_write(s_part, off, data, len);
}

static void blob_flush(void)
{
    if (!s_part) {
        return;
    }
    part_write(DIAG_RING_OFF, &s_blob, sizeof(s_blob));
    s_dirty = false;
    s_urgent = false;
    s_last_flush_us = esp_timer_get_time();
}

static void raw_flush(void)
{
    if (!s_raw_dirty || !s_part) {
        return;
    }
    part_write(DIAG_RAW_OFF, &s_raw, sizeof(s_raw));
    s_raw_dirty = false;
}

void diag_log_init(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    diag_build_t build;
    char version[sizeof(build.version)] = { 0 };
    bool loaded = false;
    bool build_ok = false;

    if (desc) {
        memcpy(version, desc->version, sizeof(version));
        version[sizeof(version) - 1] = '\0';
    }
    blob_reset();
    memset(&s_raw, 0, sizeof(s_raw));
    memset(&build, 0, sizeof(build));
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)DIAG_PART_SUBTYPE,
                                      DIAG_PART_LABEL);
    if (s_part) {
        if (esp_partition_read(s_part, DIAG_BUILD_OFF, &build, sizeof(build)) == ESP_OK &&
            build.magic == DIAG_MAGIC &&
            memcmp(build.version, version, sizeof(build.version)) == 0 &&
            (!desc || memcmp(build.sha, desc->app_elf_sha256, sizeof(build.sha)) == 0)) {
            /* The 32-byte version field truncates a dirty suffix; the ELF SHA
             * keeps two local rebuilds distinguishable. */
            build_ok = true;
        }
        if (!build_ok) {
            /* Records from another build cannot be attributed to this code.
             * Drop them now instead of printing a window with the wrong
             * provenance (bblog survives reflashes). */
            memset(&build, 0, sizeof(build));
            build.magic = DIAG_MAGIC;
            memcpy(build.version, version, sizeof(build.version));
            if (desc) {
                memcpy(build.sha, desc->app_elf_sha256, sizeof(build.sha));
            }
            part_write(DIAG_BUILD_OFF, &build, sizeof(build));
            memset(&s_raw, 0, sizeof(s_raw));
            s_raw.magic = DIAG_MAGIC;
            s_seq = 0;
            blob_flush();
            s_raw_dirty = true;
            raw_flush();
            ESP_LOGW(TAG, "black box cleared: build changed to %s", version);
        }
    }
    if (s_part && build_ok) {
        if (esp_partition_read(s_part, DIAG_RING_OFF, &s_blob, sizeof(s_blob)) == ESP_OK &&
            s_blob.magic == DIAG_MAGIC && s_blob.count <= DIAG_CAP &&
            s_blob.head < DIAG_CAP) {
            loaded = true;
        }
        (void)esp_partition_read(s_part, DIAG_RAW_OFF, &s_raw, sizeof(s_raw));
        if (s_raw.magic != DIAG_MAGIC || s_raw.count > DIAG_RAW_CAP ||
            s_raw.head >= DIAG_RAW_CAP) {
            memset(&s_raw, 0, sizeof(s_raw));
        }
    }
    if (!loaded) {
        blob_reset();
    }
    s_raw.magic = DIAG_MAGIC;
    if (s_part) {
        uint8_t sha0 = (desc && sizeof(build.sha) > 0) ? build.sha[0] : 0u;
        uint8_t sha1 = (desc && sizeof(build.sha) > 1) ? build.sha[1] : 0u;
        ESP_LOGI(TAG, "black box build: %s sha=%02x%02x", version, sha0, sha1);
    }
    if (loaded && s_blob.count > 0) {
        unsigned start = (s_blob.head + DIAG_CAP - s_blob.count) % DIAG_CAP;
        ESP_LOGI(TAG, "previous black box: %u record(s)", (unsigned)s_blob.count);
        for (unsigned i = 0; i < s_blob.count; i++) {
            const diag_rec_t *r = &s_blob.rec[(start + i) % DIAG_CAP];
            if (r->seq > s_seq) {
                s_seq = r->seq;
            }
            ESP_LOGI(TAG, "rec seq=%lu up=%lums type=%u dom=%u act=%u code=%d aux=%d",
                     (unsigned long)r->seq, (unsigned long)r->uptime_ms,
                     (unsigned)r->type, (unsigned)r->domain,
                     (unsigned)r->action, (int)r->code, (int)r->aux);
        }
    }
    if (s_raw.count > 0) {
        unsigned start = (s_raw.head + DIAG_RAW_CAP - s_raw.count) % DIAG_RAW_CAP;
        ESP_LOGI(TAG, "previous raw window: %u frame(s)", (unsigned)s_raw.count);
        for (unsigned i = 0; i < s_raw.count; i++) {
            const diag_raw_rec_t *r = &s_raw.rec[(start + i) % DIAG_RAW_CAP];
            char hex[DIAG_RAW_LEN * 2 + 1];
            static const char k_hex[] = "0123456789abcdef";
            unsigned n = r->len > DIAG_RAW_LEN ? DIAG_RAW_LEN : r->len;
            for (unsigned j = 0; j < n; j++) {
                hex[j * 2] = k_hex[r->data[j] >> 4];
                hex[j * 2 + 1] = k_hex[r->data[j] & 0x0F];
            }
            hex[n * 2] = '\0';
            ESP_LOGI(TAG, "raw dir=%u dom=%u act=%u len=%u %s",
                     (unsigned)r->dir, (unsigned)r->domain, (unsigned)r->action, n, hex);
        }
    }
    if (!s_part) {
        ESP_LOGW(TAG, "bblog partition missing; diagnostics are RAM-only");
    }
    /* Keep the previous window across resets so an accidental reboot or a
     * readback does not destroy the last field-test session. */
    diag_event(DIAG_BOOT, 0, 0, 0, 0);
}

void diag_event(uint8_t type, uint8_t domain, uint8_t action, int code, int aux)
{
    diag_rec_t *r;

    r = &s_blob.rec[s_blob.head];
    memset(r, 0, sizeof(*r));
    r->seq = ++s_seq;
    r->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    r->type = type;
    r->domain = domain;
    r->action = action;
    r->code = (uint8_t)code;
    r->aux = (int16_t)aux;
    s_blob.head = (uint16_t)((s_blob.head + 1u) % DIAG_CAP);
    if (s_blob.count < DIAG_CAP) {
        s_blob.count++;
    }
    s_dirty = true;
    s_urgent = (type != DIAG_SEND && type != DIAG_SCAN);
}

void diag_log_tick(void)
{
    int64_t now;

    if (!s_dirty) {
        return;
    }
    now = esp_timer_get_time();
    /* Batch bursts; critical events (fault/disconnect/session) flush next tick. */
    if (s_urgent || now - s_last_flush_us >= 2000000) {
        blob_flush();
    }
    raw_flush();
}

void diag_raw(uint8_t dir, uint8_t domain, uint8_t action, const uint8_t *data, size_t len)
{
    diag_raw_rec_t *r;

    if (!data || len == 0) {
        return;
    }
    r = &s_raw.rec[s_raw.head];
    memset(r, 0, sizeof(*r));
    r->dir = dir;
    r->domain = domain;
    r->action = action;
    r->len = (uint8_t)(len > DIAG_RAW_LEN ? DIAG_RAW_LEN : len);
    memcpy(r->data, data, r->len);
    s_raw.head = (uint16_t)((s_raw.head + 1u) % DIAG_RAW_CAP);
    if (s_raw.count < DIAG_RAW_CAP) {
        s_raw.count++;
    }
    s_raw_dirty = true;
}

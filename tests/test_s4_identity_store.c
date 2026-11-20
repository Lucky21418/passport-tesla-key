/*
 * Official Host Test Suite N13: S4 Versioned Identity Store & NVS Transactions.
 *
 * Tests main/port_nvs.c + components/tesla_key/src/pk_store.c against an
 * in-memory simulated NVS flash backend with deterministic fault injection.
 *
 * Scope boundary:
 *   - Uses explicit synthetic VINs and synthetic test keys only.
 *   - Host in-memory NVS stub is NOT proof of real ESP32-C3 flash power-loss,
 *     NVS page CRC, or "chunk written, index unwritten" behavior.
 *   - Must compile cleanly under -std=c11 -Wall -Wextra -Werror with zero
 *     function-pointer casts (including GCC -Werror=cast-function-type).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "pk_app.h"
#include "pk_ble_adapter.h"
#include "pk_protocol.h"
#include "pk_store.h"
#include "port_internal.h"

/* ==========================================================================
 * Test Harness Accounting
 * ========================================================================== */

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *name)
{
    if (cond) {
        g_pass++;
        printf("PASS: %s\n", name);
    } else {
        g_fail++;
        printf("FAIL: %s\n", name);
    }
}

/* ==========================================================================
 * Deterministic Synthetic Test Vectors (never real VINs or real keys)
 * ========================================================================== */

static const char *SYN_VIN_A = "5YJ3E1EA1PF000001";
static const char *SYN_VIN_B = "5YJS0000000000000";
static const char *SYN_VIN_C = "1TEST000000000003";

static void make_synthetic_keypair(uint8_t tag, uint8_t priv[PK_PROTO_PRIVKEY_LEN],
                                   uint8_t pub[PK_PROTO_PUBKEY_LEN])
{
    uint8_t seed[32];
    size_t i;
    for (i = 0; i < sizeof(seed); i++) {
        seed[i] = (uint8_t)(tag ^ (uint8_t)(i * 13u + 7u));
    }
    if (!pk_proto_p256_keygen(priv, pub, seed)) {
        fprintf(stderr, "FATAL: synthetic p256 keygen failed for tag=0x%02x\n", tag);
        exit(2);
    }
}

static void load_candidate_key(uint8_t tag, uint8_t priv_out[PK_PROTO_PRIVKEY_LEN],
                               uint8_t pub_out[PK_PROTO_PUBKEY_LEN])
{
    pk_proto_key_slot_t *cand = port_slot_keys(PK_VEHICLE_PAIR_CANDIDATE);
    uint8_t priv[PK_PROTO_PRIVKEY_LEN];
    uint8_t pub[PK_PROTO_PUBKEY_LEN];

    make_synthetic_keypair(tag, priv, pub);
    memset(cand, 0, sizeof(*cand));
    cand->has_keypair = true;
    memcpy(cand->privkey, priv, sizeof(priv));
    memcpy(cand->pubkey, pub, sizeof(pub));
    if (priv_out) {
        memcpy(priv_out, priv, sizeof(priv));
    }
    if (pub_out) {
        memcpy(pub_out, pub, sizeof(pub));
    }
}

/* ==========================================================================
 * In-Memory Simulated NVS Backend (PK_NVS_STUB_EXTERN)
 * ========================================================================== */

#define STUB_NVS_MAX_KEYS  16
#define STUB_NVS_KEY_LEN   16
#define STUB_NVS_BLOB_CAP  1024

typedef struct {
    bool    used;
    char    key[STUB_NVS_KEY_LEN];
    uint8_t data[STUB_NVS_BLOB_CAP];
    size_t  len;
} stub_nvs_entry_t;

typedef struct {
    stub_nvs_entry_t entries[STUB_NVS_MAX_KEYS];
} stub_nvs_table_t;

static stub_nvs_table_t g_nvs_work;
static stub_nvs_table_t g_nvs_flash;
static bool g_nvs_immediate_flash_on_set = true;
static int  g_nvs_set_calls = 0;
static int  g_nvs_fail_set_at = 0; /* 1-based; 0 = disabled */
static bool g_nvs_fail_commit = false;

static void nvs_stub_reset(void)
{
    memset(&g_nvs_work, 0, sizeof(g_nvs_work));
    memset(&g_nvs_flash, 0, sizeof(g_nvs_flash));
    g_nvs_immediate_flash_on_set = true;
    g_nvs_set_calls = 0;
    g_nvs_fail_set_at = 0;
    g_nvs_fail_commit = false;
}

static void nvs_stub_power_cut(void)
{
    /* Discard uncommitted working table (if immediate_flash_on_set was false) */
    g_nvs_work = g_nvs_flash;
    g_nvs_fail_set_at = 0;
    g_nvs_fail_commit = false;
}

static stub_nvs_entry_t *nvs_find_entry(stub_nvs_table_t *tbl, const char *k, bool alloc)
{
    int i;
    int free_idx = -1;
    for (i = 0; i < STUB_NVS_MAX_KEYS; i++) {
        if (tbl->entries[i].used && strcmp(tbl->entries[i].key, k) == 0) {
            return &tbl->entries[i];
        }
        if (!tbl->entries[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (alloc && free_idx >= 0) {
        memset(&tbl->entries[free_idx], 0, sizeof(tbl->entries[free_idx]));
        tbl->entries[free_idx].used = true;
        strncpy(tbl->entries[free_idx].key, k, STUB_NVS_KEY_LEN - 1);
        return &tbl->entries[free_idx];
    }
    return NULL;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)ns;
    (void)mode;
    if (out) {
        *out = 1u;
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    (void)h;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len)
{
    stub_nvs_entry_t *e;
    (void)h;
    if (!k || !len) {
        return ESP_FAIL;
    }
    e = nvs_find_entry(&g_nvs_work, k, false);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        *len = e->len;
        return ESP_FAIL;
    }
    memcpy(out, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *val, size_t len)
{
    stub_nvs_entry_t *e;
    (void)h;
    if (!k || !val || len > STUB_NVS_BLOB_CAP) {
        return ESP_FAIL;
    }
    g_nvs_set_calls++;
    if (g_nvs_fail_set_at > 0 && g_nvs_set_calls == g_nvs_fail_set_at) {
        return ESP_FAIL;
    }
    e = nvs_find_entry(&g_nvs_work, k, true);
    if (!e) {
        return ESP_FAIL;
    }
    memcpy(e->data, val, len);
    e->len = len;
    if (g_nvs_immediate_flash_on_set) {
        g_nvs_flash = g_nvs_work;
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{
    stub_nvs_entry_t *e;
    (void)h;
    if (!k) {
        return ESP_FAIL;
    }
    e = nvs_find_entry(&g_nvs_work, k, false);
    if (!e) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memset(e, 0, sizeof(*e));
    if (g_nvs_immediate_flash_on_set) {
        g_nvs_flash = g_nvs_work;
    }
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    if (g_nvs_fail_commit) {
        return ESP_FAIL;
    }
    g_nvs_flash = g_nvs_work;
    return ESP_OK;
}

/* ==========================================================================
 * Minimal App/Port Binding & Properly Typed BLE Callbacks (NO casts!)
 * ========================================================================== */

static pk_app_t g_app;
static pk_ble_adapter_t g_ble;

pk_app_t *port_bound_app(void)
{
    return &g_app;
}

pk_ble_adapter_t *port_bound_ble(void)
{
    return &g_ble;
}

static pk_err_t dummy_ble_send(void *ctx, const pk_ble_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    return PK_OK;
}

static pk_err_t dummy_ble_cancel(void *ctx, pk_id_t txn_id)
{
    (void)ctx;
    (void)txn_id;
    return PK_OK;
}

static pk_err_t dummy_ble_conn(void *ctx, int slot, pk_ms_t budget)
{
    (void)ctx;
    (void)slot;
    (void)budget;
    return PK_OK;
}

static pk_err_t dummy_ble_disc(void *ctx, int slot)
{
    (void)ctx;
    (void)slot;
    return PK_OK;
}

static bool dummy_ble_mtu(void *ctx, uint16_t *out)
{
    (void)ctx;
    if (out) {
        *out = 247u;
    }
    return true;
}

static void simulate_cold_boot_ex(int fail_set_at)
{
    pk_hal_t hal;
    pk_store_image_t recovered;

    nvs_stub_power_cut();
    g_nvs_set_calls = 0;
    g_nvs_fail_set_at = fail_set_at;
    memset(&g_app, 0, sizeof(g_app));
    memset(&g_ble, 0, sizeof(g_ble));
    memset(&recovered, 0, sizeof(recovered));

    pk_app_init(&g_app, PK_VEHICLE_NONE, true);
    pk_ble_adapter_init(&g_ble);
    g_ble.radio.write_once = dummy_ble_send;
    g_ble.radio.cancel = dummy_ble_cancel;
    g_ble.radio.connect_once = dummy_ble_conn;
    g_ble.radio.disconnect = dummy_ble_disc;
    g_ble.radio.get_mtu = dummy_ble_mtu;

    pk_hal_init_unwired(&hal);
    hal.ble = pk_ble_adapter_transport();
    hal.ble_ctx = &g_ble;
    hal.nvs = port_nvs_ops();
    hal.nvs_ctx = NULL;
    pk_hal_attach(&g_app, &hal);

    port_nvs_load_slot_keys();
    pk_store_recover(&g_app.store, &recovered);
    if (g_app.store.committed) {
        g_app.brightness = (recovered.brightness >= PK_BRIGHTNESS_MIN &&
                            recovered.brightness <= PK_BRIGHTNESS_MAX)
                               ? recovered.brightness
                               : PK_BRIGHTNESS_DEFAULT;
        g_app.power = recovered.power;
        if (recovered.current >= 0 && recovered.current < PK_MAX_VEHICLES &&
            recovered.vehicles[recovered.current].used) {
            g_app.vehicle = recovered.current;
            g_app.paired = pk_store_slot_key_ready(&g_app.store, recovered.current);
            g_app.car_focus = recovered.current;
            if (!g_app.paired) {
                pk_app_set_msg(&g_app, PK_MSG_NOT_PAIRED);
            }
        }
    }
    g_nvs_fail_set_at = 0;
}

static void simulate_cold_boot(void)
{
    simulate_cold_boot_ex(0);
}

/* Helper to read raw kslot_i record from simulated NVS */
typedef struct {
    bool    has_keypair;
    uint8_t privkey[PK_PROTO_PRIVKEY_LEN];
    uint8_t pubkey[PK_PROTO_PUBKEY_LEN];
} test_kslot_rec_t;

static bool stub_read_kslot(int slot, test_kslot_rec_t *out)
{
    char name[16];
    nvs_handle_t h = 0;
    size_t sz = sizeof(*out);
    snprintf(name, sizeof(name), "kslot_%d", slot);
    (void)nvs_open("tesla_key", NVS_READONLY, &h);
    return nvs_get_blob(h, name, out, &sz) == ESP_OK && sz == sizeof(*out);
}

static bool stub_kslot_exists(int slot)
{
    char name[16];
    nvs_handle_t h = 0;
    size_t sz = 0;
    snprintf(name, sizeof(name), "kslot_%d", slot);
    (void)nvs_open("tesla_key", NVS_READONLY, &h);
    return nvs_get_blob(h, name, NULL, &sz) == ESP_OK;
}

static void stub_write_kslot(int slot, const uint8_t priv[PK_PROTO_PRIVKEY_LEN],
                             const uint8_t pub[PK_PROTO_PUBKEY_LEN])
{
    char name[16];
    nvs_handle_t h = 0;
    test_kslot_rec_t rec;
    snprintf(name, sizeof(name), "kslot_%d", slot);
    memset(&rec, 0, sizeof(rec));
    rec.has_keypair = true;
    memcpy(rec.privkey, priv, PK_PROTO_PRIVKEY_LEN);
    memcpy(rec.pubkey, pub, PK_PROTO_PUBKEY_LEN);
    (void)nvs_open("tesla_key", NVS_READWRITE, &h);
    (void)nvs_set_blob(h, name, &rec, sizeof(rec));
    (void)nvs_commit(h);
}

static void stub_erase_kslot(int slot)
{
    char name[16];
    nvs_handle_t h = 0;
    snprintf(name, sizeof(name), "kslot_%d", slot);
    (void)nvs_open("tesla_key", NVS_READWRITE, &h);
    (void)nvs_erase_key(h, name);
    (void)nvs_commit(h);
}

/* ==========================================================================
 * N13.1: Versioned id_v1 Happy Path & Cold-Boot Recovery
 * ========================================================================== */

static void test_n13_1_happy_path(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    pk_proto_key_slot_t *ks0, *ks1;
    size_t sz = 0;
    nvs_handle_t h = 0;

    nvs_stub_reset();
    simulate_cold_boot();

    load_candidate_key(0x11, priv0, pub0);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE),
          "N13.1: add car 0 with candidate key");
    check(pk_store_switch_current(&g_app.store, 0), "N13.1: select car 0");

    load_candidate_key(0x22, priv1, pub1);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE),
          "N13.1: add car 1 with distinct candidate key");

    (void)nvs_open("tesla_key", NVS_READONLY, &h);
    check(nvs_get_blob(h, "id_v1", NULL, &sz) == ESP_OK && sz > 0, "N13.1: id_v1 present");
    check(nvs_get_blob(h, "id_cand", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND,
          "N13.1: id_cand cleared after publish");

    simulate_cold_boot();
    check(pk_store_count(&g_app.store) == 2 && pk_store_current(&g_app.store) == 0,
          "N13.1: cold boot restores 2 cars and current=0");
    check(pk_store_slot_key_ready(&g_app.store, 0) && pk_store_slot_key_ready(&g_app.store, 1),
          "N13.1: both slots key_ready after cold boot");
    ks0 = port_slot_keys(0);
    ks1 = port_slot_keys(1);
    check(ks0 && ks1 &&
          memcmp(ks0->privkey, priv0, PK_PROTO_PRIVKEY_LEN) == 0 &&
          memcmp(ks1->privkey, priv1, PK_PROTO_PRIVKEY_LEN) == 0 &&
          memcmp(ks0->privkey, ks1->privkey, PK_PROTO_PRIVKEY_LEN) != 0,
          "N13.1: per-slot isolated keys restored intact");
}

/* ==========================================================================
 * N13.2a: Unpublished id_cand with Higher Generation Must Never Be Adopted
 * ========================================================================== */

static void test_n13_2a_unpublished_cand_ignored(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    size_t sz = 0;
    nvs_handle_t h = 0;

    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x13, priv0, pub0);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE),
          "N13.2a: baseline car 0");
    check(pk_store_switch_current(&g_app.store, 0), "N13.2a: current=0");

    load_candidate_key(0x24, priv1, pub1);
    check(!pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_PREPARE),
          "N13.2a: crash during prepare returns false");

    (void)nvs_open("tesla_key", NVS_READONLY, &h);
    check(nvs_get_blob(h, "id_cand", NULL, &sz) == ESP_OK,
          "N13.2a: unpublished id_cand with higher generation left on disk before boot");

    simulate_cold_boot();
    check(pk_store_count(&g_app.store) == 1 &&
          pk_store_find_vin(&g_app.store, SYN_VIN_A) == 0 &&
          pk_store_find_vin(&g_app.store, SYN_VIN_B) == PK_VEHICLE_NONE,
          "N13.2a: cold boot discards unpublished higher-gen id_cand; old id_v1 wins");
    check(nvs_get_blob(h, "id_cand", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND,
          "N13.2a: id_cand cleaned up on recovery");
}

/* ==========================================================================
 * N13.2b: Fault Injection at Every nvs_set_blob Call During Add
 * ========================================================================== */

static void test_n13_2b_fault_every_set(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    int total_sets;
    int k;
    bool all_clean = true;

    /* Measure total nvs_set_blob calls during add of car 1 */
    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x31, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    g_nvs_set_calls = 0;
    load_candidate_key(0x32, priv1, pub1);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE),
          "N13.2b: measure baseline add car 1");
    total_sets = g_nvs_set_calls;
    check(total_sets >= 3, "N13.2b: add car 1 performs >= 3 nvs_set_blob calls (cand, kslot, id_v1)");

    for (k = 1; k <= total_sets; k++) {
        nvs_stub_reset();
        simulate_cold_boot();
        load_candidate_key(0x31, priv0, pub0);
        pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
        pk_store_switch_current(&g_app.store, 0);

        g_nvs_set_calls = 0;
        g_nvs_fail_set_at = k;
        load_candidate_key(0x32, priv1, pub1);
        if (pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE)) {
            all_clean = false;
        }
        g_nvs_fail_set_at = 0;

        simulate_cold_boot();
        if (pk_store_count(&g_app.store) != 1 ||
            !pk_store_slot_key_ready(&g_app.store, 0) ||
            stub_kslot_exists(1)) {
            all_clean = false;
        }
    }
    check(all_clean, "N13.2b: every nvs_set_blob fault recovers complete old identity with no leftover kslot_1");
}

/* ==========================================================================
 * N13.3: Delete, Factory Reset, Tombstones & GC
 * ========================================================================== */

static void test_n13_3_delete_and_gc(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    pk_store_image_t live;

    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x41, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);
    load_candidate_key(0x42, priv1, pub1);
    pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE);

    check(pk_store_delete(&g_app.store, 1, false, PK_STORE_CRASH_COMMIT, &live),
          "N13.3: delete car 1 with CRASH_COMMIT");
    simulate_cold_boot();
    check(pk_store_count(&g_app.store) == 1 && !pk_store_slot_used(&g_app.store, 1) &&
          !stub_kslot_exists(1) && pk_store_slot_key_ready(&g_app.store, 0),
          "N13.3: tombstoned car 1 stays deleted and kslot_1 is GC'd");

    check(pk_store_delete(&g_app.store, 0, true, PK_STORE_CRASH_NONE, &live),
          "N13.3: factory reset deletes all cars");
    simulate_cold_boot();
    check(pk_store_count(&g_app.store) == 0 && !stub_kslot_exists(0) && !stub_kslot_exists(1),
          "N13.3: factory reset clears all cars and slot keys");
}

/* ==========================================================================
 * N13.4a: Legacy v0 "live" Migration Without Key Rotation (+ Failure Keeps v0)
 * ========================================================================== */

typedef struct {
    char vin[PK_VIN_BUF];
    char slot_id[PK_SLOT_ID_LEN];
    char key_ref[24];
    bool used;
} test_v0_vehicle_t;

typedef struct {
    uint32_t generation;
    int current;
    int mru[PK_MAX_VEHICLES];
    unsigned mru_count;
    test_v0_vehicle_t vehicles[PK_MAX_VEHICLES];
    unsigned vehicle_count;
    int brightness;
    pk_power_profile_t power;
} test_v0_image_t;

static void seed_legacy_v0(const uint8_t priv0[PK_PROTO_PRIVKEY_LEN],
                           const uint8_t pub0[PK_PROTO_PUBKEY_LEN])
{
    test_v0_image_t v0;
    nvs_handle_t h = 0;

    memset(&v0, 0, sizeof(v0));
    v0.generation = 5u;
    v0.current = 0;
    v0.mru[0] = 0;
    v0.mru_count = 1;
    v0.vehicle_count = 1;
    v0.brightness = 70;
    v0.power = PK_POWER_ENDURANCE;
    v0.vehicles[0].used = true;
    strncpy(v0.vehicles[0].vin, SYN_VIN_A, PK_VIN_LEN);
    snprintf(v0.vehicles[0].slot_id, sizeof(v0.vehicles[0].slot_id), "car0");
    snprintf(v0.vehicles[0].key_ref, sizeof(v0.vehicles[0].key_ref), "wrapped-0");

    (void)nvs_open("tesla_key", NVS_READWRITE, &h);
    (void)nvs_set_blob(h, "live", &v0, sizeof(v0));
    (void)nvs_commit(h);
    stub_write_kslot(0, priv0, pub0);
}

static void test_n13_4a_legacy_v0_migration(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    pk_proto_key_slot_t *ks0;
    nvs_handle_t h = 0;
    size_t sz = 0;

    make_synthetic_keypair(0x55, priv0, pub0);

    /* Case 1: migration write of id_v1 fails -> legacy "live" preserved */
    nvs_stub_reset();
    seed_legacy_v0(priv0, pub0);
    simulate_cold_boot_ex(1); /* fails id_v1 write inside migrate_legacy_v0_to_v1 */
    (void)nvs_open("tesla_key", NVS_READONLY, &h);
    check(nvs_get_blob(h, "live", NULL, &sz) == ESP_OK &&
          nvs_get_blob(h, "id_v1", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND,
          "N13.4a: failed v0->v1 migration preserves legacy live");

    /* Case 2: normal boot migrates v0 -> v1 without rotating privkey and removes "live" */
    g_nvs_fail_set_at = 0;
    simulate_cold_boot();
    ks0 = port_slot_keys(0);
    check(pk_store_count(&g_app.store) == 1 &&
          pk_store_slot_key_ready(&g_app.store, 0) &&
          ks0 != NULL &&
          memcmp(ks0->privkey, priv0, PK_PROTO_PRIVKEY_LEN) == 0 &&
          nvs_get_blob(h, "id_v1", NULL, &sz) == ESP_OK &&
          nvs_get_blob(h, "live", NULL, &sz) == ESP_ERR_NVS_NOT_FOUND,
          "N13.4a: v0->v1 migration preserves private key without rotation and deletes legacy live");
}

/* ==========================================================================
 * N13.4b & N13.4c: Missing Key Cold Boot & Switch Car Stays Unpaired/Corrupted
 * ========================================================================== */

static void test_n13_4bc_missing_key_boot_and_switch(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    pk_intent_t lock_intent;

    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x61, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);
    load_candidate_key(0x62, priv1, pub1);
    pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE);

    stub_erase_kslot(0);
    simulate_cold_boot();

    check(pk_store_slot_key_corrupted(&g_app.store, 0) &&
          !pk_store_slot_key_ready(&g_app.store, 0) &&
          !g_app.paired &&
          g_app.message == PK_MSG_NOT_PAIRED &&
          port_slot_keys(0) == NULL &&
          pk_store_slot_key_ready(&g_app.store, 1),
          "N13.4b: missing kslot_0 -> corrupted, not ready, unpaired, no silent keygen, slot 1 intact");

    lock_intent = pk_app_make_intent(&g_app, PK_ACTION_LOCK);
    check(!pk_app_commit(&g_app, &lock_intent, 1000u),
          "N13.4b: LOCK rejected when current slot key is missing");

    check(pk_app_select_vehicle(&g_app, 1) && g_app.paired && port_slot_keys(1) != NULL,
          "N13.4c: switch to healthy slot 1 succeeds and paired=true");
    check(pk_app_select_vehicle(&g_app, 0) && !g_app.paired &&
          pk_store_slot_key_corrupted(&g_app.store, 0) &&
          !pk_store_slot_key_ready(&g_app.store, 0) &&
          port_slot_keys(0) == NULL,
          "N13.4c: switch back to missing-key slot 0 stays unpaired/corrupted with no key");
}

/* ==========================================================================
 * N13.4d & N13.5a/b: Tampered kslot_0 (mismatch_boot, mismatch_switch, mismatch_prefs)
 * ========================================================================== */

static void setup_tampered_slot0_boot(uint8_t tamper_priv_out[PK_PROTO_PRIVKEY_LEN])
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t priv1[PK_PROTO_PRIVKEY_LEN], pub1[PK_PROTO_PUBKEY_LEN];
    uint8_t tamper_priv[PK_PROTO_PRIVKEY_LEN], tamper_pub[PK_PROTO_PUBKEY_LEN];

    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x71, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);
    load_candidate_key(0x72, priv1, pub1);
    pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE);

    make_synthetic_keypair(0x99, tamper_priv, tamper_pub);
    stub_write_kslot(0, tamper_priv, tamper_pub);
    if (tamper_priv_out) {
        memcpy(tamper_priv_out, tamper_priv, PK_PROTO_PRIVKEY_LEN);
    }

    simulate_cold_boot();
}

static void test_n13_4d_and_5ab_tampered_key_rebind_closed(void)
{
    uint8_t tamper_priv[PK_PROTO_PRIVKEY_LEN];
    pk_proto_key_slot_t *ks0;
    const pk_vehicle_rec_t *r0;
    bool priv_is_tamper;

    /* 1. mismatch_boot (control) */
    setup_tampered_slot0_boot(tamper_priv);
    check(g_app.store.physical_nvs_attached &&
          pk_store_slot_key_corrupted(&g_app.store, 0) &&
          !pk_store_slot_key_ready(&g_app.store, 0) &&
          port_slot_keys(0) == NULL &&
          pk_store_slot_key_ready(&g_app.store, 1),
          "N13.4d [mismatch_boot]: tampered kslot_0 rejected on cold boot (corrupted=1 ready=0 attached=1)");

    /* 2. mismatch_switch: switch car on same boot must NOT republish tampered key */
    check(pk_store_switch_current(&g_app.store, 1),
          "N13.5a [mismatch_switch]: switch to slot 1");
    check(pk_store_switch_current(&g_app.store, 0),
          "N13.5a [mismatch_switch]: switch back to slot 0");
    r0 = pk_store_get(&g_app.store, 0);
    ks0 = port_slot_keys(0);
    priv_is_tamper = (ks0 != NULL && ks0->has_keypair &&
                      memcmp(ks0->privkey, tamper_priv, PK_PROTO_PRIVKEY_LEN) == 0);
    check(r0 != NULL && !r0->key_valid && r0->key_corrupted &&
          !pk_store_slot_key_ready(&g_app.store, 0) &&
          ks0 == NULL && !priv_is_tamper,
          "N13.5a [mismatch_switch]: slot 0 stays damaged (key_valid=0 corrupted=1 priv_is_tamper=0)");

    /* Also verify via pk_app_select_vehicle */
    check(pk_app_select_vehicle(&g_app, 1) && g_app.paired,
          "N13.5a [mismatch_switch]: pk_app_select_vehicle(1) paired=1");
    check(pk_app_select_vehicle(&g_app, 0) && !g_app.paired &&
          pk_store_slot_key_corrupted(&g_app.store, 0) && port_slot_keys(0) == NULL,
          "N13.5a [mismatch_switch]: pk_app_select_vehicle(0) keeps slot 0 unpaired/corrupted");

    /* 3. mismatch_prefs: save settings on same boot must NOT republish tampered key */
    setup_tampered_slot0_boot(tamper_priv);
    check(pk_store_save_prefs(&g_app.store, 60, PK_POWER_ENDURANCE),
          "N13.5b [mismatch_prefs]: pk_store_save_prefs succeeds");
    r0 = pk_store_get(&g_app.store, 0);
    ks0 = port_slot_keys(0);
    priv_is_tamper = (ks0 != NULL && ks0->has_keypair &&
                      memcmp(ks0->privkey, tamper_priv, PK_PROTO_PRIVKEY_LEN) == 0);
    check(r0 != NULL && !r0->key_valid && r0->key_corrupted &&
          !pk_store_slot_key_ready(&g_app.store, 0) &&
          ks0 == NULL && !priv_is_tamper,
          "N13.5b [mismatch_prefs]: slot 0 stays damaged after save_prefs (key_valid=0 corrupted=1 priv_is_tamper=0)");

    /* Also verify via pk_app_activate_setting(PK_SETTING_POWER) */
    check(pk_app_activate_setting(&g_app, PK_SETTING_POWER),
          "N13.5b [mismatch_prefs]: pk_app_activate_setting(POWER) succeeds");
    r0 = pk_store_get(&g_app.store, 0);
    ks0 = port_slot_keys(0);
    check(r0 != NULL && !r0->key_valid && r0->key_corrupted && ks0 == NULL,
          "N13.5b [mismatch_prefs]: slot 0 stays damaged after power toggle setting");
}

/* ==========================================================================
 * N13.5c: Damaged Slot 0 Must Not Steal New-Car Candidate Key (candidate_steal)
 * ========================================================================== */

static void test_n13_5c_candidate_steal_closed(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t cand_priv[PK_PROTO_PRIVKEY_LEN], cand_pub[PK_PROTO_PUBKEY_LEN];
    const pk_vehicle_rec_t *r0, *r1;
    pk_proto_key_slot_t *ks0, *ks1;
    test_kslot_rec_t disk0, disk1;
    bool s0_disk_has_cand, s1_disk_has_cand, same_priv;

    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0x81, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);

    /* Delete slot 0 key and cold boot -> slot 0 is damaged */
    stub_erase_kslot(0);
    simulate_cold_boot();
    check(pk_store_slot_key_corrupted(&g_app.store, 0) &&
          !pk_store_slot_key_ready(&g_app.store, 0),
          "N13.5c [candidate_steal]: pre-condition slot 0 corrupted after cold boot");

    /* Add second vehicle with a fresh candidate key */
    load_candidate_key(0x82, cand_priv, cand_pub);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE),
          "N13.5c [candidate_steal]: add car 1 succeeds");

    r0 = pk_store_get(&g_app.store, 0);
    r1 = pk_store_get(&g_app.store, 1);
    ks0 = port_slot_keys(0);
    ks1 = port_slot_keys(1);

    s0_disk_has_cand = stub_read_kslot(0, &disk0) &&
                       memcmp(disk0.privkey, cand_priv, PK_PROTO_PRIVKEY_LEN) == 0;
    s1_disk_has_cand = stub_read_kslot(1, &disk1) &&
                       memcmp(disk1.privkey, cand_priv, PK_PROTO_PRIVKEY_LEN) == 0;
    same_priv = (ks0 != NULL && ks1 != NULL &&
                 memcmp(ks0->privkey, ks1->privkey, PK_PROTO_PRIVKEY_LEN) == 0);

    check(r0 != NULL && !r0->key_valid && r0->key_corrupted &&
          ks0 == NULL && !stub_kslot_exists(0) && !s0_disk_has_cand && !same_priv,
          "N13.5c [candidate_steal]: slot 0 stays damaged and does not steal candidate (s0_valid=0 s0_corrupt=1 s0_priv_is_cand=0 same_priv=0)");
    check(r1 != NULL && r1->key_valid && !r1->key_corrupted &&
          ks1 != NULL && memcmp(ks1->privkey, cand_priv, PK_PROTO_PRIVKEY_LEN) == 0 &&
          s1_disk_has_cand,
          "N13.5c [candidate_steal]: only new slot 1 receives candidate private key (s1_valid=1 s1_priv_is_cand=1)");
}

/* ==========================================================================
 * N13.5d: Retry After Publish Failure Must Not Bind Old Candidate (leftover_retry)
 * ========================================================================== */

static void test_n13_5d_leftover_retry_closed(void)
{
    uint8_t priv0[PK_PROTO_PRIVKEY_LEN], pub0[PK_PROTO_PUBKEY_LEN];
    uint8_t old_cand_priv[PK_PROTO_PRIVKEY_LEN], old_cand_pub[PK_PROTO_PUBKEY_LEN];
    uint8_t new_cand_priv[PK_PROTO_PRIVKEY_LEN], new_cand_pub[PK_PROTO_PUBKEY_LEN];
    int set_calls;
    pk_proto_key_slot_t *ks1;
    test_kslot_rec_t disk1;
    bool priv_is_old, priv_is_new;

    /* Step 1: measure set_calls for adding car 1 */
    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0xA1, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);

    g_nvs_set_calls = 0;
    load_candidate_key(0xA2, old_cand_priv, old_cand_pub);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE),
          "N13.5d [leftover_retry]: measure set_calls for add car 1");
    set_calls = g_nvs_set_calls;
    check(set_calls >= 3, "N13.5d [leftover_retry]: set_calls >= 3 (id_cand, kslot_1, id_v1)");

    /* Step 2: rebuild with car 0, fail on the final nvs_set_blob (id_v1 publish) */
    nvs_stub_reset();
    simulate_cold_boot();
    load_candidate_key(0xA1, priv0, pub0);
    pk_store_add_verified(&g_app.store, SYN_VIN_A, PK_STORE_CRASH_NONE);
    pk_store_switch_current(&g_app.store, 0);

    g_nvs_set_calls = 0;
    g_nvs_fail_set_at = set_calls; /* fails id_v1 publish right after kslot_1 was written */
    load_candidate_key(0xA2, old_cand_priv, old_cand_pub);
    check(!pk_store_add_verified(&g_app.store, SYN_VIN_B, PK_STORE_CRASH_NONE),
          "N13.5d [leftover_retry]: first add fails at id_v1 publish (failed_first=1)");
    g_nvs_fail_set_at = 0;

    /* Intermediate assertion BEFORE retry: unreferenced kslot_1 MUST be withdrawn! */
    check(!stub_kslot_exists(1) &&
          pk_store_count(&g_app.store) == 1 &&
          pk_store_find_vin(&g_app.store, SYN_VIN_B) == PK_VEHICLE_NONE &&
          port_slot_keys(1) == NULL,
          "N13.5d [leftover_retry]: intermediate check - unreferenced kslot_1 withdrawn before retry and old id_v1 intact");

    /* Step 3: same-boot retry with a DIFFERENT candidate key */
    load_candidate_key(0xB3, new_cand_priv, new_cand_pub);
    check(pk_store_add_verified(&g_app.store, SYN_VIN_C, PK_STORE_CRASH_NONE),
          "N13.5d [leftover_retry]: same-boot retry with new candidate succeeds (retried=1)");

    ks1 = port_slot_keys(1);
    priv_is_old = (ks1 != NULL && memcmp(ks1->privkey, old_cand_priv, PK_PROTO_PRIVKEY_LEN) == 0);
    priv_is_new = (ks1 != NULL && memcmp(ks1->privkey, new_cand_priv, PK_PROTO_PRIVKEY_LEN) == 0);
    check(ks1 != NULL && ks1->has_keypair && !priv_is_old && priv_is_new &&
          stub_read_kslot(1, &disk1) &&
          memcmp(disk1.privkey, new_cand_priv, PK_PROTO_PRIVKEY_LEN) == 0,
          "N13.5d [leftover_retry]: retry binds ONLY new candidate (has_key=1 priv_is_old=0 priv_is_new=1)");
}

/* ==========================================================================
 * Entrypoint
 * ========================================================================== */

int main(void)
{
    printf("INFO|sizeof pk_store_image_t=%zu pk_vehicle_rec_t=%zu stub_blob_cap=%d\n",
           sizeof(pk_store_image_t), sizeof(pk_vehicle_rec_t), STUB_NVS_BLOB_CAP);

    test_n13_1_happy_path();
    test_n13_2a_unpublished_cand_ignored();
    test_n13_2b_fault_every_set();
    test_n13_3_delete_and_gc();
    test_n13_4a_legacy_v0_migration();
    test_n13_4bc_missing_key_boot_and_switch();
    test_n13_4d_and_5ab_tampered_key_rebind_closed();
    test_n13_5c_candidate_steal_closed();
    test_n13_5d_leftover_retry_closed();

    printf("SUMMARY: %d PASS, %d FAIL\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}

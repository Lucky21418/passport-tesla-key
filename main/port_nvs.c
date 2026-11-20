#include "port_internal.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define NVS_NAMESPACE       "tesla_key"
#define NVS_KEY_ID_V1       "id_v1"
#define NVS_KEY_ID_CAND     "id_cand"
#define NVS_KEY_LEGACY_LIVE "live"
#define NVS_KEY_LEGACY_JRN  "journal"

#define NVS_ID_V1_MAGIC     0x504B4931u /* "PKI1" */
#define NVS_ID_V1_VERSION   1u

static const char *TAG = "port_nvs";

/* Compact persistent keypair record without volatile runtime sessions */
typedef struct {
    bool    has_keypair;
    uint8_t privkey[PK_PROTO_PRIVKEY_LEN];
    uint8_t pubkey[PK_PROTO_PUBKEY_LEN];
} nvs_slot_key_rec_t;

/* Versioned single-record identity envelope persisted under "id_v1" / "id_cand" */
typedef struct {
    uint32_t         magic;
    uint16_t         version;
    uint16_t         reserved;
    uint32_t         crc32;
    uint32_t         generation;
    pk_store_image_t image;
} nvs_id_v1_blob_t;

/* Legacy v0 layout (pre-S4 "live" blob) for read-only migration without key rotation */
typedef struct {
    char vin[PK_VIN_BUF];
    char slot_id[PK_SLOT_ID_LEN];
    char key_ref[24];
    bool used;
} nvs_legacy_v0_vehicle_t;

typedef struct {
    uint32_t generation;
    int current;
    int mru[PK_MAX_VEHICLES];
    unsigned mru_count;
    nvs_legacy_v0_vehicle_t vehicles[PK_MAX_VEHICLES];
    unsigned vehicle_count;
    int brightness;
    pk_power_profile_t power;
} nvs_legacy_v0_image_t;

/* Per-slot key material (0..2 saved cars + index 3 for PK_VEHICLE_PAIR_CANDIDATE) */
static pk_proto_key_slot_t s_keys[PK_MAX_VEHICLES + 1];

/* In-flight transaction staging (cleared on commit, rollback, or recovery) */
typedef struct {
    bool                prepared;
    bool                has_new_slot;
    int                 new_slot;
    pk_proto_key_slot_t new_key;
} nvs_tx_state_t;

static nvs_tx_state_t s_tx;

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

static uint32_t id_blob_crc(const nvs_id_v1_blob_t *blob)
{
    const uint8_t *start = (const uint8_t *)&blob->generation;
    size_t len = sizeof(*blob) - offsetof(nvs_id_v1_blob_t, generation);
    return crc32_ieee(start, len);
}

static bool id_blob_valid(const nvs_id_v1_blob_t *blob)
{
    if (!blob) {
        return false;
    }
    if (blob->magic != NVS_ID_V1_MAGIC || blob->version != NVS_ID_V1_VERSION) {
        return false;
    }
    if (blob->generation == 0 || blob->generation != blob->image.generation) {
        return false;
    }
    return blob->crc32 == id_blob_crc(blob);
}

static void key_slot_zeroize(pk_proto_key_slot_t *ks)
{
    if (!ks) {
        return;
    }
    pk_proto_zeroize(ks, sizeof(*ks));
}

static void tx_state_clear(void)
{
    pk_proto_zeroize(&s_tx, sizeof(s_tx));
    s_tx.new_slot = PK_VEHICLE_NONE;
}

static bool key_pubkey_valid(const uint8_t pubkey[PK_PROTO_PUBKEY_LEN])
{
    return pubkey && pubkey[0] == 0x04u;
}

static bool key_fingerprint(const uint8_t pubkey[PK_PROTO_PUBKEY_LEN],
                            uint8_t fp_out[PK_KEY_FP_LEN])
{
    uint8_t digest[32];
    if (!key_pubkey_valid(pubkey) || !fp_out) {
        return false;
    }
    if (!pk_proto_sha256(pubkey, PK_PROTO_PUBKEY_LEN, digest)) {
        pk_proto_zeroize(digest, sizeof(digest));
        return false;
    }
    memcpy(fp_out, digest, PK_KEY_FP_LEN);
    pk_proto_zeroize(digest, sizeof(digest));
    return true;
}

static void format_kslot_name(int slot, char out[16])
{
    snprintf(out, 16, "kslot_%d", slot);
}

static bool nvs_read_slot_key_raw(nvs_handle_t h, int slot, nvs_slot_key_rec_t *out_rec)
{
    char key_name[16];
    nvs_slot_key_rec_t rec;
    size_t sz = sizeof(rec);
    esp_err_t err;

    if (slot < 0 || slot >= PK_MAX_VEHICLES) {
        return false;
    }
    format_kslot_name(slot, key_name);
    memset(&rec, 0, sizeof(rec));
    err = nvs_get_blob(h, key_name, &rec, &sz);
    if (err != ESP_OK || sz != sizeof(rec) || !rec.has_keypair || !key_pubkey_valid(rec.pubkey)) {
        pk_proto_zeroize(&rec, sizeof(rec));
        return false;
    }
    if (out_rec) {
        *out_rec = rec;
    }
    pk_proto_zeroize(&rec, sizeof(rec));
    return true;
}

static bool nvs_read_slot_key_verified(nvs_handle_t h, int slot,
                                       const uint8_t expected_fp[PK_KEY_FP_LEN],
                                       nvs_slot_key_rec_t *out_rec)
{
    nvs_slot_key_rec_t rec;
    uint8_t actual_fp[PK_KEY_FP_LEN];

    if (!expected_fp || !nvs_read_slot_key_raw(h, slot, &rec)) {
        return false;
    }
    if (!key_fingerprint(rec.pubkey, actual_fp) ||
        !pk_proto_ct_equal(actual_fp, expected_fp, PK_KEY_FP_LEN)) {
        pk_proto_zeroize(&rec, sizeof(rec));
        pk_proto_zeroize(actual_fp, sizeof(actual_fp));
        return false;
    }
    if (out_rec) {
        *out_rec = rec;
    }
    pk_proto_zeroize(&rec, sizeof(rec));
    pk_proto_zeroize(actual_fp, sizeof(actual_fp));
    return true;
}

static bool nvs_write_slot_key_verified(nvs_handle_t h, int slot, const pk_proto_key_slot_t *ks)
{
    char key_name[16];
    nvs_slot_key_rec_t rec;
    nvs_slot_key_rec_t rb;
    size_t rb_sz = sizeof(rb);
    esp_err_t err;

    if (slot < 0 || slot >= PK_MAX_VEHICLES || !ks || !ks->has_keypair ||
        !key_pubkey_valid(ks->pubkey)) {
        return false;
    }
    format_kslot_name(slot, key_name);
    memset(&rec, 0, sizeof(rec));
    rec.has_keypair = true;
    memcpy(rec.privkey, ks->privkey, sizeof(rec.privkey));
    memcpy(rec.pubkey, ks->pubkey, sizeof(rec.pubkey));

    err = nvs_set_blob(h, key_name, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        memset(&rb, 0, sizeof(rb));
        err = nvs_get_blob(h, key_name, &rb, &rb_sz);
        if (err != ESP_OK || rb_sz != sizeof(rec) || memcmp(&rb, &rec, sizeof(rec)) != 0) {
            err = ESP_FAIL;
        }
        pk_proto_zeroize(&rb, sizeof(rb));
    }
    pk_proto_zeroize(&rec, sizeof(rec));
    return err == ESP_OK;
}

static void nvs_erase_key_ignore_missing(nvs_handle_t h, const char *key_name)
{
    esp_err_t err = nvs_erase_key(h, key_name);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "erase %s failed: %s", key_name, esp_err_to_name(err));
    }
}

static void nvs_erase_slot_key(nvs_handle_t h, int slot)
{
    char key_name[16];
    if (slot < 0 || slot >= PK_MAX_VEHICLES) {
        return;
    }
    format_kslot_name(slot, key_name);
    nvs_erase_key_ignore_missing(h, key_name);
}

typedef enum {
    NVS_WRITE_OK = 0,
    NVS_WRITE_ERR_SET,
    NVS_WRITE_ERR_READBACK
} nvs_write_status_t;

static nvs_write_status_t nvs_write_blob_verified(nvs_handle_t h, const char *key_name,
                                                  const nvs_id_v1_blob_t *blob)
{
    nvs_id_v1_blob_t rb;
    size_t rb_sz = sizeof(rb);
    esp_err_t err;

    err = nvs_set_blob(h, key_name, blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        return NVS_WRITE_ERR_SET;
    }
    memset(&rb, 0, sizeof(rb));
    err = nvs_get_blob(h, key_name, &rb, &rb_sz);
    if (err == ESP_OK && rb_sz == sizeof(*blob) && memcmp(&rb, blob, sizeof(*blob)) == 0) {
        return NVS_WRITE_OK;
    }
    return NVS_WRITE_ERR_READBACK;
}

static bool nvs_load_id_v1(nvs_handle_t h, nvs_id_v1_blob_t *out_blob)
{
    nvs_id_v1_blob_t tmp;
    size_t sz = sizeof(tmp);
    esp_err_t err;

    memset(&tmp, 0, sizeof(tmp));
    err = nvs_get_blob(h, NVS_KEY_ID_V1, &tmp, &sz);
    if (err != ESP_OK || sz != sizeof(tmp) || !id_blob_valid(&tmp)) {
        return false;
    }
    if (out_blob) {
        *out_blob = tmp;
    }
    return true;
}

/* Revert id_v1 to its pre-transaction state after a publish or readback failure.
 * Returns true only when on-disk id_v1 is verified to match the pre-transaction
 * state (previous valid blob, or absent if no previous id_v1 existed). */
static bool nvs_rollback_id_v1(nvs_handle_t h, bool has_prev,
                               const nvs_id_v1_blob_t *prev_blob)
{
    if (has_prev) {
        if (!prev_blob) {
            return false;
        }
        {
            nvs_id_v1_blob_t cur;
            if (nvs_load_id_v1(h, &cur) &&
                memcmp(&cur, prev_blob, sizeof(cur)) == 0) {
                return true;
            }
        }
        return nvs_write_blob_verified(h, NVS_KEY_ID_V1, prev_blob) == NVS_WRITE_OK;
    } else {
        size_t probe_sz = 0;
        if (nvs_get_blob(h, NVS_KEY_ID_V1, NULL, &probe_sz) == ESP_ERR_NVS_NOT_FOUND) {
            return true;
        }
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_V1);
        if (nvs_commit(h) != ESP_OK) {
            return false;
        }
        return nvs_get_blob(h, NVS_KEY_ID_V1, NULL, &probe_sz) == ESP_ERR_NVS_NOT_FOUND;
    }
}

static void image_sanitize(pk_store_image_t *img)
{
    int i;
    int tmp[PK_MAX_VEHICLES];
    unsigned mru_n = 0;
    unsigned veh_n = 0;

    if (!img) {
        return;
    }
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        img->vehicles[i].vin[PK_VIN_LEN] = '\0';
        img->vehicles[i].slot_id[PK_SLOT_ID_LEN - 1] = '\0';
        img->vehicles[i].key_ref[sizeof(img->vehicles[i].key_ref) - 1] = '\0';
        if (img->vehicles[i].used) {
            veh_n++;
        }
    }
    if (img->mru_count > PK_MAX_VEHICLES) {
        img->mru_count = PK_MAX_VEHICLES;
    }
    for (i = 0; i < (int)img->mru_count && mru_n < PK_MAX_VEHICLES; i++) {
        int s = img->mru[i];
        if (s >= 0 && s < PK_MAX_VEHICLES && img->vehicles[s].used) {
            tmp[mru_n++] = s;
        }
    }
    memcpy(img->mru, tmp, mru_n * sizeof(tmp[0]));
    img->mru_count = mru_n;
    img->vehicle_count = veh_n;
    if (img->current < 0 || img->current >= PK_MAX_VEHICLES ||
        !img->vehicles[img->current].used) {
        img->current = img->mru_count ? img->mru[0] : PK_VEHICLE_NONE;
    }
    if (img->brightness < PK_BRIGHTNESS_MIN || img->brightness > PK_BRIGHTNESS_MAX) {
        img->brightness = PK_BRIGHTNESS_DEFAULT;
    }
    if (img->power != PK_POWER_COMFORT && img->power != PK_POWER_ENDURANCE) {
        img->power = PK_POWER_COMFORT;
    }
}

/* ==========================================================================
 * 2. NVS Persistence Port (pk_nvs_port_t) & Key Storage
 * ========================================================================== */

bool port_nvs_available(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_close(h);
    return true;
}

static bool nvs_save_slot_key(int slot, const pk_proto_key_slot_t *ks)
{
    nvs_handle_t h;
    esp_err_t err;
    bool ok = false;

    if (slot < 0 || slot >= PK_MAX_VEHICLES || !ks) {
        return false;
    }
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    if (ks->has_keypair) {
        ok = nvs_write_slot_key_verified(h, slot, ks);
    } else {
        nvs_erase_slot_key(h, slot);
        ok = (nvs_commit(h) == ESP_OK);
    }
    nvs_close(h);
    return ok;
}

void port_nvs_load_slot_keys(void)
{
    /* Clear volatile RAM slot keys; nvs_port_recover() hydrates only keys whose
     * public-key fingerprints match the published id_v1 record. */
    int i;
    tx_state_clear();
    for (i = 0; i <= PK_MAX_VEHICLES; i++) {
        key_slot_zeroize(&s_keys[i]);
    }
}

bool port_ensure_slot_keypair(int slot)
{
    uint8_t seed[32];

    if (slot < 0 || slot > PK_MAX_VEHICLES) {
        return false;
    }
    if (s_keys[slot].has_keypair && key_pubkey_valid(s_keys[slot].pubkey)) {
        return true;
    }
    if (!pk_proto_random_bytes(seed, sizeof(seed))) {
        ESP_LOGW(TAG, "slot %d keygen blocked: RF entropy not ready", slot);
        pk_proto_zeroize(seed, sizeof(seed));
        key_slot_zeroize(&s_keys[slot]);
        return false;
    }
    if (pk_proto_p256_keygen(s_keys[slot].privkey, s_keys[slot].pubkey, seed)) {
        s_keys[slot].has_keypair = true;
        if (slot < PK_MAX_VEHICLES) {
            (void)nvs_save_slot_key(slot, &s_keys[slot]);
        }
        pk_proto_zeroize(seed, sizeof(seed));
        return true;
    }
    ESP_LOGW(TAG, "slot %d keygen failed; fail-closed", slot);
    pk_proto_zeroize(seed, sizeof(seed));
    key_slot_zeroize(&s_keys[slot]);
    return false;
}

static pk_err_t nvs_port_prepare(void *ctx, const void *image, size_t len)
{
    nvs_handle_t h;
    nvs_id_v1_blob_t pub_blob;
    bool has_pub;
    nvs_id_v1_blob_t cand_blob;
    pk_store_image_t *img;
    int i;
    int new_slot = PK_VEHICLE_NONE;
    int new_slot_count = 0;

    (void)ctx;
    if (!image || len != sizeof(pk_store_image_t)) {
        return PK_ERR_INVALID;
    }
    tx_state_clear();

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return PK_ERR_NOT_WIRED;
    }

    has_pub = nvs_load_id_v1(h, &pub_blob);
    memset(&cand_blob, 0, sizeof(cand_blob));
    memcpy(&cand_blob.image, image, sizeof(cand_blob.image));
    img = &cand_blob.image;
    image_sanitize(img);
    if (img->generation == 0) {
        img->generation = has_pub ? (pub_blob.generation + 1u) : 1u;
    } else if (has_pub && img->generation <= pub_blob.generation) {
        /* Reject stale RAM snapshots (e.g. after a commit whose readback and
         * rollback both failed, leaving a newer consistent id_v1 on disk) so
         * a same-boot retry cannot void or overwrite the published record. */
        nvs_close(h);
        return PK_ERR_INVALID;
    }

    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        bool was_used = has_pub && pub_blob.image.vehicles[i].used;
        bool will_use = img->vehicles[i].used;

        if (!will_use) {
            img->vehicles[i].key_valid = false;
            img->vehicles[i].key_corrupted = false;
            memset(img->vehicles[i].key_fp, 0, sizeof(img->vehicles[i].key_fp));
            if (was_used) {
                img->tombstones |= (uint8_t)(1u << i);
            }
            continue;
        }

        if (was_used) {
            /* Existing vehicle slot: VIN must match and key_fp is immutable.
             * Rule 1: A damaged slot MUST stay damaged; never adopt another
             * key from disk or from PK_VEHICLE_PAIR_CANDIDATE. */
            if (strcmp(pub_blob.image.vehicles[i].vin, img->vehicles[i].vin) != 0) {
                nvs_close(h);
                return PK_ERR_INVALID;
            }
            memcpy(img->vehicles[i].key_fp, pub_blob.image.vehicles[i].key_fp, PK_KEY_FP_LEN);
            if (!img->vehicles[i].key_valid && !img->vehicles[i].key_corrupted &&
                pub_blob.image.vehicles[i].key_valid &&
                !pub_blob.image.vehicles[i].key_corrupted) {
                /* Do not silently convert an unvalidated new-slot candidate
                 * into a corrupted marker over a still-valid published slot. */
                nvs_close(h);
                return PK_ERR_INVALID;
            }
            if (pub_blob.image.vehicles[i].key_corrupted ||
                !pub_blob.image.vehicles[i].key_valid ||
                img->vehicles[i].key_corrupted ||
                !img->vehicles[i].key_valid) {
                img->vehicles[i].key_valid = false;
                img->vehicles[i].key_corrupted = true;
                key_slot_zeroize(&s_keys[i]);
            } else if (!nvs_read_slot_key_verified(h, i, pub_blob.image.vehicles[i].key_fp, NULL)) {
                img->vehicles[i].key_valid = false;
                img->vehicles[i].key_corrupted = true;
                key_slot_zeroize(&s_keys[i]);
            } else {
                img->vehicles[i].key_valid = true;
                img->vehicles[i].key_corrupted = false;
            }
            continue;
        }

        /* Genuinely new slot:
         * Rule 2: Accepts ONLY this transaction's candidate keypair from
         * PK_VEHICLE_PAIR_CANDIDATE. Never adopts any leftover kslot_i on disk. */
        if (new_slot_count > 0) {
            nvs_close(h);
            return PK_ERR_INVALID;
        }
        if (!s_keys[PK_VEHICLE_PAIR_CANDIDATE].has_keypair ||
            !key_pubkey_valid(s_keys[PK_VEHICLE_PAIR_CANDIDATE].pubkey)) {
            nvs_close(h);
            return PK_ERR_INVALID;
        }
        if (!key_fingerprint(s_keys[PK_VEHICLE_PAIR_CANDIDATE].pubkey, img->vehicles[i].key_fp)) {
            nvs_close(h);
            return PK_ERR_INVALID;
        }
        img->vehicles[i].key_valid = true;
        img->vehicles[i].key_corrupted = false;
        img->tombstones &= (uint8_t)~(1u << i);
        new_slot = i;
        new_slot_count++;
    }

    cand_blob.magic = NVS_ID_V1_MAGIC;
    cand_blob.version = NVS_ID_V1_VERSION;
    cand_blob.reserved = 0;
    cand_blob.generation = img->generation;
    cand_blob.crc32 = id_blob_crc(&cand_blob);

    if (nvs_write_blob_verified(h, NVS_KEY_ID_CAND, &cand_blob) != NVS_WRITE_OK) {
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
        (void)nvs_commit(h);
        nvs_close(h);
        return PK_ERR_NOT_WIRED;
    }

    nvs_close(h);
    s_tx.prepared = true;
    s_tx.has_new_slot = (new_slot != PK_VEHICLE_NONE);
    s_tx.new_slot = new_slot;
    if (s_tx.has_new_slot) {
        s_tx.new_key = s_keys[PK_VEHICLE_PAIR_CANDIDATE];
    }
    return PK_OK;
}

static pk_err_t nvs_port_commit(void *ctx)
{
    nvs_handle_t h;
    nvs_id_v1_blob_t prev_blob;
    bool has_prev = false;
    nvs_id_v1_blob_t cand_blob;
    size_t sz = sizeof(cand_blob);
    esp_err_t err;
    int i;
    bool wrote_new_slot_key = false;
    int new_slot = s_tx.has_new_slot ? s_tx.new_slot : PK_VEHICLE_NONE;

    (void)ctx;
    if (!s_tx.prepared) {
        return PK_ERR_INVALID;
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        tx_state_clear();
        return PK_ERR_NOT_WIRED;
    }

    memset(&cand_blob, 0, sizeof(cand_blob));
    err = nvs_get_blob(h, NVS_KEY_ID_CAND, &cand_blob, &sz);
    if (err != ESP_OK || sz != sizeof(cand_blob) || !id_blob_valid(&cand_blob)) {
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
        (void)nvs_commit(h);
        nvs_close(h);
        tx_state_clear();
        return PK_ERR_INVALID;
    }

    /* Snapshot the currently published id_v1 before mutating it so that a
     * post-write readback mismatch can roll id_v1 back before withdrawing
     * any new-slot key. */
    memset(&prev_blob, 0, sizeof(prev_blob));
    has_prev = nvs_load_id_v1(h, &prev_blob);

    /* Step 1: If this transaction adds a new slot, persist its candidate key
     * under kslot_<new_slot> before publishing id_v1. */
    if (new_slot >= 0 && new_slot < PK_MAX_VEHICLES) {
        uint8_t fp[PK_KEY_FP_LEN];
        if (!cand_blob.image.vehicles[new_slot].used ||
            !s_tx.new_key.has_keypair ||
            !key_fingerprint(s_tx.new_key.pubkey, fp) ||
            !pk_proto_ct_equal(fp, cand_blob.image.vehicles[new_slot].key_fp, PK_KEY_FP_LEN)) {
            pk_proto_zeroize(fp, sizeof(fp));
            nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
            (void)nvs_commit(h);
            nvs_close(h);
            tx_state_clear();
            return PK_ERR_INVALID;
        }
        pk_proto_zeroize(fp, sizeof(fp));
        if (!nvs_write_slot_key_verified(h, new_slot, &s_tx.new_key)) {
            /* Withdraw any partial new-slot key and abort cleanly. */
            nvs_erase_slot_key(h, new_slot);
            nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
            (void)nvs_commit(h);
            nvs_close(h);
            key_slot_zeroize(&s_keys[new_slot]);
            tx_state_clear();
            return PK_ERR_NOT_WIRED;
        }
        wrote_new_slot_key = true;
    }

    /* Step 2: Publish single versioned identity record id_v1 + readback verify. */
    if (nvs_write_blob_verified(h, NVS_KEY_ID_V1, &cand_blob) != NVS_WRITE_OK) {
        /* Roll back id_v1 to its pre-transaction state before withdrawing the
         * new-slot key. Never erase kslot_<new_slot> while on-disk id_v1 may
         * still mark that slot key_valid=1. */
        bool id_v1_reverted = nvs_rollback_id_v1(h, has_prev, &prev_blob);
        if (wrote_new_slot_key) {
            if (id_v1_reverted) {
                nvs_erase_slot_key(h, new_slot);
            } else {
                ESP_LOGW(TAG, "id_v1 rollback failed; preserving kslot_%d for consistency",
                         new_slot);
            }
            key_slot_zeroize(&s_keys[new_slot]);
        }
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
        (void)nvs_commit(h);
        nvs_close(h);
        tx_state_clear();
        return PK_ERR_NOT_WIRED;
    }

    /* Step 3: Published! Activate new slot key in RAM, remove candidate staging,
     * and garbage-collect unreferenced/tombstoned slot keys. */
    if (wrote_new_slot_key) {
        s_keys[new_slot] = s_tx.new_key;
        key_slot_zeroize(&s_keys[PK_VEHICLE_PAIR_CANDIDATE]);
    }
    nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);

    {
        bool any_used = false;
        for (i = 0; i < PK_MAX_VEHICLES; i++) {
            if (cand_blob.image.vehicles[i].used) {
                any_used = true;
                if (!cand_blob.image.vehicles[i].key_valid ||
                    cand_blob.image.vehicles[i].key_corrupted) {
                    key_slot_zeroize(&s_keys[i]);
                }
            } else {
                key_slot_zeroize(&s_keys[i]);
                nvs_erase_slot_key(h, i);
            }
        }
        if (!any_used) {
            key_slot_zeroize(&s_keys[PK_VEHICLE_PAIR_CANDIDATE]);
        }
    }
    (void)nvs_commit(h);
    nvs_close(h);
    tx_state_clear();
    return PK_OK;
}

static bool migrate_legacy_v0_to_v1(nvs_handle_t h, nvs_id_v1_blob_t *out_blob)
{
    nvs_legacy_v0_image_t v0;
    size_t sz = sizeof(v0);
    nvs_id_v1_blob_t v1;
    int i;

    memset(&v0, 0, sizeof(v0));
    if (nvs_get_blob(h, NVS_KEY_LEGACY_LIVE, &v0, &sz) != ESP_OK || sz != sizeof(v0)) {
        return false;
    }

    memset(&v1, 0, sizeof(v1));
    v1.magic = NVS_ID_V1_MAGIC;
    v1.version = NVS_ID_V1_VERSION;
    v1.reserved = 0;
    v1.image.generation = (v0.generation > 0) ? v0.generation : 1u;
    v1.image.current = v0.current;
    v1.image.mru_count = v0.mru_count;
    memcpy(v1.image.mru, v0.mru, sizeof(v1.image.mru));
    v1.image.brightness = v0.brightness;
    v1.image.power = v0.power;
    v1.image.tombstones = 0;

    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (!v0.vehicles[i].used) {
            continue;
        }
        v1.image.vehicles[i].used = true;
        memcpy(v1.image.vehicles[i].vin, v0.vehicles[i].vin, sizeof(v1.image.vehicles[i].vin));
        memcpy(v1.image.vehicles[i].slot_id, v0.vehicles[i].slot_id, sizeof(v1.image.vehicles[i].slot_id));
        memcpy(v1.image.vehicles[i].key_ref, v0.vehicles[i].key_ref, sizeof(v1.image.vehicles[i].key_ref));

        /* Preserve existing paired private key without rotation; if missing,
         * mark damaged and never silently keygen. */
        {
            nvs_slot_key_rec_t rec;
            if (nvs_read_slot_key_raw(h, i, &rec) &&
                key_fingerprint(rec.pubkey, v1.image.vehicles[i].key_fp)) {
                v1.image.vehicles[i].key_valid = true;
                v1.image.vehicles[i].key_corrupted = false;
            } else {
                v1.image.vehicles[i].key_valid = false;
                v1.image.vehicles[i].key_corrupted = true;
            }
            pk_proto_zeroize(&rec, sizeof(rec));
        }
    }

    image_sanitize(&v1.image);
    v1.generation = v1.image.generation;
    v1.crc32 = id_blob_crc(&v1);

    /* Keep legacy "live" intact if writing or verifying id_v1 fails, and
     * remove any unverified id_v1 blob written during this failed migration. */
    if (nvs_write_blob_verified(h, NVS_KEY_ID_V1, &v1) != NVS_WRITE_OK) {
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_V1);
        (void)nvs_commit(h);
        return false;
    }

    /* Migration succeeded: remove legacy "live" so old firmware cannot read a
     * diverging vehicle list. */
    nvs_erase_key_ignore_missing(h, NVS_KEY_LEGACY_LIVE);
    (void)nvs_commit(h);
    *out_blob = v1;
    return true;
}

static pk_err_t nvs_port_recover(void *ctx, void *image, size_t len)
{
    nvs_handle_t h;
    nvs_id_v1_blob_t blob;
    bool have_id = false;
    bool dirty_gc = false;
    size_t probe_sz = 0;
    int i;

    (void)ctx;
    if (!image || len != sizeof(pk_store_image_t)) {
        return PK_ERR_INVALID;
    }
    tx_state_clear();

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return PK_ERR_NOT_WIRED;
    }

    /* Discard any unpublished candidate or legacy journal; never adopt id_cand
     * merely because its generation is higher than id_v1. */
    if (nvs_get_blob(h, NVS_KEY_ID_CAND, NULL, &probe_sz) == ESP_OK) {
        nvs_erase_key_ignore_missing(h, NVS_KEY_ID_CAND);
        dirty_gc = true;
    }
    if (nvs_get_blob(h, NVS_KEY_LEGACY_JRN, NULL, &probe_sz) == ESP_OK) {
        nvs_erase_key_ignore_missing(h, NVS_KEY_LEGACY_JRN);
        dirty_gc = true;
    }

    have_id = nvs_load_id_v1(h, &blob);
    if (!have_id) {
        have_id = migrate_legacy_v0_to_v1(h, &blob);
    }

    if (!have_id) {
        if (dirty_gc) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
        return PK_ERR_NOT_WIRED;
    }

    image_sanitize(&blob.image);

    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        char key_name[16];
        format_kslot_name(i, key_name);
        if (!blob.image.vehicles[i].used) {
            key_slot_zeroize(&s_keys[i]);
            blob.image.vehicles[i].key_valid = false;
            blob.image.vehicles[i].key_corrupted = false;
            memset(blob.image.vehicles[i].key_fp, 0, sizeof(blob.image.vehicles[i].key_fp));
            if (nvs_get_blob(h, key_name, NULL, &probe_sz) == ESP_OK) {
                nvs_erase_slot_key(h, i);
                dirty_gc = true;
            }
            continue;
        }

        if (blob.image.vehicles[i].key_corrupted || !blob.image.vehicles[i].key_valid) {
            /* Damaged slot remains damaged; never load or regenerate key. */
            blob.image.vehicles[i].key_valid = false;
            blob.image.vehicles[i].key_corrupted = true;
            key_slot_zeroize(&s_keys[i]);
            continue;
        }

        {
            nvs_slot_key_rec_t rec;
            if (nvs_read_slot_key_verified(h, i, blob.image.vehicles[i].key_fp, &rec)) {
                key_slot_zeroize(&s_keys[i]);
                s_keys[i].has_keypair = true;
                memcpy(s_keys[i].privkey, rec.privkey, sizeof(rec.privkey));
                memcpy(s_keys[i].pubkey, rec.pubkey, sizeof(rec.pubkey));
                blob.image.vehicles[i].key_valid = true;
                blob.image.vehicles[i].key_corrupted = false;
            } else {
                /* Missing or fingerprint-mismatched slot key: fail closed,
                 * mark damaged, never silently keygen. */
                key_slot_zeroize(&s_keys[i]);
                blob.image.vehicles[i].key_valid = false;
                blob.image.vehicles[i].key_corrupted = true;
            }
            pk_proto_zeroize(&rec, sizeof(rec));
        }
    }

    if (dirty_gc) {
        (void)nvs_commit(h);
    }
    nvs_close(h);

    memcpy(image, &blob.image, len);
    return PK_OK;
}

static const pk_nvs_port_t k_nvs_port = {
    .prepare = nvs_port_prepare,
    .commit = nvs_port_commit,
    .recover = nvs_port_recover,
};

const pk_nvs_port_t *port_nvs_ops(void)
{
    return &k_nvs_port;
}

const char *port_slot_vin(int slot)
{
    const pk_app_t *app = port_bound_app();
    const pk_vehicle_rec_t *rec;
    if (!app) {
        return NULL;
    }
    if (slot == PK_VEHICLE_PAIR_CANDIDATE) {
        return app->has_vin ? app->vin : NULL;
    }
    rec = pk_store_get(&app->store, slot);
    return rec ? rec->vin : NULL;
}

pk_proto_key_slot_t *port_slot_keys(int slot)
{
    if (slot == PK_VEHICLE_PAIR_CANDIDATE) {
        /* S2 §2E: Candidate keypair is generated explicitly in
         * port_ble_start_candidate_pairing(); getter must not silently
         * generate a keypair behind AddKey's missing-key guard. */
        return &s_keys[PK_VEHICLE_PAIR_CANDIDATE];
    }
    if (slot >= 0 && slot < PK_MAX_VEHICLES) {
        const pk_app_t *app = port_bound_app();
        if (app && app->store.physical_nvs_attached) {
            /* S4: Never silently keygen for a saved car when physical NVS is
             * attached. Missing or corrupted slot keys must fail closed. */
            if (!pk_store_slot_key_ready(&app->store, slot)) {
                return NULL;
            }
            return s_keys[slot].has_keypair ? &s_keys[slot] : NULL;
        }
        port_ensure_slot_keypair(slot);
        return &s_keys[slot];
    }
    return NULL;
}

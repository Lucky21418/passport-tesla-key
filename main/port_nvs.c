#include "port_internal.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define NVS_NAMESPACE "tesla_key"
static const char *TAG = "port_nvs";

/* Compact persistent keypair record without volatile runtime sessions */
typedef struct {
    bool    has_keypair;
    uint8_t privkey[PK_PROTO_PRIVKEY_LEN];
    uint8_t pubkey[PK_PROTO_PUBKEY_LEN];
} nvs_slot_key_rec_t;

/* Per-slot key material (0..2 saved cars + index 3 for PK_VEHICLE_PAIR_CANDIDATE) */
static pk_proto_key_slot_t s_keys[PK_MAX_VEHICLES + 1];

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
    char key_name[16];
    esp_err_t err;

    if (slot < 0 || slot >= PK_MAX_VEHICLES || !ks) {
        return false;
    }
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    snprintf(key_name, sizeof(key_name), "kslot_%d", slot);
    if (ks->has_keypair) {
        nvs_slot_key_rec_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.has_keypair = true;
        memcpy(rec.privkey, ks->privkey, sizeof(rec.privkey));
        memcpy(rec.pubkey, ks->pubkey, sizeof(rec.pubkey));
        err = nvs_set_blob(h, key_name, &rec, sizeof(rec));
        memset(&rec, 0, sizeof(rec));
    } else {
        err = nvs_erase_key(h, key_name);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key_name, esp_err_to_name(err));
        return false;
    }
    return true;
}

void port_nvs_load_slot_keys(void)
{
    nvs_handle_t h;
    int i;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        char key_name[16];
        nvs_slot_key_rec_t rec;
        size_t sz = sizeof(rec);
        snprintf(key_name, sizeof(key_name), "kslot_%d", i);
        memset(&s_keys[i], 0, sizeof(s_keys[i]));
        if (nvs_get_blob(h, key_name, &rec, &sz) == ESP_OK && sz == sizeof(rec) && rec.has_keypair) {
            s_keys[i].has_keypair = true;
            memcpy(s_keys[i].privkey, rec.privkey, sizeof(rec.privkey));
            memcpy(s_keys[i].pubkey, rec.pubkey, sizeof(rec.pubkey));
        }
        memset(&rec, 0, sizeof(rec));
    }
    nvs_close(h);
}

bool port_ensure_slot_keypair(int slot)
{
    uint8_t seed[32];

    if (slot < 0 || slot > PK_MAX_VEHICLES) {
        return false;
    }
    if (s_keys[slot].has_keypair && s_keys[slot].pubkey[0] == 0x04u) {
        return true;
    }
    if (!pk_proto_random_bytes(seed, sizeof(seed))) {
        ESP_LOGW(TAG, "slot %d keygen blocked: RF entropy not ready", slot);
        pk_proto_zeroize(seed, sizeof(seed));
        pk_proto_zeroize(s_keys[slot].privkey, sizeof(s_keys[slot].privkey));
        pk_proto_zeroize(s_keys[slot].pubkey, sizeof(s_keys[slot].pubkey));
        s_keys[slot].has_keypair = false;
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
    pk_proto_zeroize(s_keys[slot].privkey, sizeof(s_keys[slot].privkey));
    pk_proto_zeroize(s_keys[slot].pubkey, sizeof(s_keys[slot].pubkey));
    s_keys[slot].has_keypair = false;
    return false;
}

static pk_err_t nvs_port_prepare(void *ctx, const void *image, size_t len)
{
    nvs_handle_t h;
    esp_err_t err;

    (void)ctx;
    if (!image || len != sizeof(pk_store_image_t)) {
        return PK_ERR_INVALID;
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return PK_ERR_NOT_WIRED;
    }
    err = nvs_set_blob(h, "journal", image, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return (err == ESP_OK) ? PK_OK : PK_ERR_NOT_WIRED;
}

static pk_err_t nvs_port_commit(void *ctx)
{
    nvs_handle_t h;
    pk_store_image_t img;
    size_t sz = sizeof(img);
    esp_err_t err;
    int i;

    (void)ctx;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return PK_ERR_NOT_WIRED;
    }
    err = nvs_get_blob(h, "journal", &img, &sz);
    if (err != ESP_OK || sz != sizeof(img)) {
        nvs_close(h);
        return PK_ERR_INVALID;
    }
    /* Persist new per-slot keypairs BEFORE overwriting "live" so a slot-key
     * write failure never commits an unkeyed vehicle record.
     */
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (img.vehicles[i].used && !s_keys[i].has_keypair &&
            s_keys[PK_VEHICLE_PAIR_CANDIDATE].has_keypair) {
            if (!nvs_save_slot_key(i, &s_keys[PK_VEHICLE_PAIR_CANDIDATE])) {
                nvs_close(h);
                return PK_ERR_NOT_WIRED;
            }
            s_keys[i] = s_keys[PK_VEHICLE_PAIR_CANDIDATE];
            memset(&s_keys[PK_VEHICLE_PAIR_CANDIDATE], 0, sizeof(s_keys[PK_VEHICLE_PAIR_CANDIDATE]));
        }
    }
    err = nvs_set_blob(h, "live", &img, sizeof(img));
    if (err == ESP_OK) {
        (void)nvs_erase_key(h, "journal");
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        bool any_used = false;
        for (i = 0; i < PK_MAX_VEHICLES; i++) {
            if (img.vehicles[i].used) {
                any_used = true;
            } else {
                memset(&s_keys[i], 0, sizeof(s_keys[i]));
                (void)nvs_save_slot_key(i, &s_keys[i]);
            }
        }
        if (!any_used) {
            memset(&s_keys[PK_VEHICLE_PAIR_CANDIDATE], 0, sizeof(s_keys[PK_VEHICLE_PAIR_CANDIDATE]));
        }
    }
    return (err == ESP_OK) ? PK_OK : PK_ERR_NOT_WIRED;
}

static pk_err_t nvs_port_recover(void *ctx, void *image, size_t len)
{
    nvs_handle_t h;
    size_t sz = len;
    size_t jsz = 0;
    esp_err_t err;

    (void)ctx;
    if (!image || len != sizeof(pk_store_image_t)) {
        return PK_ERR_INVALID;
    }
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return PK_ERR_NOT_WIRED;
    }
    if (nvs_get_blob(h, "journal", NULL, &jsz) == ESP_OK) {
        nvs_close(h);
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, "journal");
            (void)nvs_commit(h);
            nvs_close(h);
        }
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
            return PK_ERR_NOT_WIRED;
        }
    }
    err = nvs_get_blob(h, "live", image, &sz);
    nvs_close(h);
    return (err == ESP_OK && sz == len) ? PK_OK : PK_ERR_NOT_WIRED;
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
        port_ensure_slot_keypair(slot);
        return &s_keys[slot];
    }
    return NULL;
}

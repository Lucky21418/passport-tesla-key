#ifndef PK_STORE_H
#define PK_STORE_H

#include "pk_types.h"
#include "pk_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M10 / S4: versioned single-record identity store and NVS transaction contract.
 *
 * Persisted state uses a single versioned record ("id_v1") with candidate
 * staging ("id_cand"), per-slot key binding via SHA-256 public-key fingerprint,
 * tombstones, and unreferenced-key garbage collection.
 *
 * Host stubs are in-memory simulations, NOT proof of real ESP32-C3 flash
 * power-loss, page CRC, wear-level residue clearance, or cryptographic erase.
 * Do not report "data securely erased" or claim real-flash power-loss proof
 * from host tests.
 *
 * Transaction invariants:
 *  - crash or failure before id_v1 publish: keep old id_v1; withdraw any
 *    unreferenced new-slot key written during this transaction; never adopt an
 *    unpublished candidate merely because its generation is higher.
 *  - after id_v1 publish: new identity wins; tombstoned slots stay deleted and
 *    their unreferenced slot keys are collected.
 *  - damaged slots (missing or fingerprint-mismatched key) stay damaged across
 *    switch-car, save-settings, and add-vehicle transactions; a new slot
 *    accepts only this transaction's pairing candidate key.
 */

#define PK_KEY_FP_LEN 8

typedef struct {
    char vin[PK_VIN_BUF];
    char slot_id[PK_SLOT_ID_LEN];
    char key_ref[24];
    bool used;
    bool key_valid;
    bool key_corrupted;
    uint8_t key_fp[PK_KEY_FP_LEN];
} pk_vehicle_rec_t;

typedef struct {
    uint32_t generation;
    int current;                       /* PK_VEHICLE_NONE if 0 cars */
    int mru[PK_MAX_VEHICLES];
    unsigned mru_count;
    pk_vehicle_rec_t vehicles[PK_MAX_VEHICLES];
    unsigned vehicle_count;
    int brightness;
    pk_power_profile_t power;
    uint8_t tombstones;                /* bitmask of tombstoned slots pending GC */
} pk_store_image_t;

typedef enum {
    PK_STORE_CRASH_NONE = 0,
    PK_STORE_CRASH_PREPARE, /* inject: drop before commit, old image remains */
    PK_STORE_CRASH_COMMIT   /* inject: power loss after commit, new image wins */
} pk_store_crash_t;

typedef struct {
    pk_store_image_t live;
    pk_store_image_t journal;
    bool journal_valid;
    bool committed;
    bool physical_nvs_attached;
    const pk_nvs_port_t *nvs;
    void *nvs_ctx;
} pk_store_t;

void pk_store_init(pk_store_t *s);
void pk_store_reset_empty(pk_store_t *s);

int pk_store_count(const pk_store_t *s);
int pk_store_current(const pk_store_t *s);
bool pk_store_slot_used(const pk_store_t *s, int slot);
bool pk_store_slot_key_ready(const pk_store_t *s, int slot);
bool pk_store_slot_key_corrupted(const pk_store_t *s, int slot);
const pk_vehicle_rec_t *pk_store_get(const pk_store_t *s, int slot);
int pk_store_find_vin(const pk_store_t *s, const char *vin);

/* Success does not switch the current vehicle. Rejects duplicates and a 4th car. */
bool pk_store_add_verified(pk_store_t *s, const char *vin, pk_store_crash_t crash);

/* reset=true removes every car and local settings. Only affects this device;
 * it does not revoke vehicle-side enrollment.
 */
bool pk_store_delete(pk_store_t *s, int target, bool reset, pk_store_crash_t crash,
                     pk_store_image_t *out_live);

bool pk_store_switch_current(pk_store_t *s, int slot);
bool pk_store_save_prefs(pk_store_t *s, int brightness, pk_power_profile_t power);

/* After a simulated or real power loss: if journal was not committed, the
 * old live image is kept and the caller must not report "deleted".
 */
void pk_store_recover(pk_store_t *s, pk_store_image_t *out_live);

#ifdef __cplusplus
}
#endif

#endif /* PK_STORE_H */

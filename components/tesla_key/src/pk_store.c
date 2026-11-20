#include "pk_store.h"

#include <stdio.h>
#include <string.h>

static void image_clear(pk_store_image_t *img)
{
    memset(img, 0, sizeof(*img));
    img->generation = 1;
    img->current = PK_VEHICLE_NONE;
    img->mru_count = 0;
    img->vehicle_count = 0;
    img->brightness = PK_BRIGHTNESS_DEFAULT;
    img->power = PK_POWER_COMFORT;
}

static unsigned live_count(const pk_store_image_t *img)
{
    unsigned n = 0;
    int i;
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (img->vehicles[i].used) {
            n++;
        }
    }
    return n;
}

static void mru_rebuild(pk_store_image_t *img)
{
    int tmp[PK_MAX_VEHICLES];
    unsigned n = 0;
    unsigned i;
    for (i = 0; i < img->mru_count && n < PK_MAX_VEHICLES; i++) {
        int s = img->mru[i];
        if (s >= 0 && s < PK_MAX_VEHICLES && img->vehicles[s].used) {
            tmp[n++] = s;
        }
    }
    memcpy(img->mru, tmp, n * sizeof(tmp[0]));
    img->mru_count = n;
}

static void mru_touch(pk_store_image_t *img, int slot)
{
    int tmp[PK_MAX_VEHICLES];
    unsigned n = 0;
    unsigned i;
    if (slot < 0) {
        return;
    }
    tmp[n++] = slot;
    for (i = 0; i < img->mru_count && n < PK_MAX_VEHICLES; i++) {
        if (img->mru[i] != slot) {
            tmp[n++] = img->mru[i];
        }
    }
    memcpy(img->mru, tmp, n * sizeof(tmp[0]));
    img->mru_count = n;
}

void pk_store_init(pk_store_t *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    image_clear(&s->live);
    image_clear(&s->journal);
    s->journal_valid = false;
    s->committed = false;
    s->physical_nvs_attached = false; /* 待真机: real NVS is not wired */
}

void pk_store_reset_empty(pk_store_t *s)
{
    const pk_nvs_port_t *nvs = s ? s->nvs : NULL;
    void *nvs_ctx = s ? s->nvs_ctx : NULL;
    bool attached = s ? s->physical_nvs_attached : false;
    pk_store_init(s);
    if (s) {
        s->nvs = nvs;
        s->nvs_ctx = nvs_ctx;
        s->physical_nvs_attached = attached;
    }
}

int pk_store_count(const pk_store_t *s)
{
    return s ? (int)live_count(&s->live) : 0;
}

int pk_store_current(const pk_store_t *s)
{
    return s ? s->live.current : PK_VEHICLE_NONE;
}

bool pk_store_slot_used(const pk_store_t *s, int slot)
{
    if (!s || slot < 0 || slot >= PK_MAX_VEHICLES) {
        return false;
    }
    return s->live.vehicles[slot].used;
}

const pk_vehicle_rec_t *pk_store_get(const pk_store_t *s, int slot)
{
    if (!pk_store_slot_used(s, slot)) {
        return NULL;
    }
    return &s->live.vehicles[slot];
}

int pk_store_find_vin(const pk_store_t *s, const char *vin)
{
    int i;
    if (!s || !vin) {
        return PK_VEHICLE_NONE;
    }
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (s->live.vehicles[i].used && strcmp(s->live.vehicles[i].vin, vin) == 0) {
            return i;
        }
    }
    return PK_VEHICLE_NONE;
}

static int alloc_slot(pk_store_image_t *img)
{
    int i;
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (!img->vehicles[i].used) {
            return i;
        }
    }
    return PK_VEHICLE_NONE;
}

bool pk_store_add_verified(pk_store_t *s, const char *vin, pk_store_crash_t crash)
{
    pk_store_image_t cand;
    int slot;

    if (!s || !vin || vin[0] == '\0') {
        return false;
    }
    if (live_count(&s->live) >= PK_MAX_VEHICLES) {
        return false;
    }
    if (pk_store_find_vin(s, vin) != PK_VEHICLE_NONE) {
        return false;
    }

    cand = s->live;
    slot = alloc_slot(&cand);
    if (slot == PK_VEHICLE_NONE) {
        return false;
    }
    memset(&cand.vehicles[slot], 0, sizeof(cand.vehicles[slot]));
    cand.vehicles[slot].used = true;
    strncpy(cand.vehicles[slot].vin, vin, PK_VIN_LEN);
    cand.vehicles[slot].vin[PK_VIN_LEN] = '\0';
    /* Per-car isolated key reference. This is a stub handle, not a real key.
     * Production must generate a key per car; do not share leftover material.
     */
    snprintf(cand.vehicles[slot].slot_id, sizeof(cand.vehicles[slot].slot_id), "car%d", slot);
    snprintf(cand.vehicles[slot].key_ref, sizeof(cand.vehicles[slot].key_ref), "wrapped-%d", slot);
    cand.vehicle_count = live_count(&cand);
    cand.generation++;
    mru_touch(&cand, slot);
    /* Do not auto-switch the current vehicle (C08). */

    s->journal = cand;
    s->journal_valid = true;
    s->committed = false;
    if (s->nvs && s->nvs->prepare) {
        if (s->nvs->prepare(s->nvs_ctx, &cand, sizeof(cand)) != PK_OK) {
            s->journal_valid = false;
            return false;
        }
    }
    if (crash == PK_STORE_CRASH_PREPARE) {
        /* Power loss before commit: keep the old image, do not report saved. */
        s->journal_valid = false;
        return false;
    }
    if (s->nvs && s->nvs->commit) {
        if (s->nvs->commit(s->nvs_ctx) != PK_OK) {
            s->journal_valid = false;
            return false;
        }
    }
    s->live = cand;
    s->committed = true;
    s->journal_valid = false;
    return true;
}

bool pk_store_delete(pk_store_t *s, int target, bool reset, pk_store_crash_t crash,
                     pk_store_image_t *out_live)
{
    pk_store_image_t next;
    int i;

    if (!s || (!reset && !pk_store_slot_used(s, target))) {
        return false;
    }
    next = s->live;
    next.generation++;
    for (i = 0; i < PK_MAX_VEHICLES; i++) {
        if (!next.vehicles[i].used) {
            continue;
        }
        if (reset || i == target) {
            /* Isolated key_ref is dropped with the record. A later physical
             * port must refcount shared wraps and must not claim cryptographic
             * erase has occurred.
             */
            memset(&next.vehicles[i], 0, sizeof(next.vehicles[i]));
        }
    }
    mru_rebuild(&next);
    next.vehicle_count = live_count(&next);
    if (next.current >= 0 && (reset || next.current == target || !next.vehicles[next.current].used)) {
        next.current = next.mru_count ? next.mru[0] : PK_VEHICLE_NONE;
    }
    if (reset) {
        next.brightness = PK_BRIGHTNESS_DEFAULT;
        next.power = PK_POWER_COMFORT;
    }

    s->journal = next;
    s->journal_valid = true;
    s->committed = false;
    if (s->nvs && s->nvs->prepare) {
        if (s->nvs->prepare(s->nvs_ctx, &next, sizeof(next)) != PK_OK) {
            s->journal_valid = false;
            if (out_live) {
                *out_live = s->live;
            }
            return false;
        }
    }
    if (crash == PK_STORE_CRASH_PREPARE) {
        /* Not deleted. Old configuration remains. */
        s->journal_valid = false;
        if (out_live) {
            *out_live = s->live;
        }
        return false;
    }
    if (s->nvs && s->nvs->commit) {
        if (s->nvs->commit(s->nvs_ctx) != PK_OK) {
            s->journal_valid = false;
            if (out_live) {
                *out_live = s->live;
            }
            return false;
        }
    }
    s->live = next;
    s->committed = true;
    if (crash == PK_STORE_CRASH_COMMIT) {
        /* After commit, the new image wins; tombstoned items stay gone. */
        s->journal_valid = false;
        if (out_live) {
            *out_live = s->live;
        }
        return true;
    }
    s->journal_valid = false;
    if (out_live) {
        *out_live = s->live;
    }
    return true;
}

bool pk_store_switch_current(pk_store_t *s, int slot)
{
    pk_store_image_t next;

    if (!pk_store_slot_used(s, slot)) {
        return false;
    }
    next = s->live;
    next.current = slot;
    next.generation++;
    mru_touch(&next, slot);
    if (s->nvs && s->nvs->prepare && s->nvs->commit) {
        if (s->nvs->prepare(s->nvs_ctx, &next, sizeof(next)) != PK_OK ||
            s->nvs->commit(s->nvs_ctx) != PK_OK) {
            return false;
        }
    }
    s->live = next;
    return true;
}

void pk_store_recover(pk_store_t *s, pk_store_image_t *out_live)
{
    int i;
    if (!s) {
        return;
    }
    if (s->nvs && s->nvs->recover) {
        pk_store_image_t rec;
        if (s->nvs->recover(s->nvs_ctx, &rec, sizeof(rec)) == PK_OK) {
            for (i = 0; i < PK_MAX_VEHICLES; i++) {
                rec.vehicles[i].vin[PK_VIN_LEN] = '\0';
                rec.vehicles[i].slot_id[PK_SLOT_ID_LEN - 1] = '\0';
            }
            if (rec.mru_count > PK_MAX_VEHICLES) {
                rec.mru_count = PK_MAX_VEHICLES;
            }
            mru_rebuild(&rec);
            rec.vehicle_count = live_count(&rec);
            if (rec.current < 0 || rec.current >= PK_MAX_VEHICLES ||
                !rec.vehicles[rec.current].used) {
                rec.current = rec.mru_count ? rec.mru[0] : PK_VEHICLE_NONE;
            }
            if (rec.brightness < PK_BRIGHTNESS_MIN || rec.brightness > PK_BRIGHTNESS_MAX) {
                rec.brightness = PK_BRIGHTNESS_DEFAULT;
            }
            if (rec.power != PK_POWER_COMFORT && rec.power != PK_POWER_ENDURANCE) {
                rec.power = PK_POWER_COMFORT;
            }
            s->live = rec;
            s->committed = true;
        }
    }
    /* Before commit: journal is discarded, live is the old image.
     * After commit: live already holds the new image.
     */
    s->journal_valid = false;
    if (out_live) {
        *out_live = s->live;
    }
}

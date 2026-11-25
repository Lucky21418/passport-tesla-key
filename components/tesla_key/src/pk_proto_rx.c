#include "pk_proto_internal.h"

#include <pb_decode.h>

#include "car_server.pb.h"
#include "signatures.pb.h"
#include "universal_message.pb.h"
#include "vcsec.pb.h"

#include <math.h>
#include <string.h>

/* ==========================================================================
 * 1. Session Lifecycle & Official 64-bit Anti-Replay Window (D02)
 * ========================================================================== */

void pk_proto_session_clear(pk_proto_session_t *sess)
{
    if (sess) {
        pk_proto_zeroize(sess, sizeof(*sess));
    }
}

void pk_proto_rx_window_init(pk_proto_rx_window_t *w)
{
    if (w) {
        memset(w, 0, sizeof(*w));
    }
}

/* Official vehicle-command internal/authentication/window.go semantics:
 *   - First response counter (including 0) is accepted.
 *   - Out-of-order within [counter-64 .. counter-1] is accepted once.
 *   - Duplicate (adjacent or non-adjacent A-B-A) and age > 64 are rejected.
 *   - UB-safe shift guard for shift >= 64. */
bool pk_proto_rx_window_accept(pk_proto_rx_window_t *w, uint32_t counter)
{
    if (!w) {
        return false;
    }
    if (!w->used) {
        w->used = true;
        w->counter = counter;
        w->history = 0ULL;
        return true;
    }
    if (counter == w->counter) {
        return false;
    }
    if (counter < w->counter) {
        uint32_t age = w->counter - counter;
        uint64_t mask;
        if (age > 64u) {
            return false;
        }
        mask = 1ULL << (age - 1u);
        if ((w->history & mask) != 0ULL) {
            return false;
        }
        w->history |= mask;
        return true;
    }
    {
        uint32_t shift = counter - w->counter;
        if (shift >= 64u) {
            w->history = (shift == 64u) ? (1ULL << 63) : 0ULL;
        } else {
            w->history = (w->history << shift) | (1ULL << (shift - 1u));
        }
        w->counter = counter;
        return true;
    }
}

bool pk_proto_fault_needs_resync(int fault)
{
    switch (fault) {
    case PK_PROTO_FAULT_INVALID_SIGNATURE:
    case PK_PROTO_FAULT_INVALID_COUNTER:
    case PK_PROTO_FAULT_INCORRECT_EPOCH:
    case PK_PROTO_FAULT_TIME_EXPIRED:
    case PK_PROTO_FAULT_TTL_TOO_LONG:
    case PK_PROTO_FAULT_REPEATED_COUNTER:
        return true;
    default:
        return false;
    }
}

bool pk_proto_fault_is_key_error(int fault)
{
    return fault == PK_PROTO_FAULT_UNKNOWN_KEY ||
           fault == PK_PROTO_FAULT_INACTIVE_KEY ||
           fault == PK_PROTO_FAULT_INVALID_KEY_HANDLE;
}

/* ==========================================================================
 * 2. Incoming RoutableMessage Parser & AEAD Decryption (official nanopb)
 * ========================================================================== */

bool pk_proto_parse_routable(const uint8_t *buf, size_t len, pk_proto_routable_t *out)
{
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_zero;
    pb_istream_t stream;

    if (!buf || len == 0u || len > PK_PROTO_MAX_FRAME_LEN || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, UniversalMessage_RoutableMessage_fields, &msg)) {
        return false;
    }

    if (msg.has_to_destination) {
        if (msg.to_destination.which_sub_destination == UniversalMessage_Destination_domain_tag) {
            out->has_to_domain = true;
            out->to_domain = (pk_proto_domain_t)msg.to_destination.sub_destination.domain;
        } else if (msg.to_destination.which_sub_destination ==
                   UniversalMessage_Destination_routing_address_tag) {
            if (msg.to_destination.sub_destination.routing_address.size == PK_PROTO_ROUTING_ADDR_LEN) {
                out->has_to_routing_addr = true;
                memcpy(out->to_routing_addr,
                       msg.to_destination.sub_destination.routing_address.bytes,
                       PK_PROTO_ROUTING_ADDR_LEN);
            }
        }
    }

    if (msg.has_from_destination) {
        if (msg.from_destination.which_sub_destination == UniversalMessage_Destination_domain_tag) {
            out->has_from_domain = true;
            out->from_domain = (pk_proto_domain_t)msg.from_destination.sub_destination.domain;
        } else if (msg.from_destination.which_sub_destination ==
                   UniversalMessage_Destination_routing_address_tag) {
            if (msg.from_destination.sub_destination.routing_address.size == PK_PROTO_ROUTING_ADDR_LEN) {
                out->has_from_routing_addr = true;
                memcpy(out->from_routing_addr,
                       msg.from_destination.sub_destination.routing_address.bytes,
                       PK_PROTO_ROUTING_ADDR_LEN);
            }
        }
    }

    if (msg.request_uuid.size == PK_PROTO_UUID_LEN) {
        out->has_request_uuid = true;
        memcpy(out->request_uuid, msg.request_uuid.bytes, PK_PROTO_UUID_LEN);
    }
    if (msg.uuid.size == PK_PROTO_UUID_LEN) {
        out->has_uuid = true;
        memcpy(out->uuid, msg.uuid.bytes, PK_PROTO_UUID_LEN);
    }
    out->flags = msg.flags;

    if (msg.has_signedMessageStatus) {
        out->has_msg_status = true;
        out->operation_status = (int)msg.signedMessageStatus.operation_status;
        out->signed_message_fault = (int)msg.signedMessageStatus.signed_message_fault;
    }

    if (msg.which_payload == UniversalMessage_RoutableMessage_session_info_tag) {
        out->has_session_info = true;
        out->session_info_len = msg.payload.session_info.size;
        if (out->session_info_len > 0u) {
            memcpy(out->session_info_bytes, msg.payload.session_info.bytes, out->session_info_len);
        }
    } else if (msg.which_payload == UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag) {
        out->has_payload_bytes = true;
        out->payload_len = msg.payload.protobuf_message_as_bytes.size;
        if (out->payload_len > 0u) {
            memcpy(out->payload_bytes, msg.payload.protobuf_message_as_bytes.bytes, out->payload_len);
        }
    }

    if (msg.which_sub_sigData == UniversalMessage_RoutableMessage_signature_data_tag) {
        const Signatures_SignatureData *sd = &msg.sub_sigData.signature_data;
        out->has_sig_data = true;
        out->sig_type_tag = sd->which_sig_type;

        if (sd->which_sig_type == Signatures_SignatureData_session_info_tag_tag) {
            out->has_session_info_tag = true;
            out->session_info_tag_len = sd->sig_type.session_info_tag.tag.size;
            if (out->session_info_tag_len > 0u && out->session_info_tag_len <= PK_PROTO_HMAC_LEN) {
                memcpy(out->session_info_tag,
                       sd->sig_type.session_info_tag.tag.bytes,
                       out->session_info_tag_len);
            }
        } else if (sd->which_sig_type == Signatures_SignatureData_AES_GCM_Response_data_tag) {
            const Signatures_AES_GCM_Response_Signature_Data *r =
                &sd->sig_type.AES_GCM_Response_data;
            /* Always record has_aes_gcm_resp + exact wire lengths so truncated
             * nonce/tag are explicitly rejected rather than falling back to plaintext! */
            out->has_aes_gcm_resp = true;
            out->resp_counter = r->counter;
            out->resp_nonce_len = r->nonce.size;
            if (r->nonce.size > 0u && r->nonce.size <= PK_PROTO_NONCE_LEN) {
                memcpy(out->resp_nonce, r->nonce.bytes, r->nonce.size);
            }
            out->resp_tag_len = r->tag.size;
            if (r->tag.size > 0u && r->tag.size <= PK_PROTO_TAG_LEN) {
                memcpy(out->resp_tag, r->tag.bytes, r->tag.size);
            }
        }
    }

    return true;
}

bool pk_proto_decrypt_routable_payload(const pk_proto_routable_t *msg,
                                       const pk_proto_decrypt_t *ctx,
                                       uint8_t *plain_out, size_t plain_cap,
                                       size_t *plain_len_out)
{
    uint8_t aad[32];

    if (!msg || !plain_out || !plain_len_out ||
        !msg->has_payload_bytes || msg->payload_len > plain_cap) {
        return false;
    }
    if (!msg->has_sig_data && !msg->has_aes_gcm_resp) {
        if (msg->payload_len > 0u) {
            memcpy(plain_out, msg->payload_bytes, msg->payload_len);
        }
        *plain_len_out = msg->payload_len;
        return true;
    }
    /* Reject any unexpected signature type or invalid nonce/tag length fail-closed */
    if (!msg->has_aes_gcm_resp ||
        msg->resp_nonce_len != PK_PROTO_NONCE_LEN ||
        msg->resp_tag_len != PK_PROTO_TAG_LEN) {
        pk_proto_zeroize(plain_out, msg->payload_len);
        return false;
    }
    if (!ctx || !ctx->sess || !ctx->sess->valid || !ctx->vin || !ctx->req_id) {
        pk_proto_zeroize(plain_out, msg->payload_len);
        return false;
    }
    if (!pk_proto_compute_resp_metadata_hash(msg->from_domain, ctx->vin, msg->resp_counter,
                                             msg->flags, ctx->req_id,
                                             (uint32_t)msg->signed_message_fault, aad)) {
        pk_proto_zeroize(plain_out, msg->payload_len);
        return false;
    }
    if (!pk_proto_aes128_gcm_decrypt(ctx->sess->session_key, msg->resp_nonce, aad, sizeof(aad),
                                     msg->payload_bytes, msg->payload_len,
                                     msg->resp_tag, plain_out)) {
        pk_proto_zeroize(aad, sizeof(aad));
        pk_proto_zeroize(plain_out, msg->payload_len);
        return false;
    }
    pk_proto_zeroize(aad, sizeof(aad));
    *plain_len_out = msg->payload_len;
    return true;
}

/* ==========================================================================
 * 3. VCSEC Response Parser (official nanopb VCSEC_FromVCSECMessage)
 * ========================================================================== */

static pk_closure_t map_vcsec_closure(VCSEC_ClosureState_E v)
{
    switch (v) {
    case VCSEC_ClosureState_E_CLOSURESTATE_CLOSED:        return PK_CLOSURE_CLOSED;
    case VCSEC_ClosureState_E_CLOSURESTATE_OPEN:          return PK_CLOSURE_OPEN;
    case VCSEC_ClosureState_E_CLOSURESTATE_AJAR:          return PK_CLOSURE_AJAR;
    case VCSEC_ClosureState_E_CLOSURESTATE_UNKNOWN:       return PK_CLOSURE_UNKNOWN;
    case VCSEC_ClosureState_E_CLOSURESTATE_FAILED_UNLATCH: return PK_CLOSURE_FAILED_UNLATCH;
    case VCSEC_ClosureState_E_CLOSURESTATE_OPENING:       return PK_CLOSURE_OPENING;
    case VCSEC_ClosureState_E_CLOSURESTATE_CLOSING:       return PK_CLOSURE_CLOSING;
    default:                                              return PK_CLOSURE_UNKNOWN;
    }
}

static pk_lock_read_t map_vcsec_lock(VCSEC_VehicleLockState_E v)
{
    switch (v) {
    case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_UNLOCKED:           return PK_LOCK_READ_UNLOCKED;
    case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_LOCKED:             return PK_LOCK_READ_LOCKED;
    case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_INTERNAL_LOCKED:    return PK_LOCK_READ_INTERNAL_LOCKED;
    case VCSEC_VehicleLockState_E_VEHICLELOCKSTATE_SELECTIVE_UNLOCKED: return PK_LOCK_READ_SELECTIVE_UNLOCKED;
    default:                                                           return PK_LOCK_READ_UNKNOWN;
    }
}

static bool pb_take_varint(const uint8_t *buf, size_t end, size_t *off, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;

    if (!buf || !off || !out) {
        return false;
    }
    while (*off < end && shift <= 63) {
        uint8_t b = buf[(*off)++];
        v |= (uint64_t)(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool pb_skip_value(const uint8_t *buf, size_t end, size_t *off, unsigned wire)
{
    uint64_t n = 0;

    if (!off || *off > end) {
        return false;
    }
    switch (wire) {
    case 0:
        return pb_take_varint(buf, end, off, &n);
    case 1:
        if (end - *off < 8u) {
            return false;
        }
        *off += 8u;
        return true;
    case 2:
        if (!pb_take_varint(buf, end, off, &n) || n > (uint64_t)(end - *off)) {
            return false;
        }
        *off += (size_t)n;
        return true;
    case 5:
        if (end - *off < 4u) {
            return false;
        }
        *off += 4u;
        return true;
    default:
        return false;
    }
}

/* proto3 omits VehicleLockState_E when it is the default UNLOCKED(0), and
 * nanopb then decodes the missing field as 0. Presence is the tag inside
 * VehicleStatus, not the decoded value. Field 2 inside closureStatuses is
 * a door, not the lock. */
static bool vcsec_lock_field_present(const uint8_t *buf, size_t len)
{
    size_t off = 0;

    if (!buf) {
        return false;
    }
    while (off < len) {
        uint64_t key = 0;
        unsigned field;
        unsigned wire;

        if (!pb_take_varint(buf, len, &off, &key)) {
            return false;
        }
        field = (unsigned)(key >> 3);
        wire = (unsigned)(key & 7u);
        if (field == VCSEC_FromVCSECMessage_vehicleStatus_tag && wire == 2u) {
            uint64_t n = 0;
            size_t sub;
            size_t sub_end;

            if (!pb_take_varint(buf, len, &off, &n) || n > (uint64_t)(len - off)) {
                return false;
            }
            sub = off;
            sub_end = off + (size_t)n;
            off = sub_end;
            while (sub < sub_end) {
                uint64_t sk = 0;
                unsigned sf;
                unsigned sw;

                if (!pb_take_varint(buf, sub_end, &sub, &sk)) {
                    return false;
                }
                sf = (unsigned)(sk >> 3);
                sw = (unsigned)(sk & 7u);
                if (sf == VCSEC_VehicleStatus_vehicleLockState_tag) {
                    return true;
                }
                if (!pb_skip_value(buf, sub_end, &sub, sw)) {
                    return false;
                }
            }
            continue;
        }
        if (!pb_skip_value(buf, len, &off, wire)) {
            return false;
        }
    }
    return false;
}

bool pk_proto_parse_vcsec_response(const uint8_t *buf, size_t len, pk_proto_vcsec_resp_t *out)
{
    VCSEC_FromVCSECMessage fm = VCSEC_FromVCSECMessage_init_zero;
    pb_istream_t stream;

    if (!buf || !out || len > PK_PROTO_MAX_FRAME_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, VCSEC_FromVCSECMessage_fields, &fm)) {
        return false;
    }

    switch (fm.which_sub_message) {
    case VCSEC_FromVCSECMessage_vehicleStatus_tag: {
        const VCSEC_VehicleStatus *vs = &fm.sub_message.vehicleStatus;
        out->has_vehicle_status = true;
        /* closureStatuses is a submessage with nanopb has_closureStatuses.
         * vehicleLockState is a proto3 enum: absent decodes as UNLOCKED(0).
         * Only a tag on the wire is a lock observation. */
        if (vcsec_lock_field_present(buf, len)) {
            out->has_lock_state = true;
            out->lock_state = map_vcsec_lock(vs->vehicleLockState);
        } else {
            out->has_lock_state = false;
            out->lock_state = PK_LOCK_READ_UNKNOWN;
        }
        out->sleep_status = (int)vs->vehicleSleepStatus;
        if (vs->has_closureStatuses) {
            const VCSEC_ClosureStatuses *cs = &vs->closureStatuses;
            out->has_closure_statuses = true;
            out->closures[PK_CLOSURE_FRONT_LEFT]  = map_vcsec_closure(cs->frontDriverDoor);
            out->closures[PK_CLOSURE_FRONT_RIGHT] = map_vcsec_closure(cs->frontPassengerDoor);
            out->closures[PK_CLOSURE_REAR_LEFT]   = map_vcsec_closure(cs->rearDriverDoor);
            out->closures[PK_CLOSURE_REAR_RIGHT]  = map_vcsec_closure(cs->rearPassengerDoor);
            out->closures[PK_CLOSURE_REAR]        = map_vcsec_closure(cs->rearTrunk);
            out->closures[PK_CLOSURE_FRONT]       = map_vcsec_closure(cs->frontTrunk);
            out->charge_port                      = map_vcsec_closure(cs->chargePort);
        }
        break;
    }
    case VCSEC_FromVCSECMessage_commandStatus_tag: {
        const VCSEC_CommandStatus *cmd = &fm.sub_message.commandStatus;
        out->has_command_status = true;
        out->op_status = (int)cmd->operationStatus;
        if (cmd->which_sub_message == VCSEC_CommandStatus_signedMessageStatus_tag) {
            out->signed_msg_fault = (int)cmd->sub_message.signedMessageStatus.signedMessageInformation;
        } else if (cmd->which_sub_message == VCSEC_CommandStatus_whitelistOperationStatus_tag) {
            out->has_whitelist_status = true;
            out->whitelist_info =
                (int)cmd->sub_message.whitelistOperationStatus.whitelistOperationInformation;
        }
        break;
    }
    case VCSEC_FromVCSECMessage_whitelistEntryInfo_tag: {
        const VCSEC_WhitelistEntryInfo *we = &fm.sub_message.whitelistEntryInfo;
        out->has_whitelist_entry_info = true;
        out->entry_key_role = (uint32_t)we->keyRole;
        if (we->has_keyId && we->keyId.publicKeySHA1.size <= sizeof(out->entry_pubkey_id)) {
            memcpy(out->entry_pubkey_id, we->keyId.publicKeySHA1.bytes,
                   we->keyId.publicKeySHA1.size);
        }
        break;
    }
    case VCSEC_FromVCSECMessage_nominalError_tag:
        out->has_nominal_error = true;
        out->generic_error = (int)fm.sub_message.nominalError.genericError;
        break;
    default:
        break;
    }

    return true;
}

/* ==========================================================================
 * 4. CarServer Response Parser (official nanopb CarServer_Response)
 * ========================================================================== */

static bool ts_to_ms(const google_protobuf_Timestamp *ts, int64_t *ms_out)
{
    if (!ts || !ms_out || ts->seconds <= 0 || ts->seconds > 922337203685477LL) {
        return false;
    }
    *ms_out = ts->seconds * 1000LL + (int64_t)(ts->nanos / 1000000);
    return true;
}

bool pk_proto_parse_carserver_response(const uint8_t *buf, size_t len, pk_proto_carserver_resp_t *out)
{
    CarServer_Response resp = CarServer_Response_init_zero;
    pb_istream_t stream;

    if (!buf || !out || len > PK_PROTO_MAX_FRAME_LEN) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, CarServer_Response_fields, &resp)) {
        return false;
    }

    if (resp.has_actionStatus) {
        out->has_action_status = true;
        out->result = (int)resp.actionStatus.result;
        if (resp.actionStatus.has_result_reason &&
            resp.actionStatus.result_reason.which_reason == CarServer_ResultReason_plain_text_tag) {
            strncpy(out->reason_text, resp.actionStatus.result_reason.reason.plain_text,
                    sizeof(out->reason_text) - 1u);
            out->reason_text[sizeof(out->reason_text) - 1u] = '\0';
        }
    }

    if (resp.which_response_msg != CarServer_Response_vehicleData_tag) {
        return true;
    }

    {
        const CarServer_VehicleData *vd = &resp.response_msg.vehicleData;
        out->has_vehicle_data = true;

        /* DriveState (D04: genuine nanopb oneof presence + finite float validation) */
        if (vd->has_drive_state) {
            const CarServer_DriveState *ds = &vd->drive_state;
            out->has_drive_state = true;
            if (ds->has_shift_state) {
                switch (ds->shift_state.which_type) {
                case CarServer_ShiftState_P_tag:       out->shift = PK_SHIFT_P; break;
                case CarServer_ShiftState_R_tag:       out->shift = PK_SHIFT_R; break;
                case CarServer_ShiftState_N_tag:       out->shift = PK_SHIFT_N; break;
                case CarServer_ShiftState_D_tag:       out->shift = PK_SHIFT_D; break;
                case CarServer_ShiftState_Invalid_tag:
                case CarServer_ShiftState_SNA_tag:     out->shift = PK_SHIFT_INVALID; break;
                default:                               out->shift = PK_SHIFT_ABSENT; break;
                }
            } else {
                out->shift = PK_SHIFT_ABSENT;
            }
            if (ds->has_timestamp && ts_to_ms(&ds->timestamp, &out->drive_ts_ms)) {
                out->drive_ts_present = true;
            }
            if (ds->which_optional_speed == CarServer_DriveState_speed_tag) {
                out->speed_present = true;
                if (ds->optional_speed.speed <= 1000000u) {
                    out->speed_mph = (int)ds->optional_speed.speed;
                } else {
                    out->speed_invalid = true;
                    out->speed_mph = -1;
                }
            }
            if (ds->which_optional_power == CarServer_DriveState_power_tag) {
                out->power_present = true;
                out->power_kw = (int)ds->optional_power.power;
            }
            if (ds->which_optional_speed_float == CarServer_DriveState_speed_float_tag) {
                float fv = ds->optional_speed_float.speed_float;
                /* S2 §2D: preserve BOTH presence AND invalid semantics so
                 * speed=0 + speed_float=NaN/Inf cannot masquerade as absent
                 * and qualify as Park, while unrelated display sections remain intact. */
                out->speed_float_present = true;
                if (isfinite(fv) && fv >= 0.0f && fv < 1000000.0f) {
                    out->speed_float_mph =
                        (fv == 0.0f) ? 0 : (int)(fv + 0.999f);
                } else {
                    out->speed_float_invalid = true;
                    out->speed_invalid = true;
                    out->speed_float_mph = -1;
                }
            }
            /* Conflicting zero vs non-zero speed fields mark speed evidence invalid. */
            if (out->speed_present && out->speed_float_present &&
                !out->speed_invalid &&
                ((out->speed_mph == 0) != (out->speed_float_mph == 0))) {
                out->speed_invalid = true;
            }
        }

        /* ClosuresState */
        if (vd->has_closures_state) {
            const CarServer_ClosuresState *cs = &vd->closures_state;
            out->has_closures_state = true;
            if (cs->has_sentry_mode_state && cs->sentry_mode_state.which_type > 0u) {
                out->sentry_present = true;
                out->sentry_armed =
                    (cs->sentry_mode_state.which_type != CarServer_ClosuresState_SentryModeState_Off_tag);
            }
            if (cs->which_optional_door_open_driver_front ==
                CarServer_ClosuresState_door_open_driver_front_tag) {
                out->closures_open_present[PK_CLOSURE_FRONT_LEFT] = true;
                out->closures_open[PK_CLOSURE_FRONT_LEFT] =
                    cs->optional_door_open_driver_front.door_open_driver_front;
            }
            if (cs->which_optional_door_open_driver_rear ==
                CarServer_ClosuresState_door_open_driver_rear_tag) {
                out->closures_open_present[PK_CLOSURE_REAR_LEFT] = true;
                out->closures_open[PK_CLOSURE_REAR_LEFT] =
                    cs->optional_door_open_driver_rear.door_open_driver_rear;
            }
            if (cs->which_optional_door_open_passenger_front ==
                CarServer_ClosuresState_door_open_passenger_front_tag) {
                out->closures_open_present[PK_CLOSURE_FRONT_RIGHT] = true;
                out->closures_open[PK_CLOSURE_FRONT_RIGHT] =
                    cs->optional_door_open_passenger_front.door_open_passenger_front;
            }
            if (cs->which_optional_door_open_passenger_rear ==
                CarServer_ClosuresState_door_open_passenger_rear_tag) {
                out->closures_open_present[PK_CLOSURE_REAR_RIGHT] = true;
                out->closures_open[PK_CLOSURE_REAR_RIGHT] =
                    cs->optional_door_open_passenger_rear.door_open_passenger_rear;
            }
            if (cs->which_optional_door_open_trunk_front ==
                CarServer_ClosuresState_door_open_trunk_front_tag) {
                out->closures_open_present[PK_CLOSURE_FRONT] = true;
                out->closures_open[PK_CLOSURE_FRONT] =
                    cs->optional_door_open_trunk_front.door_open_trunk_front;
            }
            if (cs->which_optional_door_open_trunk_rear ==
                CarServer_ClosuresState_door_open_trunk_rear_tag) {
                out->closures_open_present[PK_CLOSURE_REAR] = true;
                out->closures_open[PK_CLOSURE_REAR] =
                    cs->optional_door_open_trunk_rear.door_open_trunk_rear;
            }
            if (cs->which_optional_locked == CarServer_ClosuresState_locked_tag) {
                out->locked_present = true;
                out->locked = cs->optional_locked.locked;
            }
            if (cs->which_optional_remote_start == CarServer_ClosuresState_remote_start_tag) {
                out->remote_start_present = true;
                out->remote_start = cs->optional_remote_start.remote_start;
            }
            if (cs->has_timestamp && ts_to_ms(&cs->timestamp, &out->closures_ts_ms)) {
                out->closures_ts_present = true;
            }
        }

        /* ChargeState */
        if (vd->has_charge_state) {
            const CarServer_ChargeState *ch = &vd->charge_state;
            out->has_charge_state = true;
            if (ch->has_charging_state) {
                out->charging_state = (int)ch->charging_state.which_type;
            }
            if (ch->has_conn_charge_cable && ch->conn_charge_cable.which_type > 0u) {
                out->cable_connected_present = true;
                out->cable_connected =
                    (ch->conn_charge_cable.which_type != CarServer_ChargeState_CableType_SNA_tag);
            }
            if (ch->has_timestamp && ts_to_ms(&ch->timestamp, &out->charge_ts_ms)) {
                out->charge_ts_present = true;
            }
            if (ch->which_optional_battery_range == CarServer_ChargeState_battery_range_tag) {
                float fv = ch->optional_battery_range.battery_range;
                if (isfinite(fv) && fv >= 0.0f) {
                    out->battery_range_present = true;
                    out->battery_range_tenths_mi = (int)(fv * 10.0f + 0.5f);
                }
            }
            if (ch->which_optional_battery_level == CarServer_ChargeState_battery_level_tag) {
                out->battery_level_present = true;
                out->battery_level = (int)ch->optional_battery_level.battery_level;
            }
            if (ch->which_optional_usable_battery_level ==
                CarServer_ChargeState_usable_battery_level_tag) {
                out->usable_battery_level_present = true;
                out->usable_battery_level =
                    (int)ch->optional_usable_battery_level.usable_battery_level;
            }
            if (ch->which_optional_minutes_to_full_charge ==
                CarServer_ChargeState_minutes_to_full_charge_tag) {
                out->minutes_to_full_present = true;
                out->minutes_to_full =
                    (int)ch->optional_minutes_to_full_charge.minutes_to_full_charge;
            }
            if (ch->which_optional_minutes_to_charge_limit ==
                CarServer_ChargeState_minutes_to_charge_limit_tag) {
                out->minutes_to_limit_present = true;
                out->minutes_to_limit =
                    (int)ch->optional_minutes_to_charge_limit.minutes_to_charge_limit;
            }
            if (ch->which_optional_charge_port_door_open ==
                CarServer_ChargeState_charge_port_door_open_tag) {
                out->charge_port_door_open_present = true;
                out->charge_port_door_open =
                    ch->optional_charge_port_door_open.charge_port_door_open;
            }
            if (ch->which_optional_charge_current_request_max ==
                CarServer_ChargeState_charge_current_request_max_tag) {
                out->charge_current_max_present = true;
                out->charge_current_max =
                    (int)ch->optional_charge_current_request_max.charge_current_request_max;
            }
            if (ch->which_optional_charging_amps == CarServer_ChargeState_charging_amps_tag) {
                out->charging_amps_present = true;
                out->charging_amps = (int)ch->optional_charging_amps.charging_amps;
            }
        }

        /* ClimateState */
        if (vd->has_climate_state) {
            const CarServer_ClimateState *cl = &vd->climate_state;
            out->has_climate_state = true;
            if (cl->has_timestamp && ts_to_ms(&cl->timestamp, &out->climate_ts_ms)) {
                out->climate_ts_present = true;
            }
            if (cl->has_defrost_mode && cl->defrost_mode.which_type > 0u) {
                out->is_preconditioning_present = true;
                out->is_preconditioning =
                    (cl->defrost_mode.which_type == CarServer_ClimateState_DefrostMode_Max_tag);
            }
            if (cl->which_optional_inside_temp_celsius ==
                CarServer_ClimateState_inside_temp_celsius_tag) {
                float fv = cl->optional_inside_temp_celsius.inside_temp_celsius;
                if (isfinite(fv)) {
                    out->inside_temp_present = true;
                    out->inside_temp_tenths_c = (int)(fv * 10.0f + (fv >= 0.0f ? 0.5f : -0.5f));
                }
            }
            if (cl->which_optional_outside_temp_celsius ==
                CarServer_ClimateState_outside_temp_celsius_tag) {
                float fv = cl->optional_outside_temp_celsius.outside_temp_celsius;
                if (isfinite(fv)) {
                    out->outside_temp_present = true;
                    out->outside_temp_tenths_c = (int)(fv * 10.0f + (fv >= 0.0f ? 0.5f : -0.5f));
                }
            }
            if (cl->which_optional_is_climate_on == CarServer_ClimateState_is_climate_on_tag) {
                out->is_climate_on_present = true;
                out->is_climate_on = cl->optional_is_climate_on.is_climate_on;
            }
            if (cl->which_optional_seat_heater_left ==
                CarServer_ClimateState_seat_heater_left_tag) {
                out->seat_heater_left_present = true;
                out->seat_heater_left = (int)cl->optional_seat_heater_left.seat_heater_left;
            }
            if (cl->which_optional_seat_heater_right ==
                CarServer_ClimateState_seat_heater_right_tag) {
                out->seat_heater_right_present = true;
                out->seat_heater_right = (int)cl->optional_seat_heater_right.seat_heater_right;
            }
            if (cl->which_optional_seat_heater_rear_left ==
                CarServer_ClimateState_seat_heater_rear_left_tag) {
                out->seat_heater_rl_present = true;
                out->seat_heater_rl = (int)cl->optional_seat_heater_rear_left.seat_heater_rear_left;
            }
            if (cl->which_optional_seat_heater_rear_right ==
                CarServer_ClimateState_seat_heater_rear_right_tag) {
                out->seat_heater_rr_present = true;
                out->seat_heater_rr =
                    (int)cl->optional_seat_heater_rear_right.seat_heater_rear_right;
            }
            if (cl->which_optional_steering_wheel_heater ==
                CarServer_ClimateState_steering_wheel_heater_tag) {
                out->steering_heater_present = true;
                out->steering_heater_on = cl->optional_steering_wheel_heater.steering_wheel_heater;
            }
            if (cl->which_optional_is_preconditioning ==
                CarServer_ClimateState_is_preconditioning_tag) {
                out->is_preconditioning_present = true;
                out->is_preconditioning = cl->optional_is_preconditioning.is_preconditioning;
            }
        }

        /* TirePressureState */
        if (vd->has_tire_pressure_state) {
            const CarServer_TirePressureState *tp = &vd->tire_pressure_state;
            const struct {
                bool  present;
                float bar;
            } k_tpms[4] = {
                { tp->which_optional_tpms_pressure_fl == CarServer_TirePressureState_tpms_pressure_fl_tag,
                  tp->optional_tpms_pressure_fl.tpms_pressure_fl },
                { tp->which_optional_tpms_pressure_fr == CarServer_TirePressureState_tpms_pressure_fr_tag,
                  tp->optional_tpms_pressure_fr.tpms_pressure_fr },
                { tp->which_optional_tpms_pressure_rl == CarServer_TirePressureState_tpms_pressure_rl_tag,
                  tp->optional_tpms_pressure_rl.tpms_pressure_rl },
                { tp->which_optional_tpms_pressure_rr == CarServer_TirePressureState_tpms_pressure_rr_tag,
                  tp->optional_tpms_pressure_rr.tpms_pressure_rr },
            };
            size_t i;

            out->has_tire_pressure_state = true;
            if (tp->has_timestamp && ts_to_ms(&tp->timestamp, &out->tpms_ts_ms)) {
                out->tpms_ts_present = true;
            }
            for (i = 0u; i < 4u; i++) {
                if (k_tpms[i].present && isfinite(k_tpms[i].bar) && k_tpms[i].bar > 0.0f) {
                    out->tpms_present[i] = true;
                    out->tpms_mbar[i] = (int)(k_tpms[i].bar * 1000.0f + 0.5f);
                }
            }
        }
    }

    return true;
}

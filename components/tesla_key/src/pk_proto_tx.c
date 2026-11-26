#include "pk_proto_internal.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include "car_server.pb.h"
#include "signatures.pb.h"
#include "universal_message.pb.h"
#include "vcsec.pb.h"

#include <string.h>

/* ==========================================================================
 * 1. SessionInfo & Metadata Authentication (official nanopb Signatures_SessionInfo)
 * ========================================================================== */

bool pk_proto_session_info_tag_compute(const uint8_t session_key[16],
                                       const pk_proto_session_info_msg_t *msg,
                                       uint8_t tag_out[32])
{
    static const char label[] = "session info";
    uint8_t subkey[32];
    uint8_t msg_buf[512];
    pk_meta_tlv_t m;
    bool ok = false;

    if (!session_key || !msg || !msg->vin || !msg->challenge ||
        !msg->encoded_info || !tag_out ||
        msg->challenge_len > 64u || msg->encoded_info_len > 380u) {
        return false;
    }

    if (!pk_proto_hmac_sha256(session_key, 16u, (const uint8_t *)label, sizeof(label) - 1u, subkey)) {
        return false;
    }
    if (!pk_meta_tlv_begin(&m, msg_buf, sizeof(msg_buf),
                           (uint8_t)Signatures_SignatureType_SIGNATURE_TYPE_HMAC, -1, msg->vin)) {
        pk_proto_zeroize(subkey, sizeof(subkey));
        return false;
    }
    pk_meta_tlv_bytes(&m, (uint8_t)Signatures_Tag_TAG_CHALLENGE, msg->challenge, msg->challenge_len);
    (void)pk_meta_tlv_end(&m);
    pk_meta_tlv_raw(&m, msg->encoded_info, msg->encoded_info_len);
    if (!m.err) {
        ok = pk_proto_hmac_sha256(subkey, sizeof(subkey), msg_buf, m.pos, tag_out);
    }
    pk_proto_zeroize(subkey, sizeof(subkey));
    return ok;
}

bool pk_proto_session_apply_info(pk_proto_session_t *sess,
                                 const pk_proto_session_apply_t *in)
{
    Signatures_SessionInfo info = Signatures_SessionInfo_init_zero;
    pb_istream_t stream;
    uint8_t s_key[16];
    uint8_t calc_tag[32];
    bool same_epoch;
    bool prev_valid;

    if (!sess || !in || !in->privkey || !in->info.vin ||
        !in->info.encoded_info || in->info.encoded_info_len == 0u ||
        !in->tag || in->tag_len != PK_PROTO_HMAC_LEN ||
        !in->info.challenge || in->info.challenge_len == 0u) {
        return false;
    }

    stream = pb_istream_from_buffer(in->info.encoded_info, in->info.encoded_info_len);
    if (!pb_decode(&stream, Signatures_SessionInfo_fields, &info)) {
        return false;
    }
    if (info.status != Signatures_Session_Info_Status_SESSION_INFO_STATUS_OK ||
        info.publicKey.size != PK_PROTO_PUBKEY_LEN) {
        return false;
    }

    /* Enforce in-ctx peer identity continuity (NOT persistent NVS pinning):
     * once a first vehicle public key has been established in this RAM session,
     * reject any SessionInfo attempting to swap the peer key without polluting state. */
    if ((sess->has_peer_pubkey || sess->vehicle_pubkey[0] != 0u) &&
        !pk_proto_ct_equal(sess->vehicle_pubkey, info.publicKey.bytes, PK_PROTO_PUBKEY_LEN)) {
        return false;
    }

    if (!pk_proto_ecdh_derive_session_key(in->privkey, info.publicKey.bytes, s_key)) {
        return false;
    }
    if (!pk_proto_session_info_tag_compute(s_key, &in->info, calc_tag)) {
        pk_proto_zeroize(s_key, sizeof(s_key));
        return false;
    }
    if (!pk_proto_ct_equal(calc_tag, in->tag, PK_PROTO_HMAC_LEN)) {
        pk_proto_zeroize(calc_tag, sizeof(calc_tag));
        pk_proto_zeroize(s_key, sizeof(s_key));
        return false;
    }
    pk_proto_zeroize(calc_tag, sizeof(calc_tag));

    prev_valid = sess->valid;
    same_epoch = (sess->has_peer_pubkey || sess->vehicle_pubkey[0] != 0u) &&
                 memcmp(sess->epoch, info.epoch, PK_PROTO_EPOCH_LEN) == 0;

    /* Same-epoch stale clock sync: MUST NOT roll back clock_time, epoch, or counter.
     * Project-local stricter policy rejects with false so callers know sync was ignored. */
    if (same_epoch && (int32_t)(info.clock_time - sess->clock_time) < 0) {
        pk_proto_zeroize(s_key, sizeof(s_key));
        return false;
    }

    /* New epoch (e.g., vehicle reboot with a legitimately lower clock_time):
     * bump epoch_gen so any in-flight pending txn from the prior epoch is invalidated,
     * and accept the new epoch's counter per protocol.md ("MUST NOT rollback counter
     * unless the epoch changes"). Within the same epoch, keep monotonic counter. */
    if (!same_epoch) {
        if (prev_valid || sess->has_peer_pubkey) {
            sess->epoch_gen++;
        } else if (sess->epoch_gen == 0u) {
            sess->epoch_gen = 1u;
        }
        sess->counter = info.counter;
    } else if (info.counter > sess->counter) {
        sess->counter = info.counter;
    }

    sess->valid = true;
    sess->has_peer_pubkey = true;
    memcpy(sess->epoch, info.epoch, PK_PROTO_EPOCH_LEN);
    memcpy(sess->vehicle_pubkey, info.publicKey.bytes, PK_PROTO_PUBKEY_LEN);
    memcpy(sess->session_key, s_key, PK_PROTO_SESSION_KEY_LEN);
    sess->clock_time = info.clock_time;
    sess->sync_local_ms = in->now_ms;
    sess->handle = info.handle;
    sess->flags = info.flags;

    pk_proto_zeroize(s_key, sizeof(s_key));
    return true;
}

/* ==========================================================================
 * 2. Inner Domain Payload Encoders (official nanopb VCSEC & CarServer)
 * ========================================================================== */

static size_t encode_msg(const pb_msgdesc_t *fields, const void *src, uint8_t *out, size_t cap)
{
    pb_ostream_t stream;
    if (!out || cap == 0u) {
        return 0u;
    }
    stream = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&stream, fields, src)) {
        return 0u;
    }
    return stream.bytes_written;
}

size_t pk_proto_encode_vcsec_rke(pk_proto_rke_action_t rke, uint8_t *out, size_t cap)
{
    VCSEC_UnsignedMessage umsg = VCSEC_UnsignedMessage_init_zero;
    umsg.which_sub_message = VCSEC_UnsignedMessage_RKEAction_tag;
    umsg.sub_message.RKEAction = (VCSEC_RKEAction_E)rke;
    return encode_msg(VCSEC_UnsignedMessage_fields, &umsg, out, cap);
}

size_t pk_proto_encode_vcsec_closure(int closure_id, pk_proto_closure_move_t move, uint8_t *out, size_t cap)
{
    VCSEC_UnsignedMessage umsg = VCSEC_UnsignedMessage_init_zero;
    umsg.which_sub_message = VCSEC_UnsignedMessage_closureMoveRequest_tag;

    switch (closure_id) {
    case PK_CLOSURE_FRONT:
        umsg.sub_message.closureMoveRequest.frontTrunk = (VCSEC_ClosureMoveType_E)move;
        break;
    case PK_CLOSURE_REAR:
        umsg.sub_message.closureMoveRequest.rearTrunk = (VCSEC_ClosureMoveType_E)move;
        break;
    case 7:
        umsg.sub_message.closureMoveRequest.chargePort = (VCSEC_ClosureMoveType_E)move;
        break;
    default:
        return 0u;
    }
    return encode_msg(VCSEC_UnsignedMessage_fields, &umsg, out, cap);
}

size_t pk_proto_encode_vcsec_info_req(pk_proto_info_req_t req_type, uint8_t *out, size_t cap)
{
    VCSEC_UnsignedMessage umsg = VCSEC_UnsignedMessage_init_zero;
    umsg.which_sub_message = VCSEC_UnsignedMessage_InformationRequest_tag;
    umsg.sub_message.InformationRequest.informationRequestType =
        (VCSEC_InformationRequestType)req_type;
    return encode_msg(VCSEC_UnsignedMessage_fields, &umsg, out, cap);
}

pk_proto_role_t pk_proto_pair_key_role(void)
{
    /* protocol.md Roles @ 0a582da. Driver authorizes most commands except
     * other users' keys and access-control PINs. That covers lock, unlock,
     * front trunk, and rear trunk without Owner's key management. Charging
     * Manager only reads data and authorizes charging. Fleet Manager on
     * 2023.38+ cannot send BLE commands. Vehicle Monitor cannot change
     * state. Guest is a temporary Driver for rentals. Service is for
     * pairing bootstrap and roadside lock/unlock, not this enrolled key. */
    return PK_PROTO_ROLE_DRIVER;
}

size_t pk_proto_encode_vcsec_add_key(const uint8_t pubkey[65],
                                     pk_proto_role_t role,
                                     pk_proto_key_form_t form_factor,
                                     uint8_t *out, size_t cap)
{
    VCSEC_UnsignedMessage umsg = VCSEC_UnsignedMessage_init_zero;
    VCSEC_ToVCSECMessage to_msg = VCSEC_ToVCSECMessage_init_zero;
    uint8_t ubuf[160];
    size_t ulen;

    if (!pubkey || pubkey[0] != 0x04u || !out || cap == 0u) {
        return 0u;
    }
    /* S2 §2E defense-in-depth: never encode an all-zero X/Y pubkey into AddKey. */
    {
        uint8_t acc = 0u;
        for (size_t i = 1u; i < PK_PROTO_PUBKEY_LEN; ++i) {
            acc |= pubkey[i];
        }
        if (acc == 0u) {
            return 0u;
        }
    }

    umsg.which_sub_message = VCSEC_UnsignedMessage_WhitelistOperation_tag;
    umsg.sub_message.WhitelistOperation.which_sub_message =
        VCSEC_WhitelistOperation_addKeyToWhitelistAndAddPermissions_tag;
    umsg.sub_message.WhitelistOperation.sub_message.addKeyToWhitelistAndAddPermissions.has_key = true;
    umsg.sub_message.WhitelistOperation.sub_message.addKeyToWhitelistAndAddPermissions.key.PublicKeyRaw.size =
        PK_PROTO_PUBKEY_LEN;
    memcpy(umsg.sub_message.WhitelistOperation.sub_message.addKeyToWhitelistAndAddPermissions.key.PublicKeyRaw.bytes,
           pubkey, PK_PROTO_PUBKEY_LEN);
    umsg.sub_message.WhitelistOperation.sub_message.addKeyToWhitelistAndAddPermissions.keyRole =
        (Keys_Role)role;
    umsg.sub_message.WhitelistOperation.has_metadataForKey = true;
    umsg.sub_message.WhitelistOperation.metadataForKey.keyFormFactor =
        (VCSEC_KeyFormFactor)form_factor;

    ulen = encode_msg(VCSEC_UnsignedMessage_fields, &umsg, ubuf, sizeof(ubuf));
    if (ulen == 0u || ulen > sizeof(to_msg.signedMessage.protobufMessageAsBytes.bytes)) {
        return 0u;
    }

    to_msg.has_signedMessage = true;
    to_msg.signedMessage.protobufMessageAsBytes.size = (pb_size_t)ulen;
    memcpy(to_msg.signedMessage.protobufMessageAsBytes.bytes, ubuf, ulen);
    to_msg.signedMessage.signatureType = VCSEC_SignatureType_SIGNATURE_TYPE_PRESENT_KEY;

    return encode_msg(VCSEC_ToVCSECMessage_fields, &to_msg, out, cap);
}

size_t pk_proto_encode_carserver_get_data(uint32_t category_mask, uint8_t *out, size_t cap)
{
    CarServer_Action act = CarServer_Action_init_zero;
    CarServer_GetVehicleData *gd;

    act.which_action_msg = CarServer_Action_vehicleAction_tag;
    act.action_msg.vehicleAction.which_vehicle_action_msg =
        CarServer_VehicleAction_getVehicleData_tag;
    gd = &act.action_msg.vehicleAction.vehicle_action_msg.getVehicleData;

    gd->has_getChargeState = (category_mask & PK_CARSERVER_DATA_CHARGE) != 0u;
    gd->has_getClimateState = (category_mask & PK_CARSERVER_DATA_CLIMATE) != 0u;
    gd->has_getDriveState = (category_mask & PK_CARSERVER_DATA_DRIVE) != 0u;
    gd->has_getClosuresState = (category_mask & PK_CARSERVER_DATA_CLOSURES) != 0u;
    gd->has_getTirePressureState = (category_mask & PK_CARSERVER_DATA_TIRES) != 0u;

    return encode_msg(CarServer_Action_fields, &act, out, cap);
}

size_t pk_proto_encode_status_section(int section, uint8_t *out, size_t cap)
{
    static const uint32_t k_sections[PK_STATUS_SECTION_COUNT] = {
        PK_CARSERVER_DATA_CHARGE,
        PK_CARSERVER_DATA_CLIMATE,
        PK_CARSERVER_DATA_DRIVE,
        PK_CARSERVER_DATA_CLOSURES,
        PK_CARSERVER_DATA_TIRES,
    };

    if (section < 0 || section >= PK_STATUS_SECTION_COUNT) {
        return 0u;
    }
    return pk_proto_encode_carserver_get_data(k_sections[section], out, cap);
}

static size_t encode_seat_heat_action(const pk_draft_t *draft, uint8_t *out, size_t cap)
{
    static const pb_size_t k_lvl_tags[4] = {
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_OFF_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_LOW_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_MED_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_SEAT_HEATER_HIGH_tag,
    };
    static const pb_size_t k_pos_tags[5] = {
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_FRONT_LEFT_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_FRONT_RIGHT_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_REAR_LEFT_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_REAR_CENTER_tag,
        CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction_CAR_SEAT_REAR_RIGHT_tag,
    };
    CarServer_Action act = CarServer_Action_init_zero;
    CarServer_HvacSeatHeaterActions_HvacSeatHeaterAction *item;

    act.which_action_msg = CarServer_Action_vehicleAction_tag;
    act.action_msg.vehicleAction.which_vehicle_action_msg =
        CarServer_VehicleAction_hvacSeatHeaterActions_tag;
    act.action_msg.vehicleAction.vehicle_action_msg.hvacSeatHeaterActions.hvacSeatHeaterAction_count = 1u;
    item = &act.action_msg.vehicleAction.vehicle_action_msg.hvacSeatHeaterActions.hvacSeatHeaterAction[0];
    item->which_seat_heater_level = k_lvl_tags[0];
    item->which_seat_position = k_pos_tags[0];

    if (draft) {
        if (draft->seat_level >= 0 && draft->seat_level <= 3) {
            item->which_seat_heater_level = k_lvl_tags[draft->seat_level];
        }
        if (draft->seat_position >= 0 && draft->seat_position < 5) {
            item->which_seat_position = k_pos_tags[draft->seat_position];
        }
    }
    return encode_msg(CarServer_Action_fields, &act, out, cap);
}

size_t pk_proto_encode_action_payload(pk_action_t action,
                                      const pk_draft_t *draft,
                                      const uint8_t *client_pubkey65,
                                      uint8_t *out, size_t cap)
{
    CarServer_Action act = CarServer_Action_init_zero;

    if (!out || cap == 0u) {
        return 0u;
    }

    switch (action) {
    case PK_ACTION_LOCK:
        return pk_proto_encode_vcsec_rke(PK_PROTO_RKE_LOCK, out, cap);
    case PK_ACTION_UNLOCK:
        return pk_proto_encode_vcsec_rke(PK_PROTO_RKE_UNLOCK, out, cap);
    case PK_ACTION_VCSEC_WAKE_ONCE:
        return pk_proto_encode_vcsec_rke(PK_PROTO_RKE_WAKE_VEHICLE, out, cap);
    case PK_ACTION_VCSEC_GET_STATUS_ONCE:
    case PK_ACTION_PAIR_VERIFY_VCSEC:
        return pk_proto_encode_vcsec_info_req(PK_PROTO_INFO_GET_STATUS, out, cap);
    case PK_ACTION_FRUNK_OPEN:
        return pk_proto_encode_vcsec_closure(PK_CLOSURE_FRONT, PK_PROTO_CLOSURE_MOVE_MOVE, out, cap);
    case PK_ACTION_TRUNK_MOVE:
        return pk_proto_encode_vcsec_closure(PK_CLOSURE_REAR, PK_PROTO_CLOSURE_MOVE_MOVE, out, cap);
    case PK_ACTION_ADDKEY:
        return pk_proto_encode_vcsec_add_key(client_pubkey65, pk_proto_pair_key_role(),
                                             PK_PROTO_KEY_FORM_CLOUD_KEY, out, cap);
    case PK_ACTION_GET_QUALIFICATION:
    case PK_ACTION_PAIR_VERIFY_INFOTAINMENT:
        return pk_proto_encode_carserver_get_data(PK_CARSERVER_DATA_DRIVE, out, cap);
    case PK_ACTION_STATUS_READ:
        return pk_proto_encode_carserver_get_data(PK_CARSERVER_DATA_CHARGE, out, cap);
    case PK_ACTION_CHARGE_PORT_OPEN:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_chargePortDoorOpen_tag;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_CHARGE_PORT_CLOSE:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_chargePortDoorClose_tag;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_FLASH:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_vehicleControlFlashLightsAction_tag;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_HONK:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_vehicleControlHonkHornAction_tag;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_CLIMATE_ON: {
        bool on = !(draft && draft->bool_enabled == PK_CAP_NO);
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_hvacAutoAction_tag;
        act.action_msg.vehicleAction.vehicle_action_msg.hvacAutoAction.power_on = on;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    }
    case PK_ACTION_PRECONDITION_MAX_ON: {
        bool on = !(draft && draft->bool_enabled == PK_CAP_NO);
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_hvacSetPreconditioningMaxAction_tag;
        act.action_msg.vehicleAction.vehicle_action_msg.hvacSetPreconditioningMaxAction.on = on;
        act.action_msg.vehicleAction.vehicle_action_msg.hvacSetPreconditioningMaxAction.manual_override = on;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    }
    case PK_ACTION_WINDOW_VENT:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_vehicleControlWindowAction_tag;
        act.action_msg.vehicleAction.vehicle_action_msg.vehicleControlWindowAction.which_action =
            CarServer_VehicleControlWindowAction_vent_tag;
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_SET_SENTRY:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_vehicleControlSetSentryModeAction_tag;
        act.action_msg.vehicleAction.vehicle_action_msg.vehicleControlSetSentryModeAction.on =
            (draft && draft->bool_enabled == PK_CAP_YES);
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_SET_STEERING_HEAT:
        act.which_action_msg = CarServer_Action_vehicleAction_tag;
        act.action_msg.vehicleAction.which_vehicle_action_msg =
            CarServer_VehicleAction_hvacSteeringWheelHeaterAction_tag;
        act.action_msg.vehicleAction.vehicle_action_msg.hvacSteeringWheelHeaterAction.power_on =
            (draft && draft->bool_enabled == PK_CAP_YES);
        return encode_msg(CarServer_Action_fields, &act, out, cap);
    case PK_ACTION_SET_SEAT_HEAT:
        return encode_seat_heat_action(draft, out, cap);
    default:
        return 0u;
    }
}

/* ==========================================================================
 * 3. RoutableMessage Builders (official nanopb UniversalMessage_RoutableMessage)
 * ========================================================================== */

static void fill_routable_route(UniversalMessage_RoutableMessage *msg,
                                const pk_proto_route_t *route)
{
    msg->has_to_destination = true;
    msg->to_destination.which_sub_destination = UniversalMessage_Destination_domain_tag;
    msg->to_destination.sub_destination.domain = (UniversalMessage_Domain)route->domain;

    msg->has_from_destination = true;
    msg->from_destination.which_sub_destination = UniversalMessage_Destination_routing_address_tag;
    msg->from_destination.sub_destination.routing_address.size = PK_PROTO_ROUTING_ADDR_LEN;
    memcpy(msg->from_destination.sub_destination.routing_address.bytes,
           route->routing_addr, PK_PROTO_ROUTING_ADDR_LEN);

    msg->uuid.size = PK_PROTO_UUID_LEN;
    memcpy(msg->uuid.bytes, route->uuid, PK_PROTO_UUID_LEN);
}

size_t pk_proto_build_session_info_request(const pk_proto_route_t *route,
                                           const uint8_t client_pubkey[65],
                                           uint8_t *out, size_t cap)
{
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_zero;

    if (!route || !route->routing_addr || !route->uuid || !client_pubkey || !out) {
        return 0u;
    }
    fill_routable_route(&msg, route);
    msg.which_payload = UniversalMessage_RoutableMessage_session_info_request_tag;
    msg.payload.session_info_request.public_key.size = PK_PROTO_PUBKEY_LEN;
    memcpy(msg.payload.session_info_request.public_key.bytes, client_pubkey, PK_PROTO_PUBKEY_LEN);
    return encode_msg(UniversalMessage_RoutableMessage_fields, &msg, out, cap);
}

size_t pk_proto_build_unsigned_routable(const pk_proto_route_t *route,
                                        const uint8_t *inner_payload, size_t inner_len,
                                        uint8_t *out, size_t cap)
{
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_zero;

    if (!route || !route->routing_addr || !route->uuid || !out ||
        (!inner_payload && inner_len > 0u) ||
        inner_len > sizeof(msg.payload.protobuf_message_as_bytes.bytes)) {
        return 0u;
    }
    fill_routable_route(&msg, route);
    msg.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
    msg.payload.protobuf_message_as_bytes.size = (pb_size_t)inner_len;
    if (inner_len > 0u) {
        memcpy(msg.payload.protobuf_message_as_bytes.bytes, inner_payload, inner_len);
    }
    return encode_msg(UniversalMessage_RoutableMessage_fields, &msg, out, cap);
}

size_t pk_proto_build_signed_routable(const pk_proto_signed_req_t *req,
                                      pk_proto_session_t *sess,
                                      uint8_t req_id_out[17],
                                      uint8_t *out, size_t cap)
{
    UniversalMessage_RoutableMessage msg = UniversalMessage_RoutableMessage_init_zero;
    Signatures_AES_GCM_Personalized_Signature_Data *sd;
    uint8_t aad[32];
    uint8_t ciphertext[256];
    uint8_t tag[16];
    uint32_t elapsed_s;
    uint32_t expires_at;
    uint32_t next_counter;
    uint32_t flags = PK_PROTO_FLAG_ENCRYPT_RESPONSE;
    size_t written;

    if (!req || !req->route.routing_addr || !req->route.uuid || !req->vin ||
        !sess || !sess->valid || !req->client_pubkey || !req->nonce || !out ||
        (!req->inner_payload && req->inner_len > 0u) ||
        req->inner_len > sizeof(ciphertext)) {
        return 0u;
    }

    /* Official signer.go:177: refuse TX counter rollover at UINT32_MAX; never wrap to 0 */
    if (sess->counter == UINT32_MAX) {
        return 0u;
    }

    next_counter = sess->counter + 1u;
    elapsed_s = (req->now_ms >= sess->sync_local_ms)
                    ? (uint32_t)((req->now_ms - sess->sync_local_ms) / 1000u)
                    : 0u;
    expires_at = sess->clock_time + elapsed_s + (req->ttl_seconds ? req->ttl_seconds : 5u);

    if (!pk_proto_compute_cmd_metadata_hash(req->route.domain, req->vin, sess->epoch,
                                            expires_at, next_counter, flags, aad) ||
        !pk_proto_aes128_gcm_encrypt(sess->session_key, req->nonce, aad, sizeof(aad),
                                     req->inner_payload, req->inner_len, ciphertext, tag)) {
        pk_proto_zeroize(aad, sizeof(aad));
        pk_proto_zeroize(ciphertext, sizeof(ciphertext));
        pk_proto_zeroize(tag, sizeof(tag));
        return 0u;
    }

    fill_routable_route(&msg, &req->route);
    msg.which_payload = UniversalMessage_RoutableMessage_protobuf_message_as_bytes_tag;
    msg.payload.protobuf_message_as_bytes.size = (pb_size_t)req->inner_len;
    if (req->inner_len > 0u) {
        memcpy(msg.payload.protobuf_message_as_bytes.bytes, ciphertext, req->inner_len);
    }

    msg.which_sub_sigData = UniversalMessage_RoutableMessage_signature_data_tag;
    msg.sub_sigData.signature_data.has_signer_identity = true;
    msg.sub_sigData.signature_data.signer_identity.which_identity_type =
        Signatures_KeyIdentity_public_key_tag;
    msg.sub_sigData.signature_data.signer_identity.identity_type.public_key.size =
        PK_PROTO_PUBKEY_LEN;
    memcpy(msg.sub_sigData.signature_data.signer_identity.identity_type.public_key.bytes,
           req->client_pubkey, PK_PROTO_PUBKEY_LEN);

    msg.sub_sigData.signature_data.which_sig_type =
        Signatures_SignatureData_AES_GCM_Personalized_data_tag;
    sd = &msg.sub_sigData.signature_data.sig_type.AES_GCM_Personalized_data;
    memcpy(sd->epoch, sess->epoch, PK_PROTO_EPOCH_LEN);
    memcpy(sd->nonce, req->nonce, PK_PROTO_NONCE_LEN);
    sd->counter = next_counter;
    sd->expires_at = expires_at;
    memcpy(sd->tag, tag, PK_PROTO_TAG_LEN);

    msg.flags = flags;

    written = encode_msg(UniversalMessage_RoutableMessage_fields, &msg, out, cap);
    if (written > 0u) {
        /* Commit counter and output req_id ONLY after encrypt + encode succeed */
        sess->counter = next_counter;
        if (req_id_out) {
            req_id_out[0] = (uint8_t)Signatures_SignatureType_SIGNATURE_TYPE_AES_GCM_PERSONALIZED;
            memcpy(req_id_out + 1, tag, PK_PROTO_TAG_LEN);
        }
    }
    pk_proto_zeroize(aad, sizeof(aad));
    pk_proto_zeroize(ciphertext, sizeof(ciphertext));
    pk_proto_zeroize(tag, sizeof(tag));
    return written;
}

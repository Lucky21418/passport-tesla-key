#include "pk_proto_internal.h"

#include <string.h>

/* ==========================================================================
 * 1. VIN -> BLE Local Name & Bounded Streaming Wire Framing (D05)
 *    NOTE: All handwritten protobuf wire codecs have been removed in S2;
 *    protobuf encode/decode lives exclusively in official nanopb generated
 *    descriptors (components/tesla_key/proto/generated/).
 * ========================================================================== */

bool pk_proto_vin_to_ble_name(const char *vin, char out[PK_PROTO_BLE_NAME_BUF])
{
    static const char hex[] = "0123456789abcdef";
    char clean_vin[PK_VIN_BUF];
    uint8_t digest[20];
    unsigned i;

    if (!vin || !out || !pk_vin_valid(vin, clean_vin)) {
        return false;
    }
    if (!pk_proto_sha1((const uint8_t *)clean_vin, PK_VIN_LEN, digest)) {
        return false;
    }
    out[0] = 'S';
    for (i = 0; i < 8; i++) {
        out[1 + i * 2] = hex[(digest[i] >> 4) & 0x0Fu];
        out[2 + i * 2] = hex[digest[i] & 0x0Fu];
    }
    out[17] = 'C';
    out[18] = '\0';
    return true;
}

static void rx_framer_reset_partial(pk_rx_framer_t *f)
{
    f->expected_len = 0u;
    f->received_len = 0u;
    f->len_received = 0u;
    f->active = false;
}

void pk_rx_framer_init(pk_rx_framer_t *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

int pk_rx_framer_feed_ex(pk_rx_framer_t *f, const uint8_t *chunk, size_t chunk_len,
                         pk_ms_t now_ms, size_t *consumed_out)
{
    size_t i = 0u;

    if (consumed_out) {
        *consumed_out = 0u;
    }
    if (!f || (!chunk && chunk_len > 0u)) {
        return -1;
    }
    if (chunk_len == 0u) {
        return 0;
    }
    if (f->active && (uint32_t)(now_ms - f->last_chunk_ms) > PK_PROTO_RX_TIMEOUT_MS) {
        /* 1000 ms inter-chunk timeout resets partial frame buffer per protocol.md */
        rx_framer_reset_partial(f);
        f->err_timeout++;
    }
    f->last_chunk_ms = now_ms;
    f->active = true;

    while (i < chunk_len && f->len_received < 2u) {
        f->len_bytes[f->len_received++] = chunk[i++];
        if (f->len_received == 2u) {
            f->expected_len = ((size_t)f->len_bytes[0] << 8) | (size_t)f->len_bytes[1];
            f->received_len = 0u;
            if (f->expected_len == 0u) {
                f->err_zero_len++;
                rx_framer_reset_partial(f);
                if (consumed_out) {
                    *consumed_out = i;
                }
                return -1;
            }
            if (f->expected_len > PK_PROTO_MAX_FRAME_LEN) {
                f->err_oversize++;
                rx_framer_reset_partial(f);
                if (consumed_out) {
                    *consumed_out = i;
                }
                return -1;
            }
        }
    }

    while (i < chunk_len && f->received_len < f->expected_len) {
        f->buf[f->received_len++] = chunk[i++];
    }

    if (consumed_out) {
        *consumed_out = i;
    }

    if (f->len_received == 2u && f->received_len == f->expected_len) {
        f->active = false;
        f->len_received = 0u;
        return 1;
    }
    return 0;
}

int pk_rx_framer_feed(pk_rx_framer_t *f, const uint8_t *chunk, size_t chunk_len, pk_ms_t now_ms)
{
    return pk_rx_framer_feed_ex(f, chunk, chunk_len, now_ms, NULL);
}

size_t pk_proto_frame_wrap(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t out_cap)
{
    if (!msg || !out || msg_len == 0u || msg_len > PK_PROTO_MAX_FRAME_LEN ||
        out_cap < msg_len + 2u) {
        return 0u;
    }
    out[0] = (uint8_t)((msg_len >> 8) & 0xFFu);
    out[1] = (uint8_t)(msg_len & 0xFFu);
    memcpy(out + 2, msg, msg_len);
    return msg_len + 2u;
}

/* ==========================================================================
 * 2. Bounded GATT Fragmented TX & Completion-Token State Machine (N06)
 * ========================================================================== */

void pk_gatt_tx_init(pk_gatt_tx_t *tx, uint32_t conn_gen, uint16_t negotiated_mtu)
{
    if (!tx) {
        return;
    }
    memset(tx, 0, sizeof(*tx));
    tx->conn_gen = conn_gen;
    tx->att_mtu = (negotiated_mtu >= 23u) ? negotiated_mtu : 23u;
    tx->last_outcome = PK_TX_NOT_SENT;
}

void pk_gatt_tx_set_mtu(pk_gatt_tx_t *tx, uint16_t negotiated_mtu)
{
    if (!tx) {
        return;
    }
    tx->att_mtu = (negotiated_mtu >= 23u) ? negotiated_mtu : 23u;
}

uint16_t pk_gatt_tx_chunk_size(const pk_gatt_tx_t *tx)
{
    uint16_t mtu;
    uint16_t chunk;
    if (!tx) {
        return 20u;
    }
    mtu = (tx->att_mtu >= 23u) ? tx->att_mtu : 23u;
    chunk = (uint16_t)(mtu - 3u);
    if (chunk < 20u) {
        chunk = 20u;
    }
    if (chunk > 244u) {
        chunk = 244u;
    }
    return chunk;
}

pk_ms_t pk_gatt_tx_remaining_ms(const pk_gatt_tx_t *tx, pk_ms_t now_ms)
{
    if (!tx || !tx->in_flight || pk_time_due(now_ms, tx->deadline_ms)) {
        return 0u;
    }
    return (pk_ms_t)(tx->deadline_ms - now_ms);
}

bool pk_gatt_tx_begin(pk_gatt_tx_t *tx, size_t total_bytes, pk_ms_t now_ms, pk_ms_t deadline_ms)
{
    if (!tx || total_bytes == 0u || total_bytes > PK_PROTO_MAX_FRAME_LEN + 2u ||
        pk_time_due(now_ms, deadline_ms) || tx->in_flight) {
        return false;
    }
    tx->write_seq++;
    tx->in_flight = true;
    tx->deadline_ms = deadline_ms;
    tx->total_bytes = total_bytes;
    tx->bytes_sent = 0u;
    tx->chunks_sent = 0u;
    tx->last_outcome = PK_TX_NOT_SENT;
    return true;
}

pk_err_t pk_gatt_tx_step(pk_gatt_tx_t *tx, pk_ms_t now_ms, bool link_up,
                         int transport_write_rc, size_t *chunk_len_out)
{
    uint16_t max_chunk;
    size_t rem;
    size_t n;

    if (chunk_len_out) {
        *chunk_len_out = 0u;
    }
    if (!tx || !tx->in_flight) {
        return PK_ERR_INVALID;
    }
    if (pk_time_due(now_ms, tx->deadline_ms)) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
        return PK_ERR_EXPIRED;
    }
    if (!link_up) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
        return PK_ERR_OFFLINE;
    }
    max_chunk = pk_gatt_tx_chunk_size(tx);
    rem = tx->total_bytes - tx->bytes_sent;
    n = (rem < (size_t)max_chunk) ? rem : (size_t)max_chunk;
    if (chunk_len_out) {
        *chunk_len_out = n;
    }
    if (transport_write_rc != 0) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
        return PK_ERR_UNKNOWN;
    }
    tx->bytes_sent += n;
    tx->chunks_sent++;
    /* Once any byte is handed to BLE GATT, outcome is at least SentUnknown until confirmed. */
    tx->last_outcome = PK_TX_SENT_UNKNOWN;
    return PK_OK;
}

pk_err_t pk_gatt_tx_enqueue_cb(pk_gatt_tx_t *tx, uint32_t conn_gen, uint32_t write_seq, int att_status)
{
    uint8_t tail;
    if (!tx) {
        return PK_ERR_INVALID;
    }
    if (tx->cb_count >= PK_GATT_CB_QUEUE_CAP) {
        tx->cb_overflow_drops++;
        return PK_ERR_BUSY;
    }
    tail = (uint8_t)((tx->cb_head + tx->cb_count) % PK_GATT_CB_QUEUE_CAP);
    tx->cb_queue[tail].conn_gen = conn_gen;
    tx->cb_queue[tail].write_seq = write_seq;
    tx->cb_queue[tail].att_status = att_status;
    tx->cb_count++;
    return PK_OK;
}

pk_err_t pk_gatt_tx_poll_cb(pk_gatt_tx_t *tx, pk_ms_t now_ms, bool *matched_out)
{
    pk_gatt_cb_ev_t ev;

    if (matched_out) {
        *matched_out = false;
    }
    if (!tx) {
        return PK_ERR_INVALID;
    }
    if (tx->in_flight && pk_time_due(now_ms, tx->deadline_ms)) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
        return PK_ERR_EXPIRED;
    }
    if (tx->cb_count == 0u) {
        return PK_OK;
    }
    ev = tx->cb_queue[tx->cb_head];
    tx->cb_head = (uint8_t)((tx->cb_head + 1u) % PK_GATT_CB_QUEUE_CAP);
    tx->cb_count--;

    if (!tx->in_flight || ev.conn_gen != tx->conn_gen || ev.write_seq != tx->write_seq) {
        return PK_ERR_MISMATCH;
    }
    if (matched_out) {
        *matched_out = true;
    }
    if (ev.att_status != 0) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
        return PK_ERR_UNKNOWN;
    }
    if (tx->bytes_sent >= tx->total_bytes) {
        tx->in_flight = false;
        tx->last_outcome = PK_TX_DELIVERED;
    }
    return PK_OK;
}

void pk_gatt_tx_on_disconnect(pk_gatt_tx_t *tx, uint32_t new_conn_gen)
{
    if (!tx) {
        return;
    }
    if (tx->in_flight) {
        tx->in_flight = false;
        tx->last_outcome = (tx->bytes_sent > 0u) ? PK_TX_SENT_UNKNOWN : PK_TX_NOT_SENT;
    }
    tx->conn_gen = new_conn_gen;
    tx->cb_head = 0u;
    tx->cb_count = 0u;
}

/* ==========================================================================
 * 3. Official protocol.md Metadata TLV Serializer (NOT protobuf)
 * ========================================================================== */

void pk_meta_tlv_bytes(pk_meta_tlv_t *m, uint8_t tag, const void *data, size_t len)
{
    if (m->err || (!data && len > 0u) || len > 0xFFu ||
        m->pos + 2u > m->cap || len > m->cap - m->pos - 2u) {
        m->err = true;
        return;
    }
    m->buf[m->pos++] = tag;
    m->buf[m->pos++] = (uint8_t)len;
    if (len > 0u) {
        memcpy(m->buf + m->pos, data, len);
        m->pos += len;
    }
}

void pk_meta_tlv_raw(pk_meta_tlv_t *m, const void *data, size_t len)
{
    if (m->err || (!data && len > 0u) || len > m->cap - m->pos) {
        m->err = true;
        return;
    }
    if (len > 0u) {
        memcpy(m->buf + m->pos, data, len);
        m->pos += len;
    }
}

void pk_meta_tlv_u8(pk_meta_tlv_t *m, uint8_t tag, uint8_t val)
{
    pk_meta_tlv_bytes(m, tag, &val, 1u);
}

void pk_meta_tlv_be32(pk_meta_tlv_t *m, uint8_t tag, uint32_t val)
{
    uint8_t be[4];
    pk_store_be32(be, val);
    pk_meta_tlv_bytes(m, tag, be, sizeof(be));
}

bool pk_meta_tlv_begin(pk_meta_tlv_t *m, uint8_t *buf, size_t cap,
                       uint8_t sig_type, int domain_or_neg1, const char *vin)
{
    if (!m || !buf || cap == 0u || !vin || strlen(vin) != PK_VIN_LEN) {
        if (m) {
            m->err = true;
        }
        return false;
    }
    m->buf = buf;
    m->cap = cap;
    m->pos = 0u;
    m->err = false;
    pk_meta_tlv_u8(m, 0x00u, sig_type);
    if (domain_or_neg1 >= 0) {
        pk_meta_tlv_u8(m, 0x01u, (uint8_t)domain_or_neg1);
    }
    pk_meta_tlv_bytes(m, 0x02u, vin, PK_VIN_LEN);
    return !m->err;
}

bool pk_meta_tlv_end(pk_meta_tlv_t *m)
{
    if (m->err || m->pos >= m->cap) {
        m->err = true;
        return false;
    }
    m->buf[m->pos++] = 0xFFu;
    return true;
}

bool pk_meta_tlv_finish_sha256(pk_meta_tlv_t *m, uint8_t aad_out[32])
{
    if (!aad_out || !pk_meta_tlv_end(m)) {
        return false;
    }
    return pk_proto_sha256(m->buf, m->pos, aad_out);
}

bool pk_proto_compute_cmd_metadata_hash(pk_proto_domain_t domain,
                                        const char *vin,
                                        const uint8_t epoch[16],
                                        uint32_t expires_at,
                                        uint32_t counter,
                                        uint32_t flags,
                                        uint8_t aad_out[32])
{
    uint8_t tlv[96];
    pk_meta_tlv_t m;

    if (!epoch || !aad_out || !pk_meta_tlv_begin(&m, tlv, sizeof(tlv), 0x05u, (int)domain, vin)) {
        return false;
    }
    pk_meta_tlv_bytes(&m, 0x03u, epoch, 16u);
    pk_meta_tlv_be32(&m, 0x04u, expires_at);
    pk_meta_tlv_be32(&m, 0x05u, counter);
    if (flags > 0u) {
        pk_meta_tlv_be32(&m, 0x07u, flags);
    }
    return pk_meta_tlv_finish_sha256(&m, aad_out);
}

bool pk_proto_compute_resp_metadata_hash(pk_proto_domain_t from_domain,
                                         const char *vin,
                                         uint32_t counter,
                                         uint32_t flags,
                                         const uint8_t req_id[17],
                                         uint32_t fault,
                                         uint8_t aad_out[32])
{
    uint8_t tlv[112];
    pk_meta_tlv_t m;

    if (!req_id || !aad_out || !pk_meta_tlv_begin(&m, tlv, sizeof(tlv), 0x09u, (int)from_domain, vin)) {
        return false;
    }
    pk_meta_tlv_be32(&m, 0x05u, counter);
    /* protocol.md: Flags is ALWAYS included in response metadata */
    pk_meta_tlv_be32(&m, 0x07u, flags);
    pk_meta_tlv_bytes(&m, 0x08u, req_id, 17u);
    pk_meta_tlv_be32(&m, 0x09u, fault);
    return pk_meta_tlv_finish_sha256(&m, aad_out);
}

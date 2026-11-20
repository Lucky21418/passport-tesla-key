#ifndef PK_PROTO_INTERNAL_H
#define PK_PROTO_INTERNAL_H

#include "pk_protocol.h"
#include "pk_app_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Internal helpers shared by pk_crypto.c, pk_pb.c (framing + metadata TLV),
 * pk_proto_tx.c, and pk_proto_rx.c.
 *
 * NOTE: All handwritten protobuf wire encoder/decoder code (pk_pb_w_*,
 * pk_pb_r_*, pk_pb_decode_fields) has been removed in S2; every protobuf
 * message uses official nanopb-generated descriptors in proto/generated/.
 */

static inline uint32_t pk_load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void pk_store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Official protocol.md Metadata TLV serializer (NOT protobuf) */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     err;
} pk_meta_tlv_t;

void pk_meta_tlv_bytes(pk_meta_tlv_t *m, uint8_t tag, const void *data, size_t len);
void pk_meta_tlv_raw(pk_meta_tlv_t *m, const void *data, size_t len);
void pk_meta_tlv_u8(pk_meta_tlv_t *m, uint8_t tag, uint8_t val);
void pk_meta_tlv_be32(pk_meta_tlv_t *m, uint8_t tag, uint32_t val);
bool pk_meta_tlv_begin(pk_meta_tlv_t *m, uint8_t *buf, size_t cap,
                       uint8_t sig_type, int domain_or_neg1, const char *vin);
bool pk_meta_tlv_end(pk_meta_tlv_t *m);
bool pk_meta_tlv_finish_sha256(pk_meta_tlv_t *m, uint8_t aad_out[32]);

bool pk_proto_compute_cmd_metadata_hash(pk_proto_domain_t domain,
                                        const char *vin,
                                        const uint8_t epoch[16],
                                        uint32_t expires_at,
                                        uint32_t counter,
                                        uint32_t flags,
                                        uint8_t aad_out[32]);

bool pk_proto_compute_resp_metadata_hash(pk_proto_domain_t from_domain,
                                         const char *vin,
                                         uint32_t counter,
                                         uint32_t flags,
                                         const uint8_t req_id[17],
                                         uint32_t fault,
                                         uint8_t aad_out[32]);

#endif /* PK_PROTO_INTERNAL_H */

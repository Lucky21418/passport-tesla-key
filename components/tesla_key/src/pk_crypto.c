#include "pk_proto_internal.h"

#include <string.h>
#include <stdio.h>

#if defined(ESP_PLATFORM) || defined(PK_USE_MBEDTLS)
#include "mbedtls/constant_time.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

/*
 * Production entropy & CSPRNG gate:
 *   - On ESP_PLATFORM (ESP-IDF 5.5.3): esp_random.h guarantees true hardware
 *     entropy ONLY when Wi-Fi or Bluetooth RF is active. s_entropy_mask tracks
 *     active RF subsystems; if 0, pk_proto_random_bytes() fails closed.
 *     When active, entropy flows through ESP-IDF's mbedtls_entropy_func
 *     (mbedtls_hardware_poll -> esp_fill_random) into CTR-DRBG.
 *   - On host (PK_USE_MBEDTLS): OS entropy (/dev/urandom via entropy_poll.c)
 *     seeds CTR-DRBG. Optional fault-injection exists ONLY when compiled with
 *     PK_HOST_TEST_RNG_INJECT (never defined on ESP_PLATFORM).
 */
static uint32_t s_entropy_mask =
#if defined(ESP_PLATFORM)
    0u;
#else
    PK_ENTROPY_SRC_BLE;
#endif

static bool s_drbg_ready = false;
static mbedtls_entropy_context s_entropy_ctx;
static mbedtls_ctr_drbg_context s_drbg_ctx;

#if !defined(ESP_PLATFORM) && defined(PK_HOST_TEST_RNG_INJECT)
static bool s_test_rng_fail = false;
void pk_test_set_rng_fail(bool fail) { s_test_rng_fail = fail; }
#endif

void pk_proto_zeroize(void *buf, size_t len)
{
    if (buf && len > 0u) {
        mbedtls_platform_zeroize(buf, len);
    }
}

bool pk_proto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    if (!a || !b) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    return mbedtls_ct_memcmp(a, b, len) == 0;
}

void pk_proto_set_entropy_source(uint32_t src_mask, bool active)
{
    if (active) {
        s_entropy_mask |= src_mask;
    } else {
        s_entropy_mask &= ~src_mask;
        if (s_entropy_mask == 0u && s_drbg_ready) {
            mbedtls_ctr_drbg_free(&s_drbg_ctx);
            mbedtls_entropy_free(&s_entropy_ctx);
            s_drbg_ready = false;
        }
    }
}

bool pk_proto_entropy_ready(void)
{
#if !defined(ESP_PLATFORM) && defined(PK_HOST_TEST_RNG_INJECT)
    if (s_test_rng_fail) {
        return false;
    }
#endif
    return s_entropy_mask != 0u;
}

static bool ensure_drbg_seeded(void)
{
    static const unsigned char pers[] = "pk_tesla_s2_drbg";
    if (!pk_proto_entropy_ready()) {
        return false;
    }
    if (s_drbg_ready) {
        return true;
    }
    mbedtls_entropy_init(&s_entropy_ctx);
    mbedtls_ctr_drbg_init(&s_drbg_ctx);
    if (mbedtls_ctr_drbg_seed(&s_drbg_ctx, mbedtls_entropy_func, &s_entropy_ctx,
                              pers, sizeof(pers) - 1u) != 0) {
        mbedtls_ctr_drbg_free(&s_drbg_ctx);
        mbedtls_entropy_free(&s_entropy_ctx);
        return false;
    }
    s_drbg_ready = true;
    return true;
}

bool pk_proto_random_bytes(uint8_t *out, size_t len)
{
    if (!out && len > 0u) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    if (!ensure_drbg_seeded()) {
        pk_proto_zeroize(out, len);
        return false;
    }
    if (mbedtls_ctr_drbg_random(&s_drbg_ctx, out, len) != 0) {
        pk_proto_zeroize(out, len);
        return false;
    }
    return true;
}

static int pk_mbedtls_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    return pk_proto_random_bytes(buf, len) ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}

bool pk_proto_sha1(const uint8_t *data, size_t len, uint8_t out20[20])
{
    if (!out20 || (!data && len > 0u)) {
        return false;
    }
    if (mbedtls_sha1(data, len, out20) != 0) {
        pk_proto_zeroize(out20, 20u);
        return false;
    }
    return true;
}

bool pk_proto_sha256(const uint8_t *data, size_t len, uint8_t out32[32])
{
    if (!out32 || (!data && len > 0u)) {
        return false;
    }
    if (mbedtls_sha256(data, len, out32, 0) != 0) {
        pk_proto_zeroize(out32, 32u);
        return false;
    }
    return true;
}

bool pk_proto_hmac_sha256(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[32])
{
    const mbedtls_md_info_t *info;
    if (!out || (!key && key_len > 0u) || (!data && data_len > 0u)) {
        return false;
    }
    info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_hmac(info, key, key_len, data, data_len, out) != 0) {
        pk_proto_zeroize(out, 32u);
        return false;
    }
    return true;
}

static struct {
    bool                valid;
    uint8_t             key[16];
    mbedtls_gcm_context ctx;
} s_gcm_cache;

void pk_proto_aes128_gcm_clear_cache(void)
{
    if (s_gcm_cache.valid) {
        mbedtls_gcm_free(&s_gcm_cache.ctx);
        s_gcm_cache.valid = false;
    }
    mbedtls_platform_zeroize(s_gcm_cache.key, sizeof(s_gcm_cache.key));
}

static mbedtls_gcm_context *gcm_ctx_for_key(const uint8_t key[16])
{
    if (s_gcm_cache.valid && pk_proto_ct_equal(s_gcm_cache.key, key, 16u)) {
        return &s_gcm_cache.ctx;
    }
    pk_proto_aes128_gcm_clear_cache();
    mbedtls_gcm_init(&s_gcm_cache.ctx);
    if (mbedtls_gcm_setkey(&s_gcm_cache.ctx, MBEDTLS_CIPHER_ID_AES, key, 128u) != 0) {
        mbedtls_gcm_free(&s_gcm_cache.ctx);
        return NULL;
    }
    memcpy(s_gcm_cache.key, key, 16u);
    s_gcm_cache.valid = true;
    return &s_gcm_cache.ctx;
}

bool pk_proto_aes128_gcm_encrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plaintext, size_t len,
                                 uint8_t *ciphertext,
                                 uint8_t tag[16])
{
    mbedtls_gcm_context *ctx;
    int ret;

    if (!key || !iv || !tag || (len > 0u && (!plaintext || !ciphertext))) {
        return false;
    }
    ctx = gcm_ctx_for_key(key);
    if (!ctx) {
        if (ciphertext && len > 0u) {
            pk_proto_zeroize(ciphertext, len);
        }
        pk_proto_zeroize(tag, 16u);
        return false;
    }
    ret = mbedtls_gcm_crypt_and_tag(ctx, MBEDTLS_GCM_ENCRYPT, len,
                                    iv, 12u, aad, aad_len,
                                    plaintext, ciphertext, 16u, tag);
    if (ret != 0) {
        if (ciphertext && len > 0u) {
            pk_proto_zeroize(ciphertext, len);
        }
        pk_proto_zeroize(tag, 16u);
        return false;
    }
    return true;
}

bool pk_proto_aes128_gcm_decrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *ciphertext, size_t len,
                                 const uint8_t tag[16],
                                 uint8_t *plaintext)
{
    mbedtls_gcm_context *ctx;
    int ret;

    if (!key || !iv || !tag || (len > 0u && (!ciphertext || !plaintext))) {
        return false;
    }
    ctx = gcm_ctx_for_key(key);
    if (!ctx) {
        if (plaintext && len > 0u) {
            pk_proto_zeroize(plaintext, len);
        }
        return false;
    }
    ret = mbedtls_gcm_auth_decrypt(ctx, len, iv, 12u, aad, aad_len,
                                   tag, 16u, ciphertext, plaintext);
    if (ret != 0) {
        if (plaintext && len > 0u) {
            pk_proto_zeroize(plaintext, len);
        }
        return false;
    }
    return true;
}

#else

/* Portable host fallback for standalone CI builds without Mbed TLS installed.
 * Production ESP32-C3 builds (ESP_PLATFORM) and S2 security tests (PK_USE_MBEDTLS)
 * ALWAYS use real Mbed TLS 3.6.5 above. */
static uint32_t s_entropy_mask = PK_ENTROPY_SRC_BLE;

#if defined(PK_HOST_TEST_RNG_INJECT)
static bool s_test_rng_fail = false;
void pk_test_set_rng_fail(bool fail) { s_test_rng_fail = fail; }
#endif

void pk_proto_zeroize(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    if (!p) {
        return;
    }
    while (len--) {
        *p++ = 0u;
    }
}

bool pk_proto_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0u;
    size_t i;
    if (!a || !b) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}

void pk_proto_set_entropy_source(uint32_t src_mask, bool active)
{
    if (active) {
        s_entropy_mask |= src_mask;
    } else {
        s_entropy_mask &= ~src_mask;
    }
}

bool pk_proto_entropy_ready(void)
{
#if defined(PK_HOST_TEST_RNG_INJECT)
    if (s_test_rng_fail) {
        return false;
    }
#endif
    return s_entropy_mask != 0u;
}

bool pk_proto_random_bytes(uint8_t *out, size_t len)
{
    FILE *fp;
    size_t got;

    if (!out && len > 0u) {
        return false;
    }
    if (len == 0u) {
        return true;
    }
    if (!pk_proto_entropy_ready()) {
        pk_proto_zeroize(out, len);
        return false;
    }
    fp = fopen("/dev/urandom", "rb");
    if (!fp) {
        pk_proto_zeroize(out, len);
        return false;
    }
    got = fread(out, 1u, len, fp);
    fclose(fp);
    if (got != len) {
        pk_proto_zeroize(out, len);
        return false;
    }
    return true;
}

/* ==========================================================================
 * 1. SHA-1 (RFC 3174) — used by Tesla BLE for:
 *    - Advertised local name: "S" + hex(SHA1(VIN)[0..7]) + "C"
 *    - ECDH session key derivation: K = SHA1(shared_x)[0..15]
 * ========================================================================== */

static uint32_t rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}


static void sha1_compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    unsigned i;

    for (i = 0; i < 16; i++) {
        w[i] = pk_load_be32(block + i * 4);
    }
    for (i = 16; i < 80; i++) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k, temp;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

bool pk_proto_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t state[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };
    uint8_t block[64];
    size_t offset = 0;
    uint64_t bit_len = (uint64_t)len * 8u;
    unsigned i;

    if (!out || (!data && len > 0u)) {
        return false;
    }

    while (len - offset >= 64) {
        sha1_compress(state, data + offset);
        offset += 64;
    }

    memset(block, 0, sizeof(block));
    if (len > offset) {
        memcpy(block, data + offset, len - offset);
    }
    block[len - offset] = 0x80u;

    if (len - offset >= 56) {
        sha1_compress(state, block);
        memset(block, 0, sizeof(block));
    }

    pk_store_be32(block + 56, (uint32_t)(bit_len >> 32));
    pk_store_be32(block + 60, (uint32_t)(bit_len & 0xFFFFFFFFu));
    sha1_compress(state, block);

    for (i = 0; i < 5; i++) {
        pk_store_be32(out + i * 4, state[i]);
    }
    return true;
}

/* ==========================================================================
 * 2. SHA-256 & HMAC-SHA256 (FIPS 180-4 / RFC 2104)
 * ========================================================================== */

static const uint32_t k_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcU, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffaU, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

typedef struct {
    uint32_t state[8];
    uint8_t buf[64];
    size_t buf_len;
    uint64_t total_len;
} sha256_ctx_t;

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; i++) {
        w[i] = pk_load_be32(block + i * 4);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->buf_len = 0;
    ctx->total_len = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    if (len == 0 || !data) {
        return;
    }
    ctx->total_len += len;
    if (ctx->buf_len > 0) {
        while (i < len && ctx->buf_len < 64) {
            ctx->buf[ctx->buf_len++] = data[i++];
        }
        if (ctx->buf_len == 64) {
            sha256_compress(ctx->state, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (len - i >= 64) {
        sha256_compress(ctx->state, data + i);
        i += 64;
    }
    while (i < len) {
        ctx->buf[ctx->buf_len++] = data[i++];
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint64_t bit_len = ctx->total_len * 8u;
    unsigned i;

    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) {
            ctx->buf[ctx->buf_len++] = 0;
        }
        sha256_compress(ctx->state, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) {
        ctx->buf[ctx->buf_len++] = 0;
    }
    pk_store_be32(ctx->buf + 56, (uint32_t)(bit_len >> 32));
    pk_store_be32(ctx->buf + 60, (uint32_t)(bit_len & 0xFFFFFFFFu));
    sha256_compress(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) {
        pk_store_be32(out + i * 4, ctx->state[i]);
    }
}

bool pk_proto_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t ctx;
    if (!out || (!data && len > 0u)) {
        return false;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
    return true;
}

bool pk_proto_hmac_sha256(const uint8_t *key, size_t key_len,
                          const uint8_t *data, size_t data_len,
                          uint8_t out[32])
{
    uint8_t k_pad[64];
    uint8_t tk[32];
    uint8_t inner[32];
    sha256_ctx_t ctx;
    unsigned i;

    if (!out || (!key && key_len > 0u) || (!data && data_len > 0u)) {
        return false;
    }

    if (key_len > 64) {
        (void)pk_proto_sha256(key, key_len, tk);
        key = tk;
        key_len = 32;
    }

    memset(k_pad, 0, sizeof(k_pad));
    if (key && key_len > 0) {
        memcpy(k_pad, key, key_len);
    }
    for (i = 0; i < 64; i++) {
        k_pad[i] ^= 0x36u;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    memset(k_pad, 0, sizeof(k_pad));
    if (key && key_len > 0) {
        memcpy(k_pad, key, key_len);
    }
    for (i = 0; i < 64; i++) {
        k_pad[i] ^= 0x5cu;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, k_pad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
    pk_proto_zeroize(k_pad, sizeof(k_pad));
    pk_proto_zeroize(tk, sizeof(tk));
    pk_proto_zeroize(inner, sizeof(inner));
    return true;
}

/* ==========================================================================
 * 3. AES-128-GCM (FIPS 197 + NIST SP 800-38D)
 * ========================================================================== */

static const uint8_t k_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t aes_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1u) * 0x1bu));
}

static void aes128_expand_key(const uint8_t key[16], uint8_t rk[176])
{
    static const uint8_t rcon[10] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
    };
    unsigned i;
    uint8_t temp[4];

    memcpy(rk, key, 16);
    for (i = 4; i < 44; i++) {
        memcpy(temp, rk + (i - 1) * 4, 4);
        if (i % 4 == 0) {
            uint8_t t = temp[0];
            temp[0] = k_aes_sbox[temp[1]] ^ rcon[(i / 4) - 1];
            temp[1] = k_aes_sbox[temp[2]];
            temp[2] = k_aes_sbox[temp[3]];
            temp[3] = k_aes_sbox[t];
        }
        rk[i * 4 + 0] = rk[(i - 4) * 4 + 0] ^ temp[0];
        rk[i * 4 + 1] = rk[(i - 4) * 4 + 1] ^ temp[1];
        rk[i * 4 + 2] = rk[(i - 4) * 4 + 2] ^ temp[2];
        rk[i * 4 + 3] = rk[(i - 4) * 4 + 3] ^ temp[3];
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    unsigned r, i;

    memcpy(s, in, 16);
    for (i = 0; i < 16; i++) {
        s[i] ^= rk[i];
    }

    for (r = 1; r <= 10; r++) {
        uint8_t t;
        for (i = 0; i < 16; i++) {
            s[i] = k_aes_sbox[s[i]];
        }
        /* ShiftRows */
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t;
        t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;

        if (r < 10) {
            for (i = 0; i < 4; i++) {
                uint8_t *c = s + i * 4;
                uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                uint8_t tm = a0 ^ a1 ^ a2 ^ a3;
                c[0] ^= tm ^ aes_xtime(a0 ^ a1);
                c[1] ^= tm ^ aes_xtime(a1 ^ a2);
                c[2] ^= tm ^ aes_xtime(a2 ^ a3);
                c[3] ^= tm ^ aes_xtime(a3 ^ a0);
            }
        }
        for (i = 0; i < 16; i++) {
            s[i] ^= rk[r * 16 + i];
        }
    }
    memcpy(out, s, 16);
}

/* GF(2^128) multiplication for GHASH: Z = X * H */
static void ghash_mul(uint8_t x[16], const uint8_t h[16])
{
    uint8_t z[16];
    uint8_t v[16];
    unsigned i, j;

    memset(z, 0, 16);
    memcpy(v, h, 16);

    for (i = 0; i < 16; i++) {
        uint8_t xi = x[i];
        for (j = 0; j < 8; j++) {
            uint8_t bit = (uint8_t)((xi >> (7 - j)) & 1u);
            uint8_t lsb = (uint8_t)(v[15] & 1u);
            unsigned k;
            if (bit) {
                for (k = 0; k < 16; k++) {
                    z[k] ^= v[k];
                }
            }
            for (k = 15; k > 0; k--) {
                v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
            }
            v[0] >>= 1;
            if (lsb) {
                v[0] ^= 0xe1u;
            }
        }
    }
    memcpy(x, z, 16);
}

static void ghash_update(uint8_t y[16], const uint8_t h[16], const uint8_t *data, size_t len)
{
    size_t offset = 0;
    unsigned i;

    while (len - offset >= 16) {
        for (i = 0; i < 16; i++) {
            y[i] ^= data[offset + i];
        }
        ghash_mul(y, h);
        offset += 16;
    }
    if (len > offset) {
        for (i = 0; i < len - offset; i++) {
            y[i] ^= data[offset + i];
        }
        ghash_mul(y, h);
    }
}

static void aes128_gcm_crypt_and_tag(const uint8_t key[16],
                                     const uint8_t iv[12],
                                     const uint8_t *aad, size_t aad_len,
                                     const uint8_t *in, size_t len,
                                     bool encrypt,
                                     uint8_t *out,
                                     uint8_t tag_out[16])
{
    uint8_t rk[176];
    uint8_t h[16];
    uint8_t j0[16];
    uint8_t cb[16];
    uint8_t ks[16];
    uint8_t y[16];
    uint8_t len_block[16];
    uint32_t ctr;
    size_t offset = 0;
    unsigned i;

    aes128_expand_key(key, rk);
    memset(h, 0, 16);
    aes128_encrypt_block(rk, h, h);

    memcpy(j0, iv, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;

    memset(y, 0, 16);
    if (aad && aad_len > 0) {
        ghash_update(y, h, aad, aad_len);
    }

    if (!encrypt && in && len > 0) {
        ghash_update(y, h, in, len);
    }

    ctr = 2;
    memcpy(cb, iv, 12);
    while (offset < len) {
        size_t chunk = (len - offset > 16) ? 16 : (len - offset);
        pk_store_be32(cb + 12, ctr++);
        aes128_encrypt_block(rk, cb, ks);
        for (i = 0; i < chunk; i++) {
            out[offset + i] = in[offset + i] ^ ks[i];
        }
        offset += chunk;
    }

    if (encrypt && out && len > 0) {
        ghash_update(y, h, out, len);
    }

    memset(len_block, 0, 16);
    pk_store_be32(len_block + 0, (uint32_t)(((uint64_t)aad_len * 8u) >> 32));
    pk_store_be32(len_block + 4, (uint32_t)(((uint64_t)aad_len * 8u) & 0xFFFFFFFFu));
    pk_store_be32(len_block + 8, (uint32_t)(((uint64_t)len * 8u) >> 32));
    pk_store_be32(len_block + 12, (uint32_t)(((uint64_t)len * 8u) & 0xFFFFFFFFu));
    ghash_update(y, h, len_block, 16);

    aes128_encrypt_block(rk, j0, ks);
    for (i = 0; i < 16; i++) {
        tag_out[i] = y[i] ^ ks[i];
    }
}

void pk_proto_aes128_gcm_clear_cache(void)
{
}

bool pk_proto_aes128_gcm_encrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *plaintext, size_t len,
                                 uint8_t *ciphertext,
                                 uint8_t tag[16])
{
    if (!key || !iv || !tag || (len > 0 && (!plaintext || !ciphertext))) {
        return false;
    }
    aes128_gcm_crypt_and_tag(key, iv, aad, aad_len, plaintext, len, true, ciphertext, tag);
    return true;
}

bool pk_proto_aes128_gcm_decrypt(const uint8_t key[16],
                                 const uint8_t iv[12],
                                 const uint8_t *aad, size_t aad_len,
                                 const uint8_t *ciphertext, size_t len,
                                 const uint8_t tag[16],
                                 uint8_t *plaintext)
{
    uint8_t calc_tag[16];
    uint8_t diff = 0;
    unsigned i;

    if (!key || !iv || !tag || (len > 0 && (!ciphertext || !plaintext))) {
        return false;
    }
    aes128_gcm_crypt_and_tag(key, iv, aad, aad_len, ciphertext, len, false, plaintext, calc_tag);
    for (i = 0; i < 16; i++) {
        diff |= (uint8_t)(calc_tag[i] ^ tag[i]);
    }
    if (diff != 0) {
        if (plaintext && len > 0) {
            memset(plaintext, 0, len);
        }
        return false;
    }
    return true;
}

#endif /* !ESP_PLATFORM */

/* ==========================================================================
 * 4. NIST P-256 (secp256r1) ECDH & Key Generation
 *    ESP32-C3 reuses NimBLE's linked mbedtls ECP/ECDH with hardware TRNG
 *    side-channel blinding; host builds retain the portable C fallback.
 * ========================================================================== */

#if defined(ESP_PLATFORM) || defined(PK_USE_MBEDTLS)

static bool p256_mbedtls_init_priv(mbedtls_ecp_group *grp, mbedtls_mpi *d, const uint8_t priv[32])
{
    mbedtls_ecp_group_init(grp);
    mbedtls_mpi_init(d);
    return mbedtls_ecp_group_load(grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
           mbedtls_mpi_read_binary(d, priv, 32) == 0 &&
           mbedtls_ecp_check_privkey(grp, d) == 0;
}

bool pk_proto_p256_pubkey_from_priv(const uint8_t priv[32], uint8_t pub_out[65])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    size_t olen = 0;
    bool ok = false;

    if (!priv || !pub_out) {
        return false;
    }

    mbedtls_ecp_point_init(&q);
    if (p256_mbedtls_init_priv(&grp, &d, priv) &&
        mbedtls_ecp_mul(&grp, &q, &d, &grp.G, pk_mbedtls_rng, NULL) == 0 &&
        mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, pub_out, 65) == 0 &&
        olen == 65u) {
        ok = true;
    }
    if (!ok) {
        pk_proto_zeroize(pub_out, 65u);
    }
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool pk_proto_ecdh_derive_session_key(const uint8_t priv[32],
                                      const uint8_t peer_pub[65],
                                      uint8_t session_key_out[16])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_mpi z;
    mbedtls_ecp_point q;
    uint8_t sx_be[32];
    uint8_t sha1_digest[20];
    bool ok = false;

    if (!priv || !peer_pub || !session_key_out || peer_pub[0] != 0x04u) {
        if (session_key_out) {
            pk_proto_zeroize(session_key_out, 16u);
        }
        return false;
    }

    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    if (p256_mbedtls_init_priv(&grp, &d, priv) &&
        mbedtls_ecp_point_read_binary(&grp, &q, peer_pub, 65) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
        mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, pk_mbedtls_rng, NULL) == 0 &&
        mbedtls_mpi_write_binary(&z, sx_be, sizeof(sx_be)) == 0 &&
        pk_proto_sha1(sx_be, sizeof(sx_be), sha1_digest)) {
        memcpy(session_key_out, sha1_digest, 16u);
        ok = true;
    }
    if (!ok) {
        pk_proto_zeroize(session_key_out, 16u);
    }
    pk_proto_zeroize(sx_be, sizeof(sx_be));
    pk_proto_zeroize(sha1_digest, sizeof(sha1_digest));
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

#else /* !ESP_PLATFORM && !PK_USE_MBEDTLS */

typedef struct {
    uint32_t d[8];
} u256_t;

static const u256_t k_p256_p = {{
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
}};

static const u256_t k_p256_b = {{
    0x27D2604Bu, 0x3BCE3C3Eu, 0xCC53B0F6u, 0x651D06B0u,
    0x769886BCu, 0xB3EBBD55u, 0xAA3A93E7u, 0x5AC635D8u
}};

static const u256_t k_p256_gx = {{
    0xD898C296u, 0xF4A13945u, 0x2DEB33A0u, 0x77037D81u,
    0x63A440F2u, 0xF8BCE6E5u, 0xE12C4247u, 0x6B17D1F2u
}};

static const u256_t k_p256_gy = {{
    0x37BF51F5u, 0xCBB64068u, 0x6B315ECEu, 0x2BCE3357u,
    0x7C0F9E16u, 0x8EE7EB4Au, 0xFE1A7F9Bu, 0x4FE342E2u
}};

static const u256_t k_p256_n = {{
    0xFC632551u, 0xF3B9CAC2u, 0xA7179E84u, 0xBCE6FAADu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu
}};

static void u256_from_be(u256_t *out, const uint8_t be[32])
{
    int i;
    for (i = 0; i < 8; i++) {
        out->d[7 - i] = pk_load_be32(be + i * 4);
    }
}

static void u256_to_be(uint8_t be[32], const u256_t *in)
{
    int i;
    for (i = 0; i < 8; i++) {
        pk_store_be32(be + i * 4, in->d[7 - i]);
    }
}

static bool u256_is_zero(const u256_t *a)
{
    uint32_t acc = 0;
    int i;
    for (i = 0; i < 8; i++) {
        acc |= a->d[i];
    }
    return acc == 0;
}

static int u256_cmp(const u256_t *a, const u256_t *b)
{
    int i;
    for (i = 7; i >= 0; i--) {
        if (a->d[i] > b->d[i]) {
            return 1;
        }
        if (a->d[i] < b->d[i]) {
            return -1;
        }
    }
    return 0;
}

static uint32_t u256_add_raw(u256_t *r, const u256_t *a, const u256_t *b)
{
    uint64_t carry = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t s = (uint64_t)a->d[i] + (uint64_t)b->d[i] + carry;
        r->d[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return (uint32_t)carry;
}

static uint32_t u256_sub_raw(u256_t *r, const u256_t *a, const u256_t *b)
{
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t d = (uint64_t)a->d[i] - (uint64_t)b->d[i] - borrow;
        r->d[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
    return (uint32_t)borrow;
}

static void fp_add(u256_t *r, const u256_t *a, const u256_t *b)
{
    uint32_t c = u256_add_raw(r, a, b);
    if (c || u256_cmp(r, &k_p256_p) >= 0) {
        (void)u256_sub_raw(r, r, &k_p256_p);
    }
}

static void fp_sub(u256_t *r, const u256_t *a, const u256_t *b)
{
    u256_t tmp;
    if (u256_sub_raw(&tmp, a, b)) {
        (void)u256_add_raw(r, &tmp, &k_p256_p);
    } else {
        *r = tmp;
    }
}

/* Fast NIST P-256 reduction of 512-bit product c[0..15] modulo k_p256_p */
static void fp_mul(u256_t *r, const u256_t *a, const u256_t *b)
{
    uint64_t prod[16];
    uint32_t c[16];
    int64_t acc[9];
    int i, j;

    memset(prod, 0, sizeof(prod));
    for (i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (j = 0; j < 8; j++) {
            uint64_t v = prod[i + j] + (uint64_t)a->d[i] * (uint64_t)b->d[j] + carry;
            prod[i + j] = v & 0xFFFFFFFFu;
            carry = v >> 32;
        }
        prod[i + 8] += carry;
    }
    for (i = 0; i < 16; i++) {
        c[i] = (uint32_t)prod[i];
    }

    /* Solinas reduction for P-256 (FIPS 186-4 D.1.2.3) */
    acc[0] = (int64_t)c[0] + (int64_t)c[8] + (int64_t)c[9] - (int64_t)c[11] - (int64_t)c[12] - (int64_t)c[13] - (int64_t)c[14];
    acc[1] = (int64_t)c[1] + (int64_t)c[9] + (int64_t)c[10] - (int64_t)c[12] - (int64_t)c[13] - (int64_t)c[14] - (int64_t)c[15];
    acc[2] = (int64_t)c[2] + (int64_t)c[10] + (int64_t)c[11] - (int64_t)c[13] - (int64_t)c[14] - (int64_t)c[15];
    acc[3] = (int64_t)c[3] + 2 * (int64_t)c[11] + 2 * (int64_t)c[12] + (int64_t)c[13] - (int64_t)c[15] - (int64_t)c[8] - (int64_t)c[9];
    acc[4] = (int64_t)c[4] + 2 * (int64_t)c[12] + 2 * (int64_t)c[13] + (int64_t)c[14] - (int64_t)c[9] - (int64_t)c[10];
    acc[5] = (int64_t)c[5] + 2 * (int64_t)c[13] + 2 * (int64_t)c[14] + (int64_t)c[15] - (int64_t)c[10] - (int64_t)c[11];
    acc[6] = (int64_t)c[6] + 3 * (int64_t)c[14] + 2 * (int64_t)c[15] + (int64_t)c[13] - (int64_t)c[8] - (int64_t)c[9];
    acc[7] = (int64_t)c[7] + 3 * (int64_t)c[15] + (int64_t)c[8] - (int64_t)c[10] - (int64_t)c[11] - (int64_t)c[12] - (int64_t)c[13];
    acc[8] = 0;

    for (i = 0; i < 8; i++) {
        int64_t v = acc[i];
        int64_t carry = v >> 32;
        acc[i] = v & 0xFFFFFFFFLL;
        acc[i + 1] += carry;
    }

    while (acc[8] < 0) {
        int64_t carry = 0;
        for (i = 0; i < 8; i++) {
            int64_t v = acc[i] + (int64_t)k_p256_p.d[i] + carry;
            acc[i] = v & 0xFFFFFFFFLL;
            carry = v >> 32;
        }
        acc[8] += carry;
    }
    while (acc[8] > 0) {
        int64_t borrow = 0;
        for (i = 0; i < 8; i++) {
            int64_t v = acc[i] - (int64_t)k_p256_p.d[i] + borrow;
            acc[i] = v & 0xFFFFFFFFLL;
            borrow = v >> 32;
        }
        acc[8] += borrow;
    }

    for (i = 0; i < 8; i++) {
        r->d[i] = (uint32_t)acc[i];
    }
    if (u256_cmp(r, &k_p256_p) >= 0) {
        (void)u256_sub_raw(r, r, &k_p256_p);
    }
}

static void fp_sqr(u256_t *r, const u256_t *a)
{
    fp_mul(r, a, a);
}

/* Fermat inversion a^(p-2) mod p */
static void fp_inv(u256_t *r, const u256_t *a)
{
    /* p - 2 = FFFFFFFF 00000001 00000000 00000000 00000000 FFFFFFFF FFFFFFFF FFFFFFFD */
    static const uint32_t exp[8] = {
        0xFFFFFFFDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
        0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
    };
    u256_t res, base;
    int i, bit;

    memset(&res, 0, sizeof(res));
    res.d[0] = 1u;
    base = *a;

    for (i = 0; i < 8; i++) {
        uint32_t w = exp[i];
        for (bit = 0; bit < 32; bit++) {
            if ((w >> bit) & 1u) {
                fp_mul(&res, &res, &base);
            }
            fp_sqr(&base, &base);
        }
    }
    *r = res;
}

typedef struct {
    u256_t x;
    u256_t y;
    u256_t z;
} p256_pt_t;

static void pt_set_infinity(p256_pt_t *p)
{
    memset(p, 0, sizeof(*p));
}

static bool pt_is_infinity(const p256_pt_t *p)
{
    return u256_is_zero(&p->z);
}

/* Jacobian point doubling for a = -3 mod p */
static void pt_double(p256_pt_t *r, const p256_pt_t *p)
{
    u256_t delta, gamma, beta, alpha, t1, t2, x3, y3, z3;

    if (pt_is_infinity(p) || u256_is_zero(&p->y)) {
        pt_set_infinity(r);
        return;
    }

    fp_sqr(&delta, &p->z);
    fp_sqr(&gamma, &p->y);
    fp_mul(&beta, &p->x, &gamma);

    fp_sub(&t1, &p->x, &delta);
    fp_add(&t2, &p->x, &delta);
    fp_mul(&alpha, &t1, &t2);
    fp_add(&t1, &alpha, &alpha);
    fp_add(&alpha, &t1, &alpha); /* alpha = 3*(x - z^2)*(x + z^2) */

    fp_sqr(&x3, &alpha);
    fp_add(&t1, &beta, &beta);
    fp_add(&t1, &t1, &t1);       /* 4*beta */
    fp_add(&t2, &t1, &t1);       /* 8*beta */
    fp_sub(&x3, &x3, &t2);

    fp_add(&z3, &p->y, &p->z);
    fp_sqr(&z3, &z3);
    fp_sub(&z3, &z3, &gamma);
    fp_sub(&z3, &z3, &delta);

    fp_sub(&t1, &t1, &x3);       /* 4*beta - x3 */
    fp_mul(&y3, &alpha, &t1);
    fp_sqr(&t2, &gamma);
    fp_add(&t2, &t2, &t2);
    fp_add(&t2, &t2, &t2);
    fp_add(&t2, &t2, &t2);       /* 8*gamma^2 */
    fp_sub(&y3, &y3, &t2);

    r->x = x3;
    r->y = y3;
    r->z = z3;
}

/* Jacobian point addition */
static void pt_add(p256_pt_t *r, const p256_pt_t *p, const p256_pt_t *q)
{
    u256_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, x3, y3, z3, t1, t2;

    if (pt_is_infinity(p)) {
        *r = *q;
        return;
    }
    if (pt_is_infinity(q)) {
        *r = *p;
        return;
    }

    fp_sqr(&z1z1, &p->z);
    fp_sqr(&z2z2, &q->z);
    fp_mul(&u1, &p->x, &z2z2);
    fp_mul(&u2, &q->x, &z1z1);

    fp_mul(&t1, &p->y, &q->z);
    fp_mul(&s1, &t1, &z2z2);
    fp_mul(&t2, &q->y, &p->z);
    fp_mul(&s2, &t2, &z1z1);

    if (u256_cmp(&u1, &u2) == 0) {
        if (u256_cmp(&s1, &s2) == 0) {
            pt_double(r, p);
        } else {
            pt_set_infinity(r);
        }
        return;
    }

    fp_sub(&h, &u2, &u1);
    fp_add(&t1, &h, &h);
    fp_sqr(&i, &t1);
    fp_mul(&j, &h, &i);

    fp_sub(&t1, &s2, &s1);
    fp_add(&rr, &t1, &t1);
    fp_mul(&v, &u1, &i);

    fp_sqr(&x3, &rr);
    fp_sub(&x3, &x3, &j);
    fp_add(&t1, &v, &v);
    fp_sub(&x3, &x3, &t1);

    fp_sub(&t1, &v, &x3);
    fp_mul(&y3, &rr, &t1);
    fp_mul(&t2, &s1, &j);
    fp_add(&t2, &t2, &t2);
    fp_sub(&y3, &y3, &t2);

    fp_add(&t1, &p->z, &q->z);
    fp_sqr(&z3, &t1);
    fp_sub(&z3, &z3, &z1z1);
    fp_sub(&z3, &z3, &z2z2);
    fp_mul(&z3, &z3, &h);

    r->x = x3;
    r->y = y3;
    r->z = z3;
}

static bool p256_point_on_curve(const u256_t *x, const u256_t *y)
{
    u256_t lhs, rhs, x2, x3, three_x;
    if (u256_cmp(x, &k_p256_p) >= 0 || u256_cmp(y, &k_p256_p) >= 0) {
        return false;
    }
    fp_sqr(&lhs, y);
    fp_sqr(&x2, x);
    fp_mul(&x3, &x2, x);
    fp_add(&three_x, x, x);
    fp_add(&three_x, &three_x, x);
    fp_sub(&rhs, &x3, &three_x);
    fp_add(&rhs, &rhs, &k_p256_b);
    return u256_cmp(&lhs, &rhs) == 0;
}

static bool p256_scalar_mult(u256_t *out_x, u256_t *out_y,
                             const uint8_t scalar_be[32],
                             const u256_t *in_x, const u256_t *in_y)
{
    u256_t k, zinv, zinv2, zinv3;
    p256_pt_t res, base;
    int i, bit;

    u256_from_be(&k, scalar_be);
    if (u256_is_zero(&k) || u256_cmp(&k, &k_p256_n) >= 0) {
        return false;
    }
    if (!p256_point_on_curve(in_x, in_y)) {
        return false;
    }

    pt_set_infinity(&res);
    base.x = *in_x;
    base.y = *in_y;
    memset(&base.z, 0, sizeof(base.z));
    base.z.d[0] = 1u;

    for (i = 7; i >= 0; i--) {
        uint32_t w = k.d[i];
        for (bit = 31; bit >= 0; bit--) {
            pt_double(&res, &res);
            if ((w >> bit) & 1u) {
                pt_add(&res, &res, &base);
            }
        }
    }

    if (pt_is_infinity(&res)) {
        return false;
    }

    fp_inv(&zinv, &res.z);
    fp_sqr(&zinv2, &zinv);
    fp_mul(&zinv3, &zinv2, &zinv);
    fp_mul(out_x, &res.x, &zinv2);
    fp_mul(out_y, &res.y, &zinv3);
    return true;
}

bool pk_proto_p256_pubkey_from_priv(const uint8_t priv[32], uint8_t pub_out[65])
{
    u256_t x, y;
    if (!priv || !pub_out) {
        return false;
    }
    if (!p256_scalar_mult(&x, &y, priv, &k_p256_gx, &k_p256_gy)) {
        return false;
    }
    pub_out[0] = 0x04u;
    u256_to_be(pub_out + 1, &x);
    u256_to_be(pub_out + 33, &y);
    return true;
}

bool pk_proto_ecdh_derive_session_key(const uint8_t priv[32],
                                      const uint8_t peer_pub[65],
                                      uint8_t session_key_out[16])
{
    u256_t px, py, sx, sy;
    uint8_t sx_be[32];
    uint8_t sha1_digest[20];

    if (!priv || !peer_pub || !session_key_out || peer_pub[0] != 0x04u) {
        return false;
    }
    u256_from_be(&px, peer_pub + 1);
    u256_from_be(&py, peer_pub + 33);
    if (!p256_scalar_mult(&sx, &sy, priv, &px, &py)) {
        return false;
    }
    u256_to_be(sx_be, &sx);
    pk_proto_sha1(sx_be, 32, sha1_digest);
    memcpy(session_key_out, sha1_digest, 16);
    memset(sx_be, 0, sizeof(sx_be));
    memset(sha1_digest, 0, sizeof(sha1_digest));
    return true;
}

#endif /* !ESP_PLATFORM */

bool pk_proto_p256_keygen(uint8_t priv_out[32], uint8_t pub_out[65], const uint8_t *seed32)
{
    uint8_t candidate[32];
    unsigned attempt;
    bool ok = false;

    if (!priv_out || !pub_out || !seed32) {
        if (priv_out) {
            pk_proto_zeroize(priv_out, 32u);
        }
        if (pub_out) {
            pk_proto_zeroize(pub_out, 65u);
        }
        return false;
    }
    memcpy(candidate, seed32, 32u);
    for (attempt = 0; attempt < 16; attempt++) {
        if (pk_proto_p256_pubkey_from_priv(candidate, pub_out)) {
            memcpy(priv_out, candidate, 32u);
            ok = true;
            break;
        }
        if (!pk_proto_sha256(candidate, 32u, candidate)) {
            break;
        }
    }
    if (!ok) {
        pk_proto_zeroize(priv_out, 32u);
        pk_proto_zeroize(pub_out, 65u);
    }
    pk_proto_zeroize(candidate, sizeof(candidate));
    return ok;
}


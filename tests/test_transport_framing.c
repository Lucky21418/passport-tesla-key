/*
 * Host transport-framing harness for the stable Tesla key.
 *
 * This pins the wire contract that sits under every BLE reply:
 *   - 2-byte big-endian length prefix, then the RoutableMessage;
 *   - a message may arrive in any number of notification-sized chunks;
 *   - a partial message older than PK_PROTO_RX_TIMEOUT_MS is discarded;
 *   - a zero or oversized length prefix is a framing error.
 *
 * D05 streaming reassembly (updated in S2):
 *   - main/port_ble.c handle_ble_notify_rx() loops over pk_rx_framer_feed_ex()
 *     until all bytes of a notification are consumed;
 *   - coalesced frames in one notification are all emitted, and any trailing
 *     partial frame prefix is retained for the next notification without loss.
 *
 * No ESP-IDF dependency: compiled with every components/tesla_key/src source
 * on the host.
 */

#include "pk_protocol.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

static size_t make_frame(uint8_t *out, size_t cap, uint8_t fill, size_t msg_len)
{
    uint8_t msg[PK_PROTO_MAX_FRAME_LEN];

    memset(msg, fill, msg_len);
    return pk_proto_frame_wrap(msg, msg_len, out, cap);
}

static void test_full_frame_in_one_chunk(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0xA5, 100);

    expect(len == 102, "framing: 100-byte message wraps to 102");
    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, framed, len, 1000) == 1, "framing: one chunk completes");
    expect(f.expected_len == 100, "framing: expected_len is the message length");
    expect(f.buf[0] == 0xA5 && f.buf[99] == 0xA5, "framing: payload copied verbatim");
}

static void test_byte_by_byte_delivery(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x5A, 64);
    size_t i;
    int rc = 0;

    pk_rx_framer_init(&f);
    for (i = 0; i + 1 < len; i++) {
        rc = pk_rx_framer_feed(&f, framed + i, 1, 1000 + (pk_ms_t)i);
        expect(rc == 0, "framing: partial byte-by-byte feed waits");
    }
    rc = pk_rx_framer_feed(&f, framed + len - 1, 1, 1000 + (pk_ms_t)len);
    expect(rc == 1, "framing: last byte completes a byte-by-byte message");
    expect(f.expected_len == 64, "framing: reassembled length is correct");
}

static void test_split_length_prefix(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x33, 8);

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, framed, 1, 100) == 0,
           "framing: one length byte alone waits");
    expect(pk_rx_framer_feed(&f, framed + 1, len - 1, 110) == 1,
           "framing: split length prefix still completes");
}

static void test_coalesced_frames_and_trailing_prefix(void)
{
    uint8_t first[PK_PROTO_MAX_FRAME_LEN + 2];
    uint8_t second[PK_PROTO_MAX_FRAME_LEN + 2];
    uint8_t both[(PK_PROTO_MAX_FRAME_LEN + 2) * 2];
    pk_rx_framer_t f;
    size_t len_a = make_frame(first, sizeof(first), 0x11, 10);
    size_t len_b = make_frame(second, sizeof(second), 0x22, 20);
    size_t consumed = 0;

    memcpy(both, first, len_a);
    memcpy(both + len_a, second, len_b);

    pk_rx_framer_init(&f);
    /* D05 upgrade (replaces pre-S2 defect-pinned "trailing bytes dropped"
     * assertion): pk_rx_framer_feed_ex() returns exact consumed bytes so the
     * BLE RX notification loop emits every coalesced frame and preserves any
     * trailing partial frame across notifications without silently dropping it. */
    expect(pk_rx_framer_feed_ex(&f, both, len_a + len_b, 100, &consumed) == 1,
           "framing: first coalesced message is returned");
    expect(f.expected_len == 10 && consumed == len_a,
           "framing: first coalesced frame consumes only frame A bytes");
    expect(f.buf[0] == 0x11 && f.buf[9] == 0x11,
           "framing: first coalesced payload intact");

    expect(pk_rx_framer_feed_ex(&f, both + consumed, (len_a + len_b) - consumed,
                                100, &consumed) == 1,
           "framing: second coalesced message in same notification completes");
    expect(f.expected_len == 20 && consumed == len_b,
           "framing: second coalesced frame consumes frame B bytes");
    expect(f.buf[0] == 0x22 && f.buf[19] == 0x22,
           "framing: second coalesced payload intact");

    /* Trailing partial prefix after frame A must be preserved for next notify. */
    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed_ex(&f, both, len_a + 5, 200, &consumed) == 1 && consumed == len_a,
           "framing: frame A completes when followed by partial frame B");
    expect(pk_rx_framer_feed_ex(&f, both + len_a, 5, 200, &consumed) == 0 && consumed == 5,
           "framing: trailing 5-byte prefix of frame B buffered cleanly");
    expect(pk_rx_framer_feed_ex(&f, second + 5, len_b - 5, 210, &consumed) == 1 &&
           f.expected_len == 20 && consumed == len_b - 5,
           "framing: remaining bytes of frame B complete on next notification");
}

static void test_inter_chunk_timeout(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x77, 32);

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, framed, len / 2, 0) == 0, "framing: half a message waits");
    /* protocol.md: a partial message older than 1000 ms is discarded. */
    expect(pk_rx_framer_feed(&f, framed, len, 1001) == 1,
           "framing: stale partial is dropped, fresh message completes");
    expect(f.expected_len == 32, "framing: timeout restart yields the fresh length");

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, framed, len / 2, 2000) == 0,
           "framing: half a message waits again");
    expect(pk_rx_framer_feed(&f, framed + len / 2, len - len / 2, 3000) == 1,
           "framing: exactly 1000 ms between chunks is still accepted");
}

static void test_bad_lengths(void)
{
    static const uint8_t zero_len[2] = { 0x00, 0x00 };
    static const uint8_t too_long[2] = { 0x04, 0x01 }; /* 1025 > 1024 */
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x44, 4);

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, zero_len, sizeof(zero_len), 10) == -1,
           "framing: zero-length message is rejected");
    expect(f.active == false && f.len_received == 0, "framing: error resets the framer");
    expect(pk_rx_framer_feed(&f, framed, len, 20) == 1,
           "framing: valid message after zero-length error");

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, too_long, sizeof(too_long), 30) == -1,
           "framing: length above PK_PROTO_MAX_FRAME_LEN is rejected");
    expect(pk_rx_framer_feed(&f, framed, len, 40) == 1,
           "framing: valid message after oversize error");
}

static void test_max_length_frame(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x99, PK_PROTO_MAX_FRAME_LEN);

    expect(len == PK_PROTO_MAX_FRAME_LEN + 2, "framing: max message wraps");
    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(&f, framed, len, 50) == 1, "framing: max message accepted");
    expect(f.expected_len == PK_PROTO_MAX_FRAME_LEN, "framing: max length preserved");
}

/* Feed one buffer the way a caller would if it kept going after an error
 * by advancing `consumed`. An illegal length must consume the whole chunk
 * so a later valid frame in that chunk is not assembled. */
static int drain_frames(pk_rx_framer_t *f, const uint8_t *buf, size_t len, pk_ms_t now)
{
    size_t off = 0u;
    int frames = 0;

    while (off < len) {
        size_t consumed = 0u;
        int rc = pk_rx_framer_feed_ex(f, buf + off, len - off, now, &consumed);

        if (rc == 1) {
            frames++;
        }
        if (rc < 0) {
            if (consumed == 0u) {
                break;
            }
            off += consumed;
            continue;
        }
        if (consumed == 0u) {
            break;
        }
        off += consumed;
    }
    return frames;
}

static void test_no_heuristic_header_search(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    uint8_t blob[2 + PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;
    size_t len = make_frame(framed, sizeof(framed), 0x5C, 8);
    size_t consumed = 0u;

    blob[0] = 0x00;
    blob[1] = 0x00;
    memcpy(blob + 2, framed, len);
    pk_rx_framer_init(&f);
    expect(drain_frames(&f, blob, len + 2u, 10) == 0,
           "framing: zero length does not assemble the following frame");
    expect(f.active == false && f.len_received == 0 && f.err_zero_len == 1u,
           "framing: zero length discards the chunk and resets");

    blob[0] = 0x04;
    blob[1] = 0x01; /* 1025 > 1024 */
    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed_ex(&f, blob, len + 2u, 20, &consumed) == -1 &&
           consumed == len + 2u,
           "framing: oversize length consumes the whole chunk");
    expect(f.active == false && f.len_received == 0 && f.err_oversize == 1u,
           "framing: oversize length resets the framer");
    expect(drain_frames(&f, blob, len + 2u, 20) == 0,
           "framing: oversize length does not assemble the following frame");

    /* A later notification, after the reset, is still a new frame. */
    expect(pk_rx_framer_feed(&f, framed, len, 30) == 1 && f.expected_len == 8,
           "framing: valid frame after discard-and-reset is accepted");

    /* One junk byte then a real header. Sliding to that header is the search
     * this framer must not do. 0xFF plus the real high length byte is > 1024. */
    blob[0] = 0xFF;
    memcpy(blob + 1, framed, len);
    pk_rx_framer_init(&f);
    expect(drain_frames(&f, blob, len + 1u, 40) == 0,
           "framing: junk prefix is not scanned for a later header");
    expect(f.active == false && f.len_received == 0,
           "framing: junk prefix resets instead of resyncing");
}

static void test_arguments_and_empty_chunk(void)
{
    uint8_t framed[PK_PROTO_MAX_FRAME_LEN + 2];
    pk_rx_framer_t f;

    pk_rx_framer_init(&f);
    expect(pk_rx_framer_feed(NULL, framed, 1, 0) == -1, "framing: null framer rejected");
    expect(pk_rx_framer_feed(&f, NULL, 1, 0) == -1, "framing: null chunk rejected");
    expect(pk_rx_framer_feed(&f, framed, 0, 0) == 0, "framing: empty chunk is a no-op");
    expect(f.active == false, "framing: empty chunk does not arm the framer");
    pk_rx_framer_init(NULL); /* documented no-op */
}

int main(void)
{
    test_full_frame_in_one_chunk();
    test_byte_by_byte_delivery();
    test_split_length_prefix();
    test_coalesced_frames_and_trailing_prefix();
    test_inter_chunk_timeout();
    test_bad_lengths();
    test_no_heuristic_header_search();
    test_max_length_frame();
    test_arguments_and_empty_chunk();

    if (g_fail) {
        fprintf(stderr, "test_transport_framing: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_transport_framing: ok\n");
    return 0;
}

**English** · [简体中文](vehicle-interaction-protocol-checklist.zh_CN.md)

# Vehicle-interaction protocol checklist

Adjudicated edition: 2026-11-25 23:00 CST (Asia/Shanghai). The coordinator merged the official cloud verification checklist with several third-party research reports that were sent only to the coordinator, and also listed the third-party conclusions that were not accepted, so later lookups can find them and the same arguments are not repeated.

Supplement: 2026-11-25 23:30 CST. Incorporated the Word document the user submitted again, *Read-only verification checklist for the vehicle-interaction specification (against the five reference repositories named at project start)*. Official Part 1 / Part 2 are still not rewritten. New items are written into Sources and Part 3. This round's three-item fix is not expanded.

Re-adjudication: 2026-11-25 23:50 CST. The coordinator re-judged from all the research materials and personally re-read the original text of `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` (also checking the earlier `a4b43c1`). The first official cloud audit is not assumed to be correct by default. Session, counters, per-request window, explicit wake, a missing reply must not be treated as failure, and an omitted lock state must not be shown as unlocked: these core conclusions still hold after the original text was re-checked. The first cloud audit missed that the official client calls `ExchangeMTU` after subscribe (on failure, fall back to the default ATT). The Word report's claim that "official does not exchange MTU," and its treatment of `session.go:43-53` as the window implementation, do not match that live object. The five README constraints are not changed. This round's three-item fix is not expanded.

Plan reordered by P: 2026-11-25 23:55 CST. The "follow-up research plan" at the end of the document is rearranged onto baseline P00-P17. Development principles are not changed. This round's three-item fix is not expanded, and the device is not flashed.

Concentrated-testing principle: 2026-11-25 23:58 CST. The plan is rewritten under a hard user order: refuse testing once per update. Before any flash, finish the code and the unique reviews that can be done without the car, and flash and test only after authorization, both in one concentrated pass. The three fixes now in progress are still not expanded mid-course. Protocol alignment that does not depend on a real vehicle is moved to before the flash, one item at a time, each finished completely.

Authority order: `protocol.md` and the official source of `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` are the first authority. The official cloud checklist ([bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be)) is one evidenced verification, not an uncorrectable final review. Where it conflicts with the official original text, the original text wins, and the correction is recorded in this file. Third-party research reports are references and records of pitfalls already hit. Conflicts are resolved by the official original text. This file does not replace an official code unique review of a specific commit.

Product comparison node: `Lucky21418/passport-tesla-key` candidate branch `cand/d08-dead-code-cleanup` @ `1377473a0170fde55a3eacba6068ef5767adc9d2`. `main` (`e85197d56abe92c35a1cfb84fad85fba2545c9cc`) and `feature/tesla-key` (`47fd967aaa22d619fce3ffa15a3e97d5bd4ccac2`) were not moved. The device remains the authorized and uniquely reviewed `a3a5305`. This file does not authorize flashing.

The original official cloud checklist was written before the consensus document was committed, and that cloud task did not change the repository. The candidate branch later received documentation commit `4b1bd25643cec7eb4e92fc7e071821c75fd33a71` (`README.md` / `README.zh_CN.md` only), which wrote the five executable constraints. The "Consensus-doc gap" below has been corrected for that. The official re-review of `1377473` against this checklist is [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc). Conclusion FAIL: a timeout clears a known lock state, and opening the status page implicitly wakes; an omitted `vehicleLockState` can still be shown as unlocked, which is residual. The current fix is still these three items and is not expanded.

Part 1, Part 2, Conflicts, Class C, Application-layer schema, and the two official summaries below keep the original cloud-checklist text (English). Only document gaps that later facts rewrote are corrected. The new "Sources," "Product gap for this round," and "Part 3 adjudication of third-party research reports (including what is not accepted)" were written by the coordinator.

The official checklist was not connected to a real vehicle at the time. "What it does on a real car" means the consequence inferred from the cited protocol text and the cited client code, not a new capture from a real vehicle.

## Sources re-read

| Repo | Role | Live object |
| --- | --- | --- |
| `teslamotors/vehicle-command` | Primary authority | `main` `0a582da430c4aca6d397fb6cbed5b4d6bb380125` (2026-11-03, “tesla-control: add PIN to Drive command”) |
| `FoloToy/ai-passport` | Official hardware reference | `main` `8e9bfaf3c575e5c8f3107ac8cd04b4b1eb294fd1` (2026-11-24) |
| `tesla-local-control/tesla_ble_mqtt_docker` | Third-party MQTT Docker | `main` `69e3b2b20bbe42b167a30c95bd6d4beee2fb7612` (2025-03-03) |
| `tesla-local-control/tesla_ble_mqtt_core` | Submodule `app` of that Docker repo | `696d46bd362cfdc6a3bb263a12e63bb2bfeee449` (2025-02-23), also that repo’s `main` |
| `0Bu/tesla-key-esp32` | Third-party ESP32 key | `main` `3a215d60ebcb584ccd558109188d5042582066be` (2026-11-25) |
| `yoziru/tesla-ble` | Comparison oracle only. Not Passport runtime | `main` `67ec5c7ff8cf14664d76810d9f8e3fb4b499387a` (2026-11-07, tag `v6.4.0`) |

Classes:

- **A** — sentence or behavior in official Tesla `vehicle-command`, or a fact actually present in official FoloToy.
- **B** — third-party text re-read in the live tree. Each item says whether it agrees with A. B never overrides A.
- **C** — unverified, anecdotal, or a local timer. Not a protocol norm.

## Consensus-doc status (updated 2026-11-25 23:00 CST)

When the official cloud checklist was written, the consensus document had not yet recorded the session, anti-replay, and wake constraints. Later, `4b1bd25643cec7eb4e92fc7e071821c75fd33a71` changed only the root `README.md` and `README.zh_CN.md` (+11 lines each) and wrote five executable constraints: the response window is per the same request, and a first packet whose counter is 0 must be accepted; the official window width is 32; VCSEC sends only one request at a time, and a missing reply must not be automatically resent; wake must be explicit and must not implicitly wake in order to read status; an omitted proto3 `vehicleLockState` must not be shown as unlocked. It also states the roles of the five repositories, and that Class C timers are not official specification.

That commit is not a substitute for this complete checklist. `docs/assets/README.md` still mainly cites `vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` as the protobuf schema source. `PK_WAKE_SETTLE_MS` (7000) is still a local pause in the product source (C7). The comment already says it is not a safety TTL. `FoloToy/ai-passport` at the checked SHA has no `tesla` / `VCSEC` / `vehicle-command` matches and contributes no vehicle-interaction rules (A0).

The complete catalog is this file. Third-party conclusions that were not accepted are in Part 3. Do not write them back into the README as official requirements.

---

# Part 1 — Official rules, item by item

## 1. Session setup, keys, epoch, request identity

### A1. Two domains, two sessions

Rule: VCSEC and Infotainment have different public keys and need separate handshakes and session state. VCSEC handles locks, remote start, and trunk. Infotainment handles the remaining commands. VCSEC is reachable over BLE while infotainment is asleep.

Class: **A**.

Source: `teslamotors/vehicle-command` `pkg/protocol/protocol.md`, section “Domains”, lines 170–176.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L170-L176

SDK: `Vehicle.StartSession` says a VCSEC-only client can skip Infotainment and thereby avoid waking it (`pkg/vehicle/vehicle.go`, `StartSession`, lines 144–148). `BodyControllerState` says the VCSEC status read works over BLE while infotainment is asleep (`pkg/vehicle/state.go`, lines 11–14).

Passport: keep two session records. A lock, unlock, or VCSEC status read does not open an Infotainment session.

### A2. Who generates the epoch

Rule: Each domain has its own clock as `(epoch_id, timestamp)`. The vehicle generates `epoch_id`, a random 16-byte value, at boot. `timestamp` is seconds since that epoch. The client does not invent the epoch. Command expiration is in that domain’s seconds, so the client tracks a separate offset per domain.

Class: **A**.

Source: `protocol.md` “Time”, lines 178–183, and “Metadata”, lines 545–546.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L178-L183

Schema: `SessionInfo.epoch` is `bytes`, `clock_time` is `fixed32` (`pkg/protocol/protobuf/signatures.proto`, `SessionInfo`, lines 102–110). SDK constant `epochIDLength = 16` (`internal/authentication/crypto.go`, line 19).

Passport: store the epoch bytes from an authenticated `SessionInfo`. A VCSEC epoch or clock offset is not applied to Infotainment.

### A3. Client key is enrolled before the handshake

Rule: The handshake section assumes client public key `C` is already on the vehicle. New keys are authorized by an existing key, with a Tesla service key as the root of trust. The handshake request sets `session_info_request.public_key` to `ENCODE_PUBLIC(C)`: uncompressed NIST-P256, `0x04 || X(32) || Y(32)`.

Class: **A**.

Source: `protocol.md` “Handshake” / “Request”, lines 343–355, and “Notation”, lines 269–271.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L343-L355

Passport: do not start a command session with a key the car has not enrolled. The wire public key is 65 bytes, uncompressed.

### A4. Shared key K

Rule: `S = ECDH(client_private, vehicle_public)` on NIST-P256. `K` is the first 16 bytes of `SHA1(big-endian X)`. The protocol uses those raw 16 bytes as the AES-128-GCM key.

Class: **A**.

Source: `protocol.md` “Key Agreement”, lines 410–439.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L410-L439

Passport: do not use the raw ECDH secret, and do not use SHA-256, as `K`.

### A5. Session-info tag

Rule: `SESSION_INFO_KEY = HMAC-SHA256(K, "session info")` with that ASCII label. Metadata is TLV, tags ascending, ended by `0xFF`: signature type `SIGNATURE_TYPE_HMAC` (6), VIN, and the handshake request UUID as `TAG_CHALLENGE`. The tag is `HMAC-SHA256(SESSION_INFO_KEY, metadata || session_info_bytes)`. Compare in constant time. Discard the response if the tag does not match. The vehicle’s `signature_data.session_info_tag` proves integrity of the session info, and the binding to the request UUID is what stops a replayed handshake response. The UUID must be unpredictable and at most 16 bytes.

Class: **A**.

Source: `protocol.md` lines 52–58 and “Response authentication”, lines 441–514.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L449-L508

SDK: label `labelSessionInfo = "session info"` (`crypto.go`, line 13). `NativeSession.SessionInfoHMAC` (`internal/authentication/native.go`, lines 76–87). `NewAuthenticatedSigner` uses `hmac.Equal` (`internal/authentication/signer.go`, lines 50–63). `Dispatcher.Send` draws 16 random UUID bytes (`internal/dispatcher/dispatcher.go`, lines 393–410).

Passport: install session material only after that HMAC succeeds against the UUID of a handshake this client just sent. Do not use a counter or the VIN as that UUID.

### A6. When a later SessionInfo may replace the current one

Rule: Discard included session info if any of these is true: the request UUID was not used in the last several seconds; the session-info HMAC is wrong; the clock time is earlier than a previously authenticated session info with the same epoch. Otherwise update. On update, do not roll the command anti-replay counter backward unless the epoch changes.

Class: **A** (specification).

Source: `protocol.md` “Recovering from synchronization errors”, lines 748–768.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L748-L768

SDK age: BLE `maxLatency = 4 * time.Second` (`pkg/connector/ble/ble.go`, lines 27–29, 94–96). The dispatcher drops session info older than that (`dispatcher.go`, `checkForSessionUpdate`, lines 182–201). Fleet API default is 10 s (`pkg/connector/inet/inet.go`, `MaxLatency`).

SDK counter: `Signer.UpdateSessionInfo` raises the local counter only when the new counter is greater, including when the epoch bytes change (`signer.go`, lines 102–117). It never stores a lower counter. That is stricter than the spec’s permission to roll back on an epoch change, and it still obeys “do not roll back unless the epoch changes.”

Passport: same-epoch clock must not move backward. Never decrease the stored command counter for the same epoch. Use 4 s as the BLE session-info age limit when matching the official client.

### A7. Command counter versus response counter

Rule: These are different counters.

The **command** counter is metadata on the request. It starts as `session_info.counter` and must increase for every command in that epoch. Infotainment accepts a sliding window so commands may arrive out of order. VCSEC requires command counters to arrive in order. The worked example has handshake `counter: 6` and the next command `counter: 7`.

The **response** counter is `AES_GCM_Response_Signature_Data.counter`. It is checked per request (A11–A16). It is not the command counter.

Class: **A**.

Source: `protocol.md` lines 402, 541, 548–550, 616, 671, and “Counter verification”, lines 819–826.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L548-L550

SDK command counter: `NewSigner` stores `verifierInfo.GetCounter()` (`signer.go`, lines 35–39). `Encrypt` increments before sealing and refuses `0xFFFFFFFF` (`signer.go`, lines 176–181).

Passport: the first sealed command counter is one greater than the handshake counter. Do not put the handshake counter on the wire unchanged, and do not use the response counter as the next command counter.

### A8. Vehicle-side command counter 0 is not a response rule

Rule, official vehicle verifier only: when checking an **incoming command**, `Verifier.Verify` runs the sliding window only if `counter > 0`. A command counter of 0 skips that window. `verifySessionInfo` says counter 0 disables the counter check and is only for messages that can be replayed safely, are likely to arrive out of order, and expire soon. If the command counter is 0 or older than the verifier’s counter, remaining lifetime must be at most 30 s or the fault is `MESSAGEFAULT_ERROR_TIME_TO_LIVE_TOO_LONG`.

Class: **A** (SDK). This sentence is not in `protocol.md`.

Source: `internal/authentication/verifier.go`, lines 228–233 and 354–364. `maxSecondsWithoutCounter = 30` in `crypto.go`, line 20.

Passport: do not send command counter 0. Do not treat this verifier branch as a reason to reject response counter 0.

### A9. Request authentication on BLE

Rule: BLE uses AES-GCM. Detached AES-GCM is for Fleet API scope checks and is advertised only by Infotainment (`SESSION_INFO_FLAG_SUPPORTS_DETACHED_ENCRYPTION`). HMAC plaintext is only for Fleet API when detached encryption is unavailable. Request metadata, tags ascending, `0xFF` terminated: signature type, domain, VIN, epoch, expires-at (uint32 big-endian), counter (uint32 big-endian), flags only when non-zero, and the command prefix only for detached encryption. AES-GCM key is `K`, nonce is 12 random bytes, AAD is `SHA256(metadata)`. Ciphertext is `protobuf_message_as_bytes`. `signer_identity.public_key` is the uncompressed client key.

Class: **A**.

Source: `protocol.md` “Metadata” through “AES-GCM encryption”, lines 530–682.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L552-L582

SDK: `PreferredAuthMethod` returns `AuthMethodGCM` (`ble.go`, lines 55–57). Flags are added only when `message.Flags > 0` (`internal/authentication/peer.go`, `extractMetadata`, lines 78–83). `nonceLength = 12` (`verifier.go`, line 18). Fault comment: “AES-GCM must use 12-byte IVs” (`universal_message.proto`, line 48).

Passport: BLE commands are personalized AES-GCM. Omit a zero flags field from **request** AAD. Nonce is 12 bytes.

### A10. Request identity (signature-type byte + tag)

Rule: The request hash, which the official client calls the request id, is one signature-type byte followed by that request’s authentication tag.

- Personalized AES-GCM: byte `SIGNATURE_TYPE_AES_GCM_PERSONALIZED` (5) plus `AES_GCM_Personalized_data.tag`.
- Detached AES-GCM: byte `SIGNATURE_TYPE_AES_GCM_DETACHED` (10) plus `AES_GCM_Detached_data.tag`.
- HMAC: byte `SIGNATURE_TYPE_HMAC_PERSONALIZED` (8) plus `HMAC_Personalized_data.tag`.

AES-GCM tags are 16 bytes, so those identities are 17 bytes on every domain: `type || 16-byte tag`. For a request **to VCSEC**, the hash is truncated to 17 bytes (type byte plus the first 16 MAC bytes). The spec says that truncation changes HMAC-SHA256 tags, which are longer than 16 bytes. An Infotainment HMAC tag is not truncated.

Class: **A**.

Source: `protocol.md` “Request hash”, lines 797–817.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L797-L817

SDK: `authentication.RequestID` (`peer.go`, lines 97–122). `TestRequestID` expects 17 bytes for VCSEC HMAC and the full tag for Infotainment HMAC (`peer_test.go`, lines 104–146). `Dispatcher.Send` stores that slice on the receiver (`dispatcher.go`, line 433): `createHandler(&key, authentication.RequestID(message))`.

Passport: for BLE AES-GCM, save `0x05 || request_tag` (or `0x0A || tag` if detached) **on that in-flight request**, and feed that exact slice into the response AAD as `TAG_REQUEST_HASH`. Do not keep one shared `req_id` for the whole domain if more than one request can be outstanding (see P2).

## 2. Response counter: definition, scope, window, unmatched replies

### A11. Where the response counter lives

Rule: `Signatures.AES_GCM_Response_Signature_Data` is `nonce` (field 1), `counter` (field 2, uint32), `tag` (field 3). If this field is present, `protobuf_message_as_bytes` is encrypted. If it is absent, the payload is plaintext. Clients set `FLAG_ENCRYPT_RESPONSE` (bit 1, wire value 2) on every command. Firmware 2024.38+ encrypts. Older firmware ignores the flag, so the client does not read the version first.

Class: **A**.

Source: `protocol.md` lines 62–65 and 786–795. Schema: `signatures.proto`, lines 59–63 and 83.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protobuf/signatures.proto#L59-L63

SDK default flags: `DefaultFlags` in `pkg/vehicle/vehicle.go`, line 22.

Passport: presence of `AES_GCM_Response_data` selects decryption. A missing signature is not counter 0 (P1).

### A12. Response AAD uses that counter and that request’s identity

Rule: Decrypt with `K`, the response nonce, and the response tag. AAD is `SHA256` of TLV metadata, tags ascending, `0xFF` terminated:

| Tag | Contents |
| --- | --- |
| `TAG_SIGNATURE_TYPE` (0) | `SIGNATURE_TYPE_AES_GCM_RESPONSE` (9) |
| `TAG_DOMAIN` (1) | domain that sent the response |
| `TAG_PERSONALIZATION` (2) | VIN |
| `TAG_COUNTER` (5) | `AES_GCM_Response_data.counter`, uint32 big-endian |
| `TAG_FLAGS` (7) | flags of the **response**, always present |
| `TAG_REQUEST_HASH` (8) | the request identity from A10 |
| `TAG_FAULT` (9) | `signed_message_fault`, uint32 big-endian |

Request AAD omits flags when they are zero. Response AAD always includes flags.

Class: **A**.

Source: `protocol.md` “Response metadata”, lines 828–846.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L828-L846

SDK: `Peer.responseMetadata` (`peer.go`, lines 124–136). `Signer.Decrypt` reads `GetAES_GCM_ResponseData()`, builds that AAD, decrypts, and returns `gcmInfo.Counter` (`signer.go`, lines 308–337). A missing AES-GCM response block is an error, not a successful plaintext decode.

Passport: build response AAD from the response’s own counter, flags, and fault, plus the identity of the request this reply answers.

### A13. The check is per the same request, not per domain

Rule: The client must reject a response counter that was already used **in a response for the same request**. One request may produce several responses, and those responses may arrive out of order. The check is not “this counter was used earlier in this domain.”

Class: **A**.

Source: `protocol.md` “Counter verification”, lines 819–826.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L819-L826

SDK structure, which is the scope to copy:

- `receiver` holds `antireplay authentication.SlidingWindow` and `requestID []byte` (`internal/dispatcher/receiver.go`, lines 29–38).
- `createHandler` allocates a new receiver for that send (`dispatcher.go`, lines 158–173). A new Go `SlidingWindow` has `used == false`.
- `session.decrypt` calls `handler.antireplay.Update(counter)` (`internal/dispatcher/session.go`, lines 48–58). The session object does not own a response window.
- The domain `session` owns the **command** signer (`signer.go` `counter`), which is a different field.

Passport: one response-counter window per in-flight request. Creating the next request starts a new window. Do not hang the response window on the domain session (P1).

### A14. First response counter, including 0, is accepted

Rule: The specification says “not previously used.” It does not reserve 0. The official client accepts the first counter of a request at any value, including 0.

Class: **A**.

Source: `protocol.md` lines 819–826, together with `SlidingWindow.Update` (`internal/authentication/window.go`, lines 58–63):

```text
if !w.used {
    w.used = true
    w.counter = counter
    return true
}
```

There is no `counter == 0` rejection on this path. `Signer.Decrypt`’s comment says the client must verify that the counter increases for that id or falls inside the sliding window (`signer.go`, lines 308–311).

Passport: the first authenticated reply of a request with `counter = 0` is valid. Record it. Do not drop it as a replay and do not treat 0 as “unset.”

### A15. Reuse on that same request is rejected

Rule: The same counter value on a later reply to the same request is a replay. The official window leaves its state unchanged when it rejects.

Class: **A**.

Source: `updateSlidingWindow`, the branch `if counter == newCounter { return }` with `ok` still false (`window.go`, lines 17–19). Test “Update should fail because newCounter == counter” (`window_test.go`, lines 73–81). `session.decrypt` returns `protocol.ErrReplayedResponse` (`session.go`, lines 55–57). The dispatcher logs and drops that duplicate (`dispatcher.go`, lines 301–304). The error comment says a duplicate can be a benign retransmission (`pkg/protocol/error.go`, lines 55–58).

Passport: a second frame with the same response counter, including a second 0, is dropped. Do not fail the whole command solely because a duplicate frame arrived after a good one.

### A16. Out of order is allowed inside 32; older than that is rejected

Rule: The spec allows out-of-order responses for one request. The official client’s definition of “already used / still fresh” is a 32-deep bit window.

Class: **A** for the out-of-order requirement (`protocol.md` lines 823–824). **A** for the width 32 as official client behavior. `protocol.md` does not print the integer 32.

Source: `windowSize = 32` (`crypto.go`, line 21: “Verifier.windowSize is uint64, so must be ≤ 64”). Algorithm (`window.go`, lines 22–48):

- If `newCounter > counter`, accept, shift the bit window by the gap, and set the bit for the previous highest.
- If `newCounter < counter`, `age = counter - newCounter`. `age > 32` rejects and does not change state, because the client can no longer prove the value is fresh.
- `age` from 1 through 32 is accepted when bit `(age-1)` is clear, and rejected when that bit is set.
- A jump that shifts by 32 or more clears the remembered bits (`window_test.go`, the case newCounter 500). The new high value is still accepted. The skipped values are then outside history and a late copy of an old one is rejected.

Passport: implement width **32**, not 64 (Conflict 1). Accept a reply whose counter is inside that window even if it arrives before a higher counter. Reject a counter more than 32 behind the highest accepted counter of **that request**.

### A17. A reply with no matching request does not touch a window

Rule: Match the reply to an outstanding request before decryption and before any counter update. An unmatched reply is dropped. It must not mark a counter used.

Class: **A**.

Source: `Dispatcher.process` (`dispatcher.go`, lines 245–314):

- Missing source: drop (lines 248–251).
- `request_uuid` length other than 0 or 16: drop (lines 254–257).
- Non-VCSEC: the match key includes the UUID (lines 259–261). VCSEC: the UUID is not copied into the key; the per-request routing address is the discriminator (lines 400–407 and 259–261).
- Destination must be the client’s routing address, 16 bytes (lines 269–285).
- No registered handler: “Dropping message without registered handler” and return (lines 287–292). `decrypt` is not called.
- Only then: optional session-info update, then `decrypt`, which is the only place `antireplay.Update` runs.

`dispatcher.decrypt` returns immediately when `AES_GCM_Response_data` is absent, so a plaintext reply does not consume the counter window (`dispatcher.go`, lines 226–230).

Passport: look up the in-flight request first. If none matches, discard the frame and leave every window unchanged. A stray or late frame must not poison the next command.

### A18. Official queue order

Rule: The official client updates the window inside `decrypt`, then tries to enqueue. If the receiver channel is full (`receiverBufferSize = 10`), the frame is dropped **after** the counter was consumed (`dispatcher.go`, lines 310–314; `receiver.go`, line 11).

Class: **A** (SDK order).

Passport: a VCSEC request can produce three replies (A19). The per-request queue has to hold them. Do not mark a counter used and then throw the payload away because the queue depth is 1. Depth 10 in the official client is enough for the spec’s three VCSEC replies.

## 3. VCSEC replies and concurrency

### A19. Up to three replies, and which one is final

Rule: VCSEC emits up to three responses to one request. Fleet API callers see only the last. BLE callers decide:

- NFC pairing: `OPERATIONSTATUS_WAIT` means waiting for the card.
- Any other request: `OPERATIONSTATUS_WAIT` means busy. Retry after a short delay.
- Other `operationStatus` values can be ignored. `OPERATIONSTATUS_ERROR` is for legacy clients. New clients discard that message and wait for a more specific error.
- A whitelist add/remove is terminal when `whitelistOperationStatus` is populated. Empty whitelist messages are dropped.
- For every other operation, an empty message means success and `nominalError` means an error.

Class: **A**.

Source: `protocol.md` “VCSEC application-layer responses”, lines 855–882.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L855-L882

SDK: `readUntil` keeps reading until the predicate says terminal (`pkg/vehicle/vcsec.go`, lines 66–82). RKE and closures treat a nil `commandStatus` as success (lines 175–179, 218–222). Whitelist completion requires `whitelistOperationStatus` (lines 132–143). `OPERATIONSTATUS_WAIT` becomes `ErrBusy` (lines 50–51).

SDK difference, still official code: `unmarshalVCSECResponse` can return on `OPERATIONSTATUS_ERROR` when a whitelist information code is set, or `ErrUnknown` when signed-message status is absent (lines 52–58). For a non-whitelist command, Passport follows the spec sentence and keeps waiting. A populated whitelist status and a `nominalError` are terminal.

Passport: do not complete a lock or unlock on the first VCSEC frame. Hold the same request’s counter window open across the up-to-three replies.

### A20. Do not run two VCSEC requests at once

Rule: `request_uuid` is often missing on VCSEC replies. Clients tell those replies apart with a unique 16-byte routing address, and should avoid simultaneous requests to that domain.

Class: **A**.

Source: `protocol.md` lines 52–57.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L52-L57

SDK: for VCSEC, `Send` draws a fresh routing address and does not put the UUID in the match key. Other domains reuse the connection address and match the UUID (`dispatcher.go`, lines 400–407). The HTTP proxy serializes all commands for one VIN because “VCSEC commands fail if they arrive out of order” (`pkg/proxy/proxy.go`, lines 477–483).

Passport: one outstanding VCSEC request. Match its replies by the routing address chosen for that request. Do not demux an empty `request_uuid` by “whichever VCSEC call is first in a list” (P4).

### A21. Three BLE connections is a different “three”

Rule: VCSEC can reliably hold at most three simultaneous BLE connections, shared with key fobs and phone keys.

Class: **A**. This is not three replies and not three pipelined commands.

Source: `protocol.md` lines 163–164.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L163-L164

SDK: `Connectable == false` on the matching advertisement returns `ErrMaxConnectionsExceeded` (`ble.go`, lines 302–304). The spec states the limit. The flag test is the official client’s signal.

Passport: a non-connectable advertisement of the right local name means the connection slots are full.

### A22. BLE identifiers and framing

Rule: Service `00000211-b2d1-43f0-9b88-960cebf8b91e`. Write `00000212-b2d1-43f0-9b88-960cebf8b91e` with the stack’s write-with-response API. Read `00000213-b2d1-43f0-9b88-960cebf8b91e`. Local name is `S` + lowercase hex of `SHA1(VIN)[:8]` + `C`. VIN `5YJS0000000000000` advertises `S1a87a5a75f3df858C`. Each BLE message is preceded by a 2-byte big-endian length.

Class: **A**.

Source: `protocol.md` “BLE”, lines 146–161.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L146-L161

SDK: same UUIDs and `VehicleLocalName` (`ble.go`, lines 33–36, 133–137). `Send` prefixes the length (`ble.go`, lines 108–116) and calls `WriteCharacteristic(..., false)` (line 121). `flush` rejects a declared length above `maxBLEMessageSize` 1024 (lines 19–21, 67–73). If the next chunk arrives more than `rxTimeout` (1 s) later, the partial buffer is discarded (lines 27–28, 98–101). The 1 s and 1024 limits are official client behavior. They are not sentences in `protocol.md`.

Passport: write-with-response, 2-byte big-endian length, drop a partial frame after 1 s, reject a declared length above 1024. After CCCD subscribe, follow A22a for MTU.

### A22a. Official client ATT MTU exchange (coordinator re-check addendum, 2026-11-25)

Rule / SDK behavior: after a successful Subscribe on the RX characteristic, the official BLE client calls `client.ExchangeMTU(maxBLEMTUSize)` (`pkg/connector/ble/ble.go`, lines 349–357). On failure it logs a warning and sets `blockLength = ble.DefaultMTU - 3`. On success, `blockLength = min(txMtu, maxBLEMessageSize) - 3`. The RoutableMessage cap remains 1024. This is official client behavior, not a sentence in `protocol.md`, and it is not a requirement to force ATT MTU 247.

Class: **A** (official client), same category as the 1 s partial-frame timeout and the 1024-byte cap in A22.

The first official checklist pass recorded write-with-response, 1024, and 1 s, but did not record this `ExchangeMTU`. The same call is already present at earlier commit `a4b43c1eff0e09d77deb9f2dce97031141fe8c8a`. Third-party text that says “official `ble.go` does not exchange MTU” is false against these objects (see R17).

Passport: after CCCD subscribe, attempt MTU exchange and fall back to the default ATT payload; do not treat 247 as a protocol must; do not raise the 1024 message cap. Aligning product BLE to this official client behavior is a later increment, not part of the current three-item fix.

## 4. Wake, polling, command versus status

### A23. What actually wakes the car

Rule: Over BLE, wake is the authenticated VCSEC RKE action `RKE_ACTION_WAKE_VEHICLE` (enum value 30). `Vehicle.Wakeup` uses that action unless the connector is Fleet API, which calls `api/1/vehicles/<vin>/wake_up`. An Internet wake cannot be authenticated by Infotainment because Infotainment is asleep. A BLE wake is authenticated by VCSEC, which then pokes Infotainment. The CLI therefore handshakes only VCSEC for a BLE wake.

Class: **A**.

Source: `vcsec.proto` `RKEAction_E`, lines 76–82. `pkg/vehicle/vcsec.go` `wakeupRKE`, lines 196–198, called from `vehicle.go` `Wakeup`, lines 259–264. `cmd/tesla-control/commands.go`, lines 145–157.

Passport: the only BLE wake Passport sends is an explicit `RKE_ACTION_WAKE_VEHICLE` on VCSEC. Reading status is not that action.

### A24. Status does not require a wake; actuators do require authentication

Rule, official client: `getVCSECInfo` (vehicle status, whitelist info, whitelist entry) is sent with `AuthMethodNone`. RKE actions, including lock, unlock, and wake, use `v.authMethod`. `body-controller-state` is documented as “Fetch limited vehicle state information. Works over BLE when infotainment is asleep.”

Class: **A** (SDK plus the asleep sentence in `protocol.md` A1). `protocol.md` does not itself say the status read is unauthenticated.

Source: `vcsec.go` `getVCSECInfo`, line 129 (`AuthMethodNone`); `executeRKEAction`, lines 171–193. CLI help `commands.go`, line 1007. Information request enum: `GET_STATUS = 0` (`vcsec.proto`, lines 47–51).

Passport: a VCSEC status snapshot is not proof that an authenticated command succeeded. Do not wake Infotainment in order to learn lock or sleep state. Do not treat an unauthenticated status body as a signed command result.

### A25. Timed background polling just to read Infotainment status is not a Tesla requirement

Rule: Nothing in `protocol.md` authorizes, or requires, a timer that wakes the car to read charge, climate, or other Infotainment state. The official statement is the opposite shape: VCSEC is reachable while Infotainment is asleep (A1), and a VCSEC-only session avoids waking Infotainment (A1 SDK note). Messages may be dropped. A missing response does not prove the command failed, so extra speculative commands are not free (`protocol.md`, lines 142–144 and 770–774).

Class: **A** as the boundary of the spec. The positive “do not poll on a timer” product rule is the third-party evidence in P5 item 1, which agrees with this boundary.

Passport: a timer may issue VCSEC `GET_STATUS` (A24). A timer must not send `RKE_ACTION_WAKE_VEHICLE` or an Infotainment `GetVehicleData` solely because the timer fired while the car is asleep.

### A26. A sleeping car is not implicitly woken

Rule: Wake is a distinct command (A23). The official client does not call `Wakeup` from `BodyControllerState` or `getVCSECInfo`. Infotainment commands that need the computer awake are the caller’s choice to precede with `Wakeup`. The spec does not say “any read wakes the car.”

Class: **A**.

Source: call graph of `getVCSECInfo` / `BodyControllerState` (no wake) versus `Wakeup` (`vehicle.go`, lines 259–264). Domain text, `protocol.md` lines 175–176.

Passport: asleep plus a status refresh stays on VCSEC. Asleep plus a user command that needs Infotainment may send an explicit wake first, and that wake is visible as its own action.

## 5. Errors, refusals, busy, authentication failure

### A27. Two error layers

Rule: `signedMessageStatus.signed_message_fault` is a protocol error. Application errors are inside `protobuf_message_as_bytes` after decryption. An HTTP 200 from Fleet API means the server received a vehicle message, not that the command succeeded.

Class: **A**.

Source: `protocol.md` lines 59–61, 130–136, 776–784.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L776-L784

### A28. Faults whose remediation is written down

Rule: Use the comments on `MessageFault_E` in `universal_message.proto`, lines 30–61. In particular:

- `BUSY` (1), `TIMEOUT` (2): try again.
- `UNKNOWN_KEY_ID` (3): key not recognized. Pair it. SDK maps this, and `SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST`, to `ErrKeyNotPaired` (`error.go`, lines 234–255).
- `INACTIVE_KEY` (4): key disabled.
- `INVALID_SIGNATURE` (5), `INVALID_TOKEN_OR_COUNTER` (6), `INCORRECT_EPOCH` (15), `TIME_EXPIRED` (17): use the included session info (A6) and try again.
- `INSUFFICIENT_PRIVILEGES` (7): role or vehicle state. Not fixed by a new handshake.
- `INVALID_DOMAINS` (8), `INVALID_COMMAND` (9), `DECODING` (10), `BAD_PARAMETER` (13), `IV_INCORRECT_LENGTH` (16): client bug or firmware mismatch.
- `INTERNAL` (11): try again. Often the car is still booting.
- `WRONG_PERSONALIZATION` (12): wrong VIN.
- `KEYCHAIN_IS_FULL` (14): delete a key before adding one.
- `TIME_TO_LIVE_TOO_LONG` (20): expiration too far ahead.
- `REMOTE_ACCESS_DISABLED` (21), `REMOTE_SERVICE_ACCESS_DISABLED` (22), `COMMAND_REQUIRES_ACCOUNT_CREDENTIALS` (23): policy, not a retry of the same BLE frame.
- `REQUEST_MTU_EXCEEDED` (24): client field too large.
- `RESPONSE_MTU_EXCEEDED` (25): the request was received and the response did not fit. SDK `MayHaveSucceeded` is true (`error.go`, lines 198–201).

Class: **A**.

Source: `pkg/protocol/protobuf/universal_message.proto`, lines 30–61.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protobuf/universal_message.proto#L30-L61

SDK retry set (`error.go`, `retriableErrors`, lines 205–214): busy, timeout, invalid signature, invalid token or counter, internal, incorrect epoch, time expired, TTL too long. `ShouldRetry` is false when `MayHaveSucceeded` is true (lines 124–138), so a caller timeout (`vehicle.go` `trySend`, lines 209–212, `PossibleSuccess: true`) is not automatically sent again. Routable `OPERATIONSTATUS_WAIT` is `ErrBusy`, “vehicle busy or finishing wake-up” (`error.go`, lines 33–34 and 257–261).

Passport: session-desync faults go through A6 and then a **new** command counter. Insufficient privileges, inactive key, wrong VIN, and a full keychain stop. A BLE wait that expires is an unknown actuator outcome (A29).

### A29. No response is not a clean failure

Rule: If the client transmits and hears nothing, the car may still have executed the command.

Class: **A**.

Source: `protocol.md` lines 770–774.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L770-L774

Passport: do not send the opposite actuator because a reply timed out. Resolve with a later status read or an explicit user retry that uses a new counter.

### A30. Roles

Rule: Owner can add and remove keys. Driver cannot manage other users’ keys or access-control PINs. Fleet Manager on 2023.38+ cannot add or remove keys and cannot send BLE commands. Vehicle Monitor can read data and cannot change state. Charging Manager can read data and authorize charging commands. Guest is a temporary Driver. Service keys bootstrap pairing and have the roadside limits in the spec. Role powers can change as features are added.

Class: **A**.

Source: `protocol.md` “Roles”, lines 185–220. Enum: `pkg/protocol/protobuf/keys.proto`, `Role`.
https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protocol.md#L185-L220

Passport: a Charging Manager key is not an Owner key. `INSUFFICIENT_PRIVILEGES` on a key-management command is a role refusal, not a session bug.

### A31. Unnamed fault numbers

Rule: The enum also names `REPEATED_COUNTER` (26), `INVALID_KEY_HANDLE` (27), and `REQUIRES_RESPONSE_ENCRYPTION` (28), with no remediation comment.

Class: **A** for the names. No retry policy is adopted beyond recognizing them.

Source: `universal_message.proto`, lines 58–60.

### A32. HMAC command label, if that method is ever used off BLE

Rule: `K' = HMAC-SHA256(K, "authenticated command")`, tag = `HMAC-SHA256(K', metadata || plaintext)`.

Class: **A**. BLE does not use this method (A9).

Source: `protocol.md` lines 719–731. Label in `crypto.go`, line 14.

### A0. FoloToy adds no vehicle rule

Rule: none.

Class: **A** as an absence check.

Source: `FoloToy/ai-passport` `main` `8e9bfaf3c575e5c8f3107ac8cd04b4b1eb294fd1`, search for `tesla`, `VCSEC`, and `vehicle-command` returned no matches.

Passport: hardware and flash rules stay on that baseline. Session behavior does not come from it.

---

# Part 2 — Mandatory avoid-again checks

Each item is something a third-party tree has already encoded, either as a bug, a superseded patch, or a default that fights the official client. Passport’s route is official `.proto` + nanopb + a project-owned thin C session + mbedTLS. `yoziru/tesla-ble` is a comparison oracle, not the runtime. These checks are how that C session must behave.

On a real car, the shared consequence of P1–P4 is: the car emits a legal reply (often the first encrypted reply of a new request, with counter 0, and often one of up to three VCSEC frames), the client drops it, the UI times out, and the actuator may already have run (A29). This pass did not record a new vehicle log of that sequence. The sequence is what the cited code does with the replies `protocol.md` requires the client to accept.

## P1. Response counter 0 rejected because the window is per domain

What it is: `AES_GCM_Response_data.counter` restarts for each request. The first value may be 0 (A14). If one sliding window is stored on the domain peer and is not cleared when a new request is created, the next request’s 0 is “already seen.”

How the live code can do that:

- Official scope is the receiver, not the domain (`session.go` lines 48–58; `receiver.go` lines 36–37). First `Update` accepts 0 (`window.go` lines 58–63).
- `yoziru/tesla-ble` stores the window on the peer: `SlidingWindow response_window_` (`include/peer.h`, line 180). `Peer::validate_response_counter` only calls `response_window_.add(counter)` (`src/peer.cpp`, lines 604–614). `SlidingWindow::add` accepts the first value with no zero check (`include/sliding_window.h`, lines 34–40). The second `add(0)` hits the “already seen” bit and returns false (lines 63–69). The comment on that path logs “Counter N is a replay.”
- The library tries to paper over the domain scope with one reset: `Client::build_universal_message_with_payload` calls `session->reset_response_window()` only when `encrypt_payload` is true (`src/client.cpp`, line 729: “Anti-replay is per-request in official protocol”). `update_session` resets the window only when the epoch bytes change (`peer.cpp`, lines 249–251). A new request in the same epoch does **not** reset inside `Peer`. If the caller validates responses without going through that Client reset, request 2’s counter 0 is a replay of request 1’s counter 0.
- `0Bu/tesla-key-esp32` still calls that peer function after a successful decrypt: `handle_vcsec_frame_` (`main/vehicle_telemetry.cpp`, lines 1257–1267, log “Dropping replayed encrypted VCSEC response”), `handle_carserver_frame_` (lines 1369–1372, log “Duplicate response counter detected”), `handle_empty_vcsec_reply_` (lines 1209–1214), and `authenticate_fault_reply_` (lines 911–914). The peer window is domain-scoped. Their own `BleDispatcher` also has a per-request window (P1b). Both run. The peer window can still drop a fresh 0 if the Client reset did not happen between the two requests.
- Live ADR-0005 does **not** say “reject counter 0.” It says replay windows follow successful authentication, **including counter zero**, and a **repeated** counter, including zero, is dropped (`docs/adr/0005-tesla-ble-seam.md`, the anti-replay rows and the paragraph that begins “An AES-GCM-declared response…”). https://github.com/0Bu/tesla-key-esp32/blob/3a215d60ebcb584ccd558109188d5042582066be/docs/adr/0005-tesla-ble-seam.md
- Live ADR-0003 is **superseded**. It says the old patch only returned early for invalid **non-zero** counters, that plaintext CarServer replies used counter 0 and skipped the check, and that this is not the same thing as cross-request authentication. https://github.com/0Bu/tesla-key-esp32/blob/3a215d60ebcb584ccd558109188d5042582066be/docs/adr/0003-reject-replayed-tesla-responses.md

Class: the official scope is **A** (A13, A14). The domain-scoped window is **B**, and it does **not** agree with official unless every new request resets it before any reply is checked. ADR-0003’s old “ignore zero” behavior is **not** current law.

What it does on a real car: the car is allowed to send counter 0 on the first encrypted reply of a request. A shared domain window drops that frame on the next request. The command then sits until timeout. Lock, unlock, or wake may already have happened (A29). A missing `AES_GCM_Response_data` must not be fed in as integer 0 either: proto3 decodes an absent uint32 as 0, and the official client never calls `antireplay.Update` when the response signature is absent (A17). Doing so rejects a plaintext status or a second plaintext frame as a fake replay of counter 0.

Passport must: allocate the response window on the in-flight request object, with `used = false`. Accept the first counter even when it is 0. Reject only a repeat inside that request. Start the next request with a new window. Match the request before touching any window (A17). If `AES_GCM_Response_data` is absent, do not run the counter window.

## P1b. Copying the third-party window width

What it is: `yoziru/tesla-ble` `SlidingWindow::WINDOW_SIZE = 64` (`include/sliding_window.h`, line 22). `0Bu/tesla-key-esp32` `tk::AntiReplayWindow::kWindowSize = 64` (`main/logic/ble_dispatcher.hpp`, lines 57–60). ADR-0005 records the departure from `crypto.go`’s 32.

Class: **B**. Does not agree with the official client (A16).

What it does on a real car: a reply more than 32 and at most 63 behind the highest counter of that request is accepted by these trees and rejected by `vehicle-command`. Passport would then keep a frame the official client has defined as too old to prove fresh.

Passport must: use 32, from `crypto.go`.

## P2. One request hash and one UUID slot per domain

What it is: `Client` keeps `last_request_hash_vcsec_` and `last_request_hash_infotainment_` (`include/client.h`, lines 170–173) and overwrites them in `build_universal_message_with_payload` (`src/client.cpp`, lines 780–798). UUIDs are the same shape (`store_last_request_uuid_`, lines 1142–1149). The hash buffer used for AES-GCM is 17 bytes (line 781). Official `RequestID` is stored on the receiver for that send (`dispatcher.go`, line 433), so two outstanding Infotainment requests keep two identities.

Class: **B**. The 17-byte AES-GCM layout agrees with A10. The single slot per domain does not agree with A13’s per-request binding once two requests overlap.

What it does on a real car: response AAD includes `TAG_REQUEST_HASH` (A12). After the second request overwrites the slot, the first request’s real reply fails authentication, or it is checked against the wrong tag. Combined with P1, the first reply can also be scored against the wrong counter window. The user sees a timeout or a signature error on a command the car answered.

Passport must: store `type_byte || tag` and the routing address on the in-flight request. Decrypt with that request’s bytes only. A second command must not replace the first command’s identity until the first request is finished or explicitly cancelled.

## P3. Completing a VCSEC command on the first frame

What it is: the spec allows up to three VCSEC replies, and an early `OPERATIONSTATUS_ERROR` is legacy (A19). `tesla-key-esp32` ADR-0005 says this adapter still treats an attributed current-request ERROR as terminal and that this is not exact protocol equivalence.

Class: **B**. Does not agree with the `protocol.md` sentence for new clients. The official Go parser’s whitelist-error return is the narrower SDK behavior in A19, not a reason to fail a lock on the first legacy ERROR.

What it does on a real car: VCSEC can send WAIT, then a specific result, or a legacy ERROR followed by `nominalError` or an empty success. Stopping on the first ERROR reports failure while a later frame of the same request is the real result, or it reports success before the terminal empty message. The car’s actuator state and the UI diverge.

Passport must: keep reading on that request’s routing address until the terminal condition in A19, or until the request deadline. `WAIT` on a non-NFC command is busy, not success and not final failure. An empty non-whitelist message is success. `nominalError` is failure. A bare `OPERATIONSTATUS_ERROR` is discarded while waiting.

## P4. Demuxing VCSEC by “first active slot”

What it is: `tk::BleDispatcher::match_slot_` pass 2, when `uuid_len == 0` and the domain is VCSEC, selects the first active VCSEC slot (`main/logic/ble_dispatcher.hpp`, lines 313–321). Official matching for VCSEC uses the per-request routing address and tells the client not to overlap requests (A20). ADR-0005 says the firmware also checks a request-scoped reply address and allows an empty UUID only in that context. The dispatcher function above, read alone, does not.

Class: **B**. Pass 2 does not agree with A20 if two VCSEC requests are active. The ADR’s extra address check is an attempt to agree with official, and it is still their code, not the spec.

What it does on a real car: VCSEC often omits `request_uuid` (A20). Two overlapping VCSEC calls then deliver both cars’ replies — status and the lock result — into whichever slot was registered first. One command completes with the other command’s body, or a status broadcast closes an actuator request.

Passport must: one VCSEC request at a time, matched by the 16-byte routing address of that request. An empty UUID is acceptable only for that address. A frame for any other address is unmatched and must not update the window (A17).

## P5. Background polling that wakes, or that is easy to call in the waking form

What it is, three live shapes:

1. `tesla_ble_mqtt_core` `read-state.sh` `poll_state` (lines 67–109) sends `body-controller-state` on the polling interval and publishes the Infotainment refresh `read-state-envcheck` only when `vehicleSleepStatus` is the JSON string `VEHICLE_SLEEP_STATUS_AWAKE`. It does not call `wake`. The Docker README states the same policy (`tesla_ble_mqtt_docker` `README.md`, lines 14–18). https://github.com/tesla-local-control/tesla_ble_mqtt_core/blob/696d46bd362cfdc6a3bb263a12e63bb2bfeee449/read-state.sh#L67-L109
   This **agrees** with A24–A26 for the category poll. The comment “without actually waking it” on line 67 is not a measurement (C1).

2. The same core, `tesla-commands.sh` `sendBLECommand` (lines 46–65), **does** wake when the status is not `AWAKE`, via `tesla-control -domain vcsec wake`, then `sleep 7`. That is an explicit wake before a user command, which A23 allows, plus a 7 s pause that is not a Tesla constant (C7). https://github.com/tesla-local-control/tesla_ble_mqtt_core/blob/696d46bd362cfdc6a3bb263a12e63bb2bfeee449/tesla-commands.sh#L46-L65

3. `yoziru/tesla-ble` comparison only. `InfotainmentPollPolicy::update` returns `WAKE_IF_NEEDED` unless the car is observed asleep or the idle timer has passed `sleep_timeout_ms_` (default 660000) (`include/polling_policy.h`, lines 7–9 and 61–78). `WAKE_IF_NEEDED` reaches `initiate_wake_sequence_`, which sends `RKE_ACTION_WAKE_VEHICLE` (`src/vehicle.cpp`, lines 485–515). `vcsec_poll`’s default argument is `WAKE_IF_NEEDED` (`include/vehicle.h`, line 255), but a VCSEC-domain command starts at `initiate_vcsec_auth_` and does not consult that policy (`vehicle.cpp`, lines 269–274 and 441–446). Infotainment polls do consult it (`initiate_infotainment_auth_`, lines 453–471). Follow-up polls after a successful command pass `NO_WAKE_SKIP` (`vehicle.cpp`, lines 724–738). The bool overloads map `force_wake == false` to `NO_WAKE_SKIP` (`vehicle.cpp`, lines 30–32).

Class: **B**. Item 1 agrees with official for “do not wake just to read Infotainment state.” Item 3’s `WAKE_IF_NEEDED` while the car is merely “not yet classified asleep” does not agree with A26. The library is not the runtime.

What it does on a real car: `WAKE_IF_NEEDED` transmits `RKE_ACTION_WAKE_VEHICLE`, which is the command that wakes Infotainment (A23). A 10 s or 30 s Infotainment poll using that policy keeps the car awake. `NO_WAKE_SKIP` on an observed-asleep car skips the Infotainment read (`vehicle.cpp`, lines 454–458) and leaves the car asleep. The MQTT category poll does the same skip unless the VCSEC status string is `AWAKE`.

Passport must: status timers use VCSEC `GET_STATUS` only. They do not call wake. They do not call Infotainment `GetVehicleData` while VCSEC sleep is not a fresh `AWAKE`. A user command that needs Infotainment may send an explicit wake, as its own action, and then the command. Do not take `vcsec_poll()` / `infotainment_poll()` defaults from tesla-ble. Do not take the 660 s or 7 s numbers as protocol (C4, C7).

## P6. Treating “not AWAKE” as permission to ignore a command result

What it is: `poll_state` treats every `vehicleSleepStatus` other than the exact string `VEHICLE_SLEEP_STATUS_AWAKE` as asleep, including a missing field (`read-state.sh`, lines 95–100). The enum’s zero is `VEHICLE_SLEEP_STATUS_UNKNOWN`, not `ASLEEP` (`vcsec.proto`, lines 263–267). `tesla-key-esp32` `main/logic/wake_poll.hpp` lines 39–41 says a `NO_WAKE_SKIP` poll does not request a wake, and that Infotainment traffic while the sample is `UNKNOWN` or `AWAKE` “requires measurement and is not proven.”

Class: **B** for the control flow. The physical effect of an Infotainment read during `UNKNOWN` is **C** (C2).

What it does on a real car: skipping an Infotainment poll when the field is missing avoids a wake, which matches A26. Showing the car as asleep, or skipping a user command, because the field was absent, mis-states `UNKNOWN`.

Passport must: missing sleep status is unknown. Do not wake. Do not display “asleep” or “unlocked” from a missing field (App1, App2).

## P7. Write Command, 3 s reassembly, 2048-byte frames, non-connectable connect

What it is: ADR-0005 in `0Bu/tesla-key-esp32` records these as departures from `vehicle-command`:

- TX is ATT Write Command, while `protocol.md` and `ble.go` use write-with-response.
- Inter-chunk timeout is configured to 3000 ms. Official `rxTimeout` is 1 s. Their `RxFramer` default is still 1000 ms.
- Max frame 2048. Official `maxBLEMessageSize` is 1024.
- A named advertisement may be dialed when the connectable flag is false. Official `ble.go` returns `ErrMaxConnectionsExceeded`.

Class: **B**. They do not agree with A21 and A22. The ADR says so.

What it does on a real car: this pass did not measure it. The ADR itself marks Write Command field notes and several link behaviors as not hardware acceptance (C3). The failure mode the ADR describes for Write Command is that a rejected ATT write looks like a response timeout, which then looks like A29 (unknown execution).

Passport must: write-with-response, 1 s partial-frame drop, 1024-byte maximum, refuse a non-connectable advertisement of the matching local name.

## P8. `force_update_session` lowering the command counter

What it is: official `Signer.UpdateSessionInfo` never stores a smaller command counter (`signer.go`, lines 109–112). `yoziru/tesla-ble` `Peer::update_session` also refuses a lower counter when the epoch did not change (`peer.cpp`, lines 202–206) and only raises the counter (`lines 217–219`). `Peer::force_update_session` assigns `session_info->counter` directly (`peer.cpp`, lines 263–291) and its header says it bypasses anti-replay (`include/peer.h`, lines 90–100).

Class: **B**. `force_update_session` does not agree with the official signer.

What it does on a real car: a lower command counter in the same epoch is what VCSEC is specified to reject as out of order (A7) and what the fault `INVALID_TOKEN_OR_COUNTER` is for (A28). The next commands fail authentication until a new handshake happens to move the counter forward again.

Passport must: the command counter moves only upward inside an epoch. Do not add a force path that copies a lower `SessionInfo.counter` over the local one.

## P9. Superseded “reject replayed responses” text

What it is: conversations that still quote ADR-0003 as “drop counter 0” or “drop every replay before UUID matching” are behind the file. The live file’s status line is “superseded by ADR-0005.” Its historical decision applied to tesla-ble v5.1.1–v5.1.3. Patch 0001 is retired. It says not to confuse duplicate delivery, cross-request authentication, and plaintext routing.

Class: **B** as a document that still exists, and **C** if an older memory of it is used as a rule.

Passport must: follow A13–A17 and P1. Do not reintroduce a check whose condition is `response_counter == 0`.

## P10. Single-threaded vehicle access

What it is: `tesla_ble_mqtt_core` `documentation/how_polling_works.md` lines 6–7 says overlapping `tesla-control` processes locked the container, so every vehicle call is queued onto one listener. Official proxy locks per VIN for the same family of reason (`proxy.go`, lines 477–483).

Class: **B**. Agrees with A7 and A20.

What it does on a real car: overlapping VCSEC commands arrive out of order or share a reply address, so a lock and a status read complete against each other’s frames (P4) or the car rejects the counter (A7).

Passport must: one BLE transaction at a time toward a given car, and never two VCSEC requests.

---

# Conflicts (official wins)

1. Response-window width **32** (`crypto.go`) versus **64** (`sliding_window.h`, `ble_dispatcher.hpp`). Adopt 32.
2. Write-with-response (`protocol.md` lines 151–152, `ble.go` line 121) versus ATT Write Command (ADR-0005). Adopt write-with-response.
3. Partial-frame timeout **1 s** (`ble.go`) versus **3000 ms** (ADR-0005 configuration). Adopt 1 s.
4. Max message **1024** versus **2048**. Adopt 1024.
5. Non-connectable advertisement is “slots full” (`ble.go` lines 302–304) versus “dial anyway” (ADR-0005). Adopt the official refusal.
6. New clients discard bare `OPERATIONSTATUS_ERROR` and wait (`protocol.md` lines 871–874) versus treating it as terminal (ADR-0005; and, for whitelist codes only, `vcsec.go` lines 52–58). Adopt the spec sentence for non-whitelist commands.
7. Command counter never decreases in `UpdateSessionInfo` versus `force_update_session`. Adopt the official signer.
8. VCSEC command counters must arrive in order (`protocol.md` line 550) versus one shared verifier window for every domain (`verifier.go` lines 228–233). As a client, send VCSEC commands in order and do not overlap them. The verifier’s window is not permission to reorder.
9. Response anti-replay is per request (`receiver.antireplay`) versus `Peer::response_window_` plus a reset that the caller must remember (`client.cpp` line 729). Adopt a window that is born with the request.
10. (Coordinator re-check, 2026-11-25) Official `ExchangeMTU` after Subscribe, with `DefaultMTU - 3` fallback (`ble.go` lines 349–357 at `0a582da` and `a4b43c1`), versus third-party “official does not exchange MTU.” Adopt the official client. Do not force ATT MTU 247, and do not raise the 1024 message cap.

---

# Class C — not norms

**C1.** The MQTT comment that `body-controller-state` checks sleep “without actually waking” (`read-state.sh` line 67). No capture in that repo proves the radio effect. The adoptable part is A1, A24, and the verified branch that skips the category poll unless the status string is `AWAKE` (P5 item 1).

**C2.** Whether an Infotainment read while VCSEC sleep is `UNKNOWN` wakes the car. `wake_poll.hpp` lines 39–41 say this needs measurement.

**C3.** ADR-0005 field notes (Write Commands “accepted in field operation,” truncation “plausible, not proven,” SMP, pending in-car tests). The ADR marks them as not hardware acceptance.

**C4.** Poll intervals 30 s, 660 s, 3600 s in the Docker README, and `sleep_timeout_ms_ = 660000` in `polling_policy.h`. Operational advice. Not a Tesla limit in the sources above.

**C5.** `yoziru/tesla-ble` `Vehicle` queue, default `WAKE_IF_NEEDED`, and follow-up poll timing (1500 ms in `schedule_follow_up_poll_`). Comparison only. Not Passport runtime. The avoid-again rule is P5, not a port of `vehicle.cpp`.

**C6.** Any memory of ADR-0003 that is not the current superseded file (P9).

**C7.** The 7 s `sleep` after wake in `tesla-commands.sh` line 65, and `PK_WAKE_SETTLE_MS` 7000 in the product header. Both are local pauses. The product comment already says the constant is not a safety TTL. No Tesla document in this pass states 7 s.

**C8.** Faults 26–28 have names and no written remediation (A31).

---

# Application-layer schema, not session rules

`protocol.md` lines 3–7 put application payloads out of scope. These are official protobuf facts. They are not anti-replay rules.

**App1.** `vcsec.proto` is `syntax = "proto3"`. `VehicleLockState_E` is `UNLOCKED = 0`, `LOCKED = 1`, `INTERNAL_LOCKED = 2`, `SELECTIVE_UNLOCKED = 3` (lines 244–249). `VehicleStatus.vehicleLockState` is field 2 (line 281). https://github.com/teslamotors/vehicle-command/blob/0a582da430c4aca6d397fb6cbed5b4d6bb380125/pkg/protocol/protobuf/vcsec.proto#L244-L249

Proto3 does not encode a zero default. A missing `vehicleLockState` decodes as `UNLOCKED`. Passport must not show “unlocked” unless the field was present on the wire.

**App2.** `ClosureState_E.CLOSED = 0` (line 235). `VehicleSleepStatus_E.UNKNOWN = 0`, `AWAKE = 1`, `ASLEEP = 2` (lines 263–267). `UserPresence_E.UNKNOWN = 0` (lines 273–277). A missing closure decodes as closed. A missing sleep field decodes as unknown, not asleep and not awake.

**App3.** `INFORMATION_REQUEST_TYPE_GET_STATUS = 0` (line 48). Combined with A24, the official client sends that request with no authenticator. An all-zero decode can look like “unlocked, closed, unknown sleep” when the fields were simply absent.

---

# Summary 1 — rules that can be adopted

From official Tesla, with third-party items only where they restate it:

1. Two domains, two handshakes, two clocks, two command counters. VCSEC works over BLE while Infotainment is asleep. Do not open Infotainment to learn lock or sleep.
2. The vehicle generates the epoch. The client generates an unpredictable 16-byte handshake UUID, a 12-byte nonce, and a per-VCSEC-request routing address. `K = SHA1(X)[:16]`. Install `SessionInfo` only after the session-info HMAC, a recent UUID (4 s on the official BLE client), and a same-epoch clock that did not go backward. Never decrease the command counter inside an epoch.
3. BLE commands are AES-GCM. Set `FLAG_ENCRYPT_RESPONSE` every time. Request AAD omits flags when zero. Response AAD always includes response flags, the response fault, and `AES_GCM_Response_data.counter`.
4. Request identity is one signature-type byte plus the tag. AES-GCM identities are 17 bytes. Store that identity on the in-flight request.
5. Response anti-replay is a new 32-deep window per request. The first counter, including 0, is accepted. A repeat on that request, including a second 0, is rejected. Out-of-order replies inside the window are accepted. Older than 32 are rejected. An unmatched reply updates nothing. A missing `AES_GCM_Response_data` is plaintext, not counter 0.
6. VCSEC may send three replies. One VCSEC request at a time. Empty non-whitelist message means success. `nominalError` means failure. `WAIT` means busy except during NFC pairing. Discard a bare legacy `OPERATIONSTATUS_ERROR` and keep waiting. At most three BLE connections. Write-with-response, 2-byte length, 1 s partial timeout, 1024-byte max, local name `S + hex(SHA1(VIN)[:8]) + C`. After subscribe, the official client exchanges MTU and falls back to the default ATT payload if that fails (A22a); 247 is not a protocol must.
7. Wake is explicit `RKE_ACTION_WAKE_VEHICLE` or Fleet `wake_up`. VCSEC status is unauthenticated and does not wake. A timer must not wake the car, and must not poll Infotainment, merely to refresh status. A missing reply may mean the actuator ran.
8. Retry the session-desync faults in A28 after A6, with a new command counter. Do not retry a possibly executed actuator as if the timeout proved failure. Pairing faults and privilege faults stop.

Do not adopt: a domain-level response window, “reject counter 0,” window width 64, one hash slot per domain, ATT Write Command, 3 s reassembly, 2048-byte frames, dialing a non-connectable advertisement, `force_update_session`, tesla-ble’s `WAKE_IF_NEEDED` poll default, or the 7 s / 660 s timers.

## Summary 2 — still unverified

1. FoloToy contains no vehicle-interaction rule. The absence is verified.
2. Whether VCSEC `GET_STATUS` on an asleep car has any physical wake effect beyond the official statement that VCSEC is reachable while Infotainment is asleep.
3. Whether an Infotainment read during `VEHICLE_SLEEP_STATUS_UNKNOWN` wakes the car.
4. The on-air connectable flag, Write Command versus write-with-response, and the ADR-0005 field anecdotes. Not measured here. Official text already chooses write-with-response and the connectable-flag refusal.
5. Numeric pauses: 7 s, 660 s, 30 s, 10 s, 1500 ms follow-up, 5 s SDK command lifetime (`dispatcher` `defaultExpiration`). None of these is a `protocol.md` limit.
6. Remediation for fault codes 26, 27, and 28.
7. A real VCSEC’s command-counter check versus the Go verifier’s shared 32-wide window. The client obligation is the spec’s in-order rule. No vehicle was observed.
8. Which `VehicleStatus` fields a given car transmits on a given day. The proto3 zero defaults are specified. The bytes on the air are not in these repositories.
9. The five executable constraints were written into the root README by `4b1bd25`. This file is the complete verification checklist and the record of what was not accepted. When the original cloud checklist was written, the documents had not yet been changed. This item is closed in the adjudicated edition.


---

# Sources included in this adjudicated edition

| Material | Time (CST) | Role | How the coordinator used it |
| --- | --- | --- | --- |
| Official cloud checklist [bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be) | 2026-11-25 | A authority base text | Part 1 / Part 2 kept almost verbatim |
| Official re-review of `1377473` [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc) | 2026-11-25 | Official code unique review | FAIL: two medium items plus residual omitted lock state; this sets the current fix scope |
| Antigravity first draft `tesla_interaction_spec_checklist.md` | 2026-11-25 | Third-party research, incomplete | The trunk may be used as a reference; it lacks a standalone pitfalls chapter and is not a complete checklist |
| DeepSeek flash checklist | 2026-11-25 | Read-only comparison | The trunk agrees; the product HMAC length was not checked; the status-page wake judgment is not accepted as final policy |
| DeepSeek v4-pro re-check (item-by-item rewrite of flash) | 2026-11-25 | Read-only comparison, better than flash | Accept its corrections of flash and its checks of the product HMAC, window origin, and wraparound; do not accept status-page wake as an acceptable policy |
| Antigravity detailed pitfalls checklist `tesla_protocol_and_pitfalls_detailed.md` | 2026-11-25 | Third-party supplement of pitfalls already hit | The trunk agrees with official; extra embedded avoid-items are kept for later and do not expand this round's fix |
| Cite-style long report *Vehicle-interaction specification and protocol verification checklist report* | 2026-11-25 | Third-party survey; citations lean on transcription | The trunk may be used as a reference; several places write B/C as A; see the not-accepted table below |
| Word *Read-only verification checklist for the vehicle-interaction specification (against the five reference repositories named at project start)* | 2026-11-25 23:22 | Third-party read-only comparison (could not read the product repository at the time, and did not read session.go / ble.go directly) | Most of the trunk agrees with official; P-01-P-12 can serve as an index of pitfalls already hit; "disconnect when finished," demoting window 32 / 1024 to unverified, and treating all of section 5.1 as a coding gate, see R13-R16; "official does not exchange MTU" and the mistaken citation of `session.go:43-53` as the window, see R14 and R17 |
| Coordinator re-judgment of the original text, 2026-11-25 23:50 | 2026-11-25 23:50 | The coordinator personally compared the official live objects and did not assume the first cloud audit was necessarily correct | Core A items still hold; A22a was added; R14 was corrected and R17 was added; development principles and the follow-up research plan are at the end of this file |

Live repository objects remain those in the Sources table at the top of this file. `yoziru/tesla-ble` is comparison only, AGPL, and is not the runtime.

---

# Product gap and fix scope for this round (coordinator adjudication)

Comparison node: `1377473`. For the class of bug in which a domain-level shared window treats a new request's `counter==0` as a replay, that commit already allocates the window from the 17-byte request hash. The direction is correct. The first unique review once passed. After re-review against this checklist it fails because of pre-existing medium issues.

The three items that must be changed now, and must not be expanded:

1. On timeout or no reply, do not clear an already-known lock state to unknown (the `expire_pending` problem on the LOCK_VERIFY / UNLOCK_VERIFY / unlock-timeout paths). No reply is not failure, and it must not make the UI become "look up the lock state."
2. When opening the status page, if the car is already known to be asleep, do not first send `RKE_ACTION_WAKE_VEHICLE`. That is an implicit wake in order to read status. The official re-review judges it a medium FAIL under A23 / A24 / A26.
3. An omitted proto3 `vehicleLockState` must not be shown as unlocked. When the field is absent, the default 0 must not be treated as a checked UNLOCKED.

Explicitly deferred, and not touched this round: aligning window width 64 with the official 32 (the wrong comment must be changed; whether to narrow to 32 is a separate security review); the domain-window fallback path must be checked so that every signed request has already called `note_req_id`, but a request that has already been identified must not take the fallback; comparison and alignment of product BLE with official A22a (`ExchangeMTU` after subscribe, fall back on failure). Do not flash `1377473` or later fixes that have not passed unique review, unless the user separately and explicitly authorizes it.

---

# Part 3 — Adjudication of third-party research reports (including what is not accepted)

Classification follows the official checklist: A is official truth, B is third-party evidence that has been checked and must not override A, and C is unverified or a local timer and must not be treated as a protocol norm.

"Accepted" below means it may be written into a later avoid-again check or into a product comparison, and it does not conflict with official. "Not accepted" means it must not again be treated as official specification, must not be used to expand this round's fix, and must not be written back into the consensus document as an official requirement that must be carried out. The item itself must be kept, so later agents can see that it was already rejected.

## 3.1 Accepted (agrees with official, or is evidence of a pitfall already hit against official)

1. Response anti-replay must be bound to the same request. The first packet, including `counter==0`, is accepted. Only a repeat on the same request is rejected. Out-of-order is allowed only inside the window. The official window width is 32, not 64. Third-party flash / v4-pro / the AG detailed checklist agree with official Part 1 A13-A16. Accepted.
2. A domain-level shared window misjudges a new request's legal first packet 0 as a replay. This is the root-cause class of that same problem, and it is official P1. Product `1377473` points in the right direction. Accepted.
3. The command counter and the response counter are not the same counter. Do not compare the response counter with the transmit or session counter. Accepted (v4-pro P-02, official A7).
4. VCSEC returns at most about three frames. Do not treat the first frame as terminal. Send only one VCSEC request at a time. An empty `request_uuid` must still be attributable, commonly by a 16-byte routing address. A missing reply may already have been executed. A state-changing command must not be automatically resent because of a timeout. ACK / OK does not mean already executed. Accepted.
5. Two domains, two sessions. A lock-only action, a trunk action, or a VCSEC status read must not handshake Infotainment in order to do those things. The valid meaning of official issue 468 is this sentence, not "official forbids all polling." Accept the correct part.
6. Wake must be explicit: `RKE_ACTION_WAKE_VEHICLE` or Fleet `wake_up`. VCSEC remains reachable while Infotainment is asleep. `GET_STATUS` may be unauthenticated and must not be used as a wake method. Background auto-refresh must not wake. Accept official A23-A26. Third-party `NO_WAKE_SKIP` / 660 s may be referenced as B/C policy and must not be labeled A.
7. The SessionInfo HMAC must be a full 32 bytes and compared in constant time. v4-pro's check is more accurate than flash: the product already does this. The 0Bu pinned library accepting a 16-byte prefix is a 0Bu departure and does not apply to the product. Accept v4-pro and correct flash.
8. The origin of the 64-wide window is yoziru `sliding_window.h` `WINDOW_SIZE=64`. 0Bu inherited it and the product followed. The product header that writes 64 as official `window.go` is wrong. Accept v4-pro. The official value is still 32. This round only records that and does not change the width first.
9. The product rejects a same-epoch `clock_time` rollback outright. RNG failure is fail-closed. A request counter of `UINT32_MAX` is rejected and is not wrapped to zero. v4-pro lines up with the official signer. Accepted, and these are already aligned, so they are not changed as defects of this round.
10. Product chunking of 1000 ms / 1024 B and a 4 s handshake (`PK_SESSION_CHALLENGE_MS=4000`) agree with the official BLE reference client. 0Bu's 3000 ms / 2048 B is a deliberate departure. Do not copy it without on-device evidence. Accepted.
11. A plaintext reply has no encrypted response counter. Do not feed a plaintext 0 into the encryption window. 0Bu's "one plaintext, one request" may be stricter and does not conflict with official. Accept the distinction. Do not write the stricter rule as an official obligation.
12. Avoid-items in the AG detailed checklist that agree with the official trunk are accepted. Among them, routing-address rotation, SessionInfo public-key binding, AddKey PRESENT_KEY framing, nanopb `has_` versus a zero value, and NimBLE / ESP32-C3 memory and disconnect cleanup are kept as later embedded avoid-items. They are not part of this round's three-item fix.
13. The proto3 omitted-field zero trap for lock state, closures, and sleep is accepted as official App1-App3. Lock-state display must be changed this round.
14. `FLAG_ENCRYPT_RESPONSE` is a bit index. The mask must be `1 << 1 = 2`. Writing 1 actually sets `FLAG_USER_COMMAND`, and on a real car the common result is Fault 28 (Issue #390). Response AAD must include `TAG_FLAGS` even when `flags==0`. This agrees with the official Part 1 summary. It is an implementation constraint, not one of this round's three items.
15. `TAG_COUNTER` in the response-decryption AAD must use the response's own `AES_GCM_Response_data.counter`, and the GCM tag must be fully checked before the counter enters the window. yoziru v5.2.0 once used the request counter by mistake and skipped the tag check. That was fixed only in v6.0.0 / 0Bu patch 0006. This agrees with official A12. P-02 of that Word report is accepted.
16. Do not heuristically search for a frame header on the BLE receive stream. If the length is illegal or above 1024, discard and reset. After connect, drop 31/41-byte passive authentication frames that do not match this endpoint's routing address (official Issue #390; that report's P-05). Keep this as a later embedded avoid-item. It does not enter this round's three items.
17. The official maintainer confirmed in Issue #368: when the vehicle is not AWAKE, `BodyControllerState` charge-port state is constantly reported closed, and some fields are only latches from before sleep. The UI must not treat "closed" at that moment as the current truth of plug-in or of open versus closed. This is a later UI rule and does not enter this round's three items. This round changes only the omitted lock state.
18. Issue #366: after an explicit wake, do not immediately handshake Infotainment. The vehicle may not be ready and may return Fault 11 or wait until timeout. An Infotainment handshake failure during sleep must also not tear down a BLE link that is still usable. The wait step is Class C, to be calibrated on a real car later, and does not enter this round.
19. Do not automatically delete and recreate the local private key because of a remote VCSEC fault or an error string. RNG failure must be fail-closed. This agrees with the 0Bu guide / patch 0007 and with item 9 above. Storage transactions and public-key binding are later items. Do not reopen this round's NVS plan (see R6).
20. Do not copy the Charging Manager role that 0Bu or TeslaBleHttpProxy use by default. Official roles are A30. If Passport needs the locks and the front and rear trunks, choose the role by the product's minimum necessary privilege. This is a pairing constraint and does not enter this round's three items.
21. WAK-06 of that Word report agrees with the official re-review: do not implicitly wake in order to read status, and opening a page defaults to VCSEC only. This point is accepted, and it is the contrast with R1. DeepSeek treated one wake on the status page as an acceptable policy. This report did not follow that error.


## 3.2 Not accepted, or downgraded (must be kept, so they are not treated as specification again)

### R1. Waking once when the status page is opened is an acceptable product policy

Where it appears: DeepSeek flash; v4-pro B-38, X-06, and 5.2 "explicitly a user action"; some summaries write one wake on the status page and no wake in the background as if they do not contradict.

Why not accepted: the official re-review [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc) already judged a medium FAIL under "reading status must not implicitly wake." The user opens the status page in order to read status, not to issue the action "wake the vehicle." The explicit wake in official A23 is `RKE_ACTION_WAKE_VEHICLE` or Fleet `wake_up`. A24 / A26 state that a status read must not wake a sleeping car for that purpose. This round must change that product path. "Do not wake in the background" still holds, but it cannot excuse an implicit wake on the status page.

### R2. Writing issue 468 as "official forbids high-frequency polling of a sleeping car" and labeling it A

Where it appears: cite-style long report 1.5, and item 06 of section 5.1.

Why not accepted: issue 468 says an HTTP proxy must not handshake both VCSEC and Infotainment for an arbitrary command, lest a VCSEC command that could have worked on its own be dragged down while Infotainment is asleep. That agrees with A1 and A23 and should be accepted. The official text does not write "forbid high-frequency background polling" as a protocol article. Not waking in the background is a third-party field B policy, and a product power policy. Official silence is not an official interval, and it must not be labeled A. The correct official statement is: wake must be explicit; do not wake, and do not poll Infotainment, in order to refresh status.

### R3. Writing "BLE must be disconnected when a command ends / `ble_gap_terminate` after a 15-second hard timeout" as an official requirement

Where it appears: cite-style long report 1.4, 2.4, and item 07 of section 5.1; the Word read-only checklist's BLE-04, P-08, and section 5.1 wording "disconnect when finished" (see R13).

Why not accepted: vehicle-side VCSEC can hold at most about 3 concurrent BLE connections. That is an official fact (A21 of this checklist, issue 469). Leaving a connection up without releasing it occupies a slot. A third-party Docker image killing the process on timeout is a Class B lesson and may be recorded. Official text does not require a key client to disconnect after every action, and it does not specify a 15-second hard timeout. Passport is a key. Keeping the connection can be a product policy. Do not write "must disconnect when finished" into the consensus document as an official obligation, and do not include it in this round's fix.

### R4. Treating 0Bu's 16-byte SessionInfo HMAC prefix as a gap the product must also fix

Where it appears: DeepSeek flash's transcription of 0Bu B items, without checking the product tag length.

Why not accepted: v4-pro already checked that the product requires `tag_len == 32` and a constant-time compare. The 0Bu departure does not apply to the product. Keep 32 bytes. Do not adopt the prefix. This flash point has already been corrected by v4-pro. Later work follows v4-pro and this adjudication.

### R5. Writing the product window width 64 as the official value, or writing "this round must change it to 32" as scope already under repair

Where it appears: a comment in the product header; some transcriptions do not distinguish "official value" from "this round's fix scope." v4-pro correctly identified the origin and the comment error.

Partly accepted, and not accepted as scope: official is 32. 64 is a wider departure. The comment must be changed. A security review then decides whether to narrow. This is not one of the current three fixes. Do not use this round to change the window width, and do not keep writing 64 as official `window.go`.

### R6. Writing NVS dual-page rotation, CRC32, and "forbid Safe Mode" as interaction specification that must land immediately

Where it appears: cite-style long report 2.3.

Not accepted as protocol specification: this is a 0Bu / ESP32 storage-layer pitfall, not a `vehicle-command` session rule. The product session is currently RAM-only and re-handshakes on every connection, so the three official recovery conditions for cache aging do not apply. The storage layer may later refer to dual pages and checksums. That does not enter this round's interaction fix, must not be labeled A, and must not require the NVS plan to be changed now.

### R7. Automatically resending a state-changing command on timeout, BUSY, or `INVALID_TOKEN_OR_COUNTER`

Where it appears: the "automatic recovery policy" in cite-style long report 1.6, for example reconnecting after TIMEOUT, or resending after updating the counter on `INVALID_TOKEN_OR_COUNTER`.

The blanket automatic resend is not accepted. Official A29 states that when no reply is received the actuator may already have executed, so a timeout must not be treated as a clean failure and a state-changing command must not be automatically resent. Session-desync faults may, under A28, after the A6 session update is complete, be signed again with a new command counter. That is not the same thing as sending once more, because of a timeout, an actuation that may already have executed. Pairing failure and insufficient privilege should stop, not retry.

### R8. Writing timers such as 7 s, 660 s, 5 s, 15 s, and 30 s as official specification

Where it appears: several third-party summaries place field timers next to the official BLE 1 s / 4 s; the cite report's 15 s disconnect; local-control / yoziru's 660 s; section 5.1 of the Word read-only checklist writes "completely disable background timed polling" as a baseline that can be adopted directly.

Not accepted as A. The timers with an official basis are mainly the BLE chunk timeout of 1 s, the official BLE client's session-info age of 4 s, and the maximum frame of 1024. The 7 s wake settle, the 660 s sleep-poll floor, the 5 s command budget, and the 15 s disconnect are Class C or Class B product or third-party policy. They may be referenced and must not be labeled official.

### R9. Treating the Antigravity first draft as an already comprehensive interaction-specification checklist

Where it appears: AG's first delivery, `tesla_interaction_spec_checklist.md`.

Not accepted as a complete checklist: the draft lacks a standalone chapter of third-party pitfalls already hit, and it does not write "a domain-shared window treats a new request's counter 0 as a replay" as a check that must be avoided again. The hardware H items remain a hardware contract, not the vehicle protocol. After that draft, the official cloud checklist is the base, and this Part 3 collects later research reports.

### R10. Relying on an expired or remembered ADR-0003 to say the current 0Bu specification is "reject counter 0"

Where it appears: expired retellings after the project started. Official checklist P9 already notes that ADR-0003 has been superseded.

Not accepted: the current ADR-0005 does not write "reject counter 0." It writes that the window after successful authentication includes counter 0, and that a repeat, including a repeated 0, is dropped. Do not use the superseded text again.

### R11. Writing practices that are stricter than official as official requirements

Where it appears: the cite report and some transcriptions write a mandatory 16-byte routing address, a mandatory disconnect at the end of a command, a persistent-counter scheme, and similar items as A.

The promotion is not accepted. The product may be stricter. A mandatory routing address with an optional UUID is compatible with the official VCSEC empty UUID, and that is an acceptable product choice. Do not write back "official requires the UUID to be present" or "official requires disconnect when finished." What official requires is Part 1.

### R12. Using a Google transcription link and "[cite: N]" in place of a line-number anchor, and labeling a transcription conclusion A directly

Where it appears: the whole cite-style long report.

That citation form is not accepted as a basis for verification. Rules in it that agree with the official original text may be kept, but verification must return to paths and line numbers on the live `vehicle-command` object: this file's Part 1, and the v4-pro items that carry line numbers. Where the transcription adds, promotes, or omits, this not-accepted table wins.

### R13. Writing "connect briefly on demand, disconnect when finished" and a mandatory disconnect after each BLE step as an official baseline that can be landed directly

Where it appears: Word read-only checklist BLE-04, P-08, and item 2 of section 5.1.

Not accepted as an official obligation. Same as R3. At most about 3 concurrent BLE connections on the vehicle is an official fact. After an iPad or similar device fills the slots, the symptom is "cannot wake," and a hung low-level connection needs a hard timeout. Those are Class B lessons from that report and from tesla_ble_mqtt_docker / TeslaBleHttpProxy, and they may be recorded. Official text does not require a key to disconnect after every action, and it does not specify an idle-disconnect number of seconds. That report itself, in 5.2, also lists idle-disconnect duration as Class C pending real-car calibration. Passport keeping the connection can be a product policy. This round does not change connection policy.

### R14. Because this report did not read `session.go` / `ble.go`, demoting the official window width 32 or the 1024-byte maximum frame back to "still pending re-check"

Where it appears: the opening boundary of the Word read-only checklist, WIN-04, CNF-03, and 5.2; and that report writing `internal/dispatcher/session.go:43-53` as "the precise windowSize = 32 and the sliding-window implementation."

The demotion is not accepted, and the line numbers are corrected. On 2026-11-25 23:50 the coordinator personally checked `0a582da` and the earlier `a4b43c1`. `windowSize = 32` is line 21 of `internal/authentication/crypto.go`. The comment states that `Verifier.windowSize` is `uint64`, so the width must be at most 64. The algorithm is in `internal/authentication/window.go`. Per-request use is `session.decrypt` calling `handler.antireplay.Update`. In both of those commits, `session.go:43-53` is the start of `newSession` / `decrypt`, not the window constant and not the sliding-window implementation. Do not use this unread boundary, or the wrong line numbers, to deny an official value that has already been checked. This round still does not change the product window width of 64.

### R15. Because the product repository could not be read at the time, treating "the gap between `1377473` and the specification has not been checked" as a question that is still open for us

Where it appears: the opening of the Word read-only checklist, and the first item of 5.2.

Not accepted as the current state. That report truly did not read the product code, so it cannot conclude about the C implementation of `1377473`. On our side there is already a first unique review that passed ([bc-e5fb9eb2](https://cursor.com/agents/bc-e5fb9eb2-8183-58be-bf29-93cd3379d170)) and a re-review against this checklist that failed ([bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc)). The current gap is "Product gap and fix scope for this round" in this file. Do not use "repository not accessible" to say the three fixes have not yet been compared.

### R16. Treating all of section 5.1, "verified, may be adopted directly, may be coded directly," as a merge gate

Where it appears: section 5.1 of the Word read-only checklist. The exported text of that section also contains damaged `[cite:]` markers. Section 5.2 writes an over-long TTL as Fault 209.

The force of that whole section is not accepted. Items that agree with official have already been accepted separately. The mixed-in "disconnect when finished" and "completely disable background timed polling" must not be official gates. See R13, R2, and R8. Verification must still return to repository paths and the line numbers in Part 1 of this file, not to end-of-document numbers or damaged cite markers. The official enum for an over-long TTL is `TIME_TO_LIVE_TOO_LONG` (20). 209 is a slip of the pen and must not be treated as a new fault code.

### R17. Writing "official `ble.go` does not initiate an ATT MTU exchange by default" as official behavior

Where it appears: the Word read-only checklist's still-to-verify item about ESP32-C3 NimBLE / MTU. Its basis is close to 0Bu ADR-0005's retelling of an earlier official client, and it names `ble.go` around line 349.

Not accepted. At the authority commit `0a582da`, and at `a4b43c1`, which that report suggested re-checking, lines 349-357 of `pkg/connector/ble/ble.go` are `ExchangeMTU(maxBLEMTUSize)` immediately after subscribing to RX. On failure, fall back to `ble.DefaultMTU - 3`. On success, `min(txMtu, 1024) - 3`. The first official cloud audit recorded write-with-response, 1024, and the 1 s fragment timeout, and missed this passage. That is a gap in that audit, not "official does not exchange MTU." 247 is still not a `protocol.md` obligation, and the maximum frame must not be raised to 2048. Whether the product already does this is a later comparison and is not part of this round's three-item fix.

## 3.3 Corrections of DeepSeek flash relative to v4-pro (accept v4-pro)

If only the flash edition is seen later, the following follow v4-pro and this adjudication:

1. The product SessionInfo HMAC is already a full 32-byte constant-time compare. Flash did not check the product tag length.
2. The origin of the 64-wide window is yoziru, not official.
3. The product rejects a same-epoch `clock_time` rollback. RNG failure is fail-closed. The `UINT32_MAX` guard corresponds to the official signer.
4. `PK_SESSION_CHALLENGE_MS=4000` aligns with the official 4 s. The departure is 0Bu, not the product.
5. The core conclusions are unchanged, but status-page wake must not be carried out as v4-pro / flash's "Class C product policy, acceptable." See R1.

## 3.4 Things not to do (for later agents)

- Do not replace an official unique review with a third-party report, and do not replace an official code unique review of a specific commit with this file. The first cloud audit may be corrected, but a correction must return to the official original text. Another third-party report must not overwrite it directly.
- Do not write the not-accepted conclusions of R1-R17 back into implementation prompts as new features to build.
- In this round, do not change the window width, the domain-window fallback, the NVS plan, or disconnect-at-end-of-command, do not force MTU 247, and do not bring in the yoziru runtime.
- Do not deny that official window width 32 has already been verified because some report did not read `session.go`, or because it cited the wrong `session.go:43-53`.
- Do not write "official does not exchange MTU" into the specification again. The official client exchanges MTU after subscribe and falls back to the default ATT only on failure.
- Do not change `main` or `feature/tesla-key`, and do not flash, unless the user separately and explicitly authorizes it.

---

# Coordinator judgment and development principles (2026-11-25 23:50 CST)

After reading all the research materials, and personally checking official `protocol.md`, `internal/authentication/crypto.go` / `window.go`, `internal/dispatcher/session.go`, `pkg/connector/ble/ble.go`, and `signer.go`, I conclude as follows.

The first official cloud audit ([bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be)) is right on the core judgments. That is not "right because an official run started it." The original text supports it: two domains and two sessions; the command counter and the response counter are not the same set; response anti-replay is per the same request, the first counter, including 0, must be accepted, and reuse on the same request must be rejected; the official window width is 32; VCSEC sends only one request at a time and at most about three replies; a missing reply may already have executed, and the actuation must not be automatically resent; VCSEC can still be reached over BLE while Infotainment is asleep; wake must be explicit, and there must be no implicit wake in order to read status; `GET_STATUS` may be unauthenticated; an omitted proto3 lock state must not be shown as unlocked; the vehicle has only about 3 BLE connection slots. I still accept the FAIL of the checklist re-review of `1377473` ([bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc)): clearing a known lock state on timeout, implicitly waking when the status page is opened, and displaying an omitted lock state are the three items to fix now. I do not accept DeepSeek downgrading status-page wake to "an acceptable product policy."

It is also not complete. Some places in `protocol.md` are vague. For example, `TAG_COUNTER` in the response AAD is written as "initially session_info.counter." The implementation must use `AES_GCM_Response_data.counter`, and it must pass GCM before updating the window. Official source is the final basis in those places. The first cloud audit missed that official `ble.go` calls `ExchangeMTU` after subscribe and falls back to the default ATT on failure. Several third-party reports record real pitfalls, but they often promote product policy, a superseded ADR, a wrong line number, or source they did not read into official specification. I recorded those and rejected them, so they are not argued again later.

How to understand the interaction specification, and how this Bluetooth key should be developed: what we are building is a key in a pocket, not a vehicle-status poller and not a charging manager. The protocol layer does only what official specifies: a session per domain, anti-replay per request, honest actuation, honest status, and a restrained wake. While the car is asleep, use VCSEC to read and write locks and closures. Do not pull Infotainment up just to glance at status. If lock was pressed and there is temporarily no reply, admit that it may already have executed. Do not clear a lock state that is already known, and do not automatically send it again. A field that did not appear is unknown. It is not "unlocked by default" or "closed by default." The link layer follows the official client: write-with-response, 1024, a 1-second fragment timeout, and an MTU exchange attempted after subscribe, with fallback allowed. Do not write 247, disconnect-when-finished, or a 7-second or 15-second timer as a Tesla rule. Third-party repositories are used only to avoid pits others have already hit. They are not the specification, and their roles or polling habits are not copied in. The product may be stricter than official, but a stricter choice must not be described as an official requirement. The hardware is an ESP32-C3 with no PSRAM. Keep the session layer thin. Buttons and power consumption yield to getting it right first, and then making it frugal.

# Follow-up research plan (by P00-P17, and by concentrated testing, 2026-11-25 23:58 CST)

The formal goal of the baseline edition is still P00 through P17. P03 is the adjudication of the common baseline, not the finish. P17 is the adjudication that formal firmware development is complete. A candidate that has not passed the formal gate cannot be called formal baseline firmware. Protocol research and fixes join this cadence. They do not open a parallel track.

Hard order: refuse testing once per update. Before any flash, finish every code change and official unique review that can be done without flashing. Do not ask the user, in the middle of the work, to flash and verify a single fix. The user flashes and tests only after authorization, and does both in one concentrated pass.

## 1. Before flashing: finish everything that can be done

This order does not expand the three fixes now in progress: do not clear a known lock state on timeout, do not implicitly wake on the status page, and do not show an omitted lock state as unlocked. Finish these three first, with an independent check and an official unique review.

After the three pass, continue on the same candidate with protocol alignment that does not depend on a real vehicle. Still one item at a time, unique review before the change, and none of them needs to go on the car first:

- Correct the comment that writes window 64 as official `window.go`, and separately review whether to narrow it to the official 32
- Verify that every signed request has registered its request identity, so the path does not fall through to the domain window
- Check the FLAG mask, that response AAD uses the response counter, that GCM precedes the window update, and that unmatched 31/41-byte authentication frames are dropped
- Check that the pairing role is not Charging Manager
- Compare, and on the code side align with the official client: `ExchangeMTU` after subscribe, fallback to the default ATT on failure, plus write-with-response, 1024, and a 1-second fragment timeout

Only after all of the above have passed official unique review is one authorized flash requested. Until then, do not flash. The device stays as it is.

## 2. After authorization, concentrate the test into one pass

Complete these in the same image and the same test. Do not split them into multiple flashes:

- **P00**: For that exact object, collect device-identity evidence such as the image checksum, boot version, and ELF prefix. A P00 record of an older object is not inherited automatically.
- **P01**: Reading the hardware guide has already been delivered. Keep it only as an applicable gate. Do not schedule a separate test for it.
- **P02**: Real-device boot, screen and three buttons, BLE initialization, storage and reboot; also unplug USB, 30-second screen-off, range deep sleep, first-key wake only, and settings retention. What was not observed is not written as PASS.
- On-car regression: lock, unlock, front trunk, rear trunk; opening the status page on a sleeping car does not wake; a missing reply still keeps the known lock state; a missing field is not shown as unlocked.
- Items observable on the same real-car pass: do not handshake Infotainment immediately after wake (issue 366); whether connection slots are stuck occupied; whether the charge port during sleep is an old latch (issue 368). Observe first, then decide product policy. Do not write them down as official obligations in advance.

Host probes and the limited P00 evidence on `47fd967` cannot be used directly as P03 or P16 evidence for a later commit.

## 3. After the concentrated test passes, advance by P level

- **P03** The coordinator adjudicates the common baseline from the official unique review, the build, and this concentrated-test evidence for that exact object. Do not enter P04 without passing the gate.
- **P04** From the accepted common baseline, freeze the baseline entry, buttons, feedback, disabled states, and the acceptance checklist. Do not start over with a new UI.
- **P05** Branch an independent baseline-edition branch from the accepted baseline. Do not overwrite existing unique results. Do not change `main`.
- **P06** Converge the baseline build configuration, modules, and background tasks. A change enters its dependents only after an official unique review.
- **P07** Three-button navigation, and selection and switching among at most three cars.
- **P08** The necessary pairing guidance, capacity, delete confirmation, and exception screens. Verify by simulation. If real-car pairing is required, fold it into the next concentrated test. Do not flash for it alone.
- **P09** Connect the four action entries and their feedback: unlock, lock, front trunk, and rear trunk.
- **P10** Official cloud unique review of mis-touch, repeated intent, the park condition, timeout, an unknown result, and no automatic resend. A contract of the same kind that was already fixed and uniquely reviewed before the flash must still formally pass the gate on the exact object of the baseline branch. One unique review of the fix candidate does not stand in for that.
- **P11** The state machine for running, idle, connected, sleep, and button wake. Do the code and the official unique review first.

Whatever in P04 through P11 can be finished on the host and in the cloud is finished and uniquely reviewed first. Do not ask the user to flash for any single one of those levels. If a new unique-review image is produced, schedule the next concentrated test and finish, in one pass, the parts that need a real device:

- **P12** Measure the running, idle, and sleep power baselines. Do not invent a range figure without an instrument.
- **P13** Optimize display refresh, backlight, and screen-off. After the code unique review, re-measure response and power in the same test.
- **P14** Optimize the BLE window, retry, wake, and sleep recovery. If idle disconnect is a product policy, decide it after this measurement. Do not trade a lost link or a mis-touch for lower power.
- **P15** Official-cloud-led regression of exceptions, storage faults, disconnects, repeated wakes, and endurance. Real-device supplementary evidence is folded into the same test.
- **P16** Official final review of the exact baseline candidate, plus IDF, the archive, and real-device evidence. Keep host, firmware, and device results separate.
- **P17** The coordinator adjudicates that formal baseline firmware development is complete, and delivers traceable firmware, a matching ELF, instructions, rollback, the power-measurement conditions, and the untested items. If the formal gate is not met, the candidate must not be called formally complete. Do not publish automatically. Pro comes after the baseline is complete.

Discipline is unchanged. The specification is the original text of the pinned official commit. A cloud audit and a third-party report may both be corrected, but a correction must cite the live-object path and line numbers. Do one thing at a time, with a clear boundary. Class C timers and connection policy do not enter an implementation prompt unless they have already been written down as an explicit product decision. Do not flash without authorization. Do not move `main` or `feature/tesla-key`.

# Vehicle-interaction protocol checklist

裁决版：2026-11-25 23:00 CST（Asia/Shanghai）。由总管把官方云端核实清单与仅发给总管的数份三方研究报告合并，并把不采信的三方结论一并列入，便于后续查找，避免重复论证。

增补：2026-11-25 23:30 CST。收入用户再交的 Word《车机交互规范只读核实清单（对照立项五个参考仓库）》。官方 Part 1 / Part 2 仍不改写；新增条目写入材料来源与 Part 3。本轮三项修复不扩大。

再裁决：2026-11-25 23:50 CST。总管按全部研究材料，并亲自对照 `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125`（兼核更早的 `a4b43c1`）原文重判，不默认第一次官方云审计必然正确。会话、计数器、按请求窗口、显式唤醒、无回包不得当失败、省略锁态不得显示未闭锁，这些核心结论经原文复核仍然成立。第一次云审计漏记了官方客户端在订阅后 `ExchangeMTU`（失败回退默认 ATT）；Word 报告「官方不交换 MTU」以及把 `session.go:43-53` 当成窗口实现，均与该活对象不符。五点 README 约束不改。本轮三项修复不扩大。

计划按 P 更新：2026-11-25 23:55 CST。将文末「后续研究计划」按基础版 P00–P17 重新编排。开发原则不改。本轮三项修复不扩大，不刷机。

权威顺序：以 `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` 的 `protocol.md` 与官方源码为第一权威。官方云端清单（[bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be)）是一次有依据的核实，不是不可订正的终审；与官方原文冲突时，以原文为准，并在本文件记下订正。三方研究报告是参考与已踩坑记录，冲突以官方原文为准。本文件不替代对具体提交的官方代码独审。

产品对照节点：`Lucky21418/passport-tesla-key` 候选分支 `cand/d08-dead-code-cleanup` @ `1377473a0170fde55a3eacba6068ef5767adc9d2`。`main`（`e85197d56abe92c35a1cfb84fad85fba2545c9cc`）与 `feature/tesla-key`（`47fd967aaa22d619fce3ffa15a3e97d5bd4ccac2`）未被移动。设备仍为已授权且独审通过的 `a3a5305`。本文件不授权刷机。

官方云端清单原文写于共识文档提交之前，当时仓库未被该次云端任务改动。其后候选分支已有文档提交 `4b1bd25643cec7eb4e92fc7e071821c75fd33a71`（仅 `README.md` / `README.zh_CN.md`），写入五点可执行约束。下方「Consensus-doc gap」已按此更正。按本清单对 `1377473` 的官方复审为 [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc)，结论 FAIL：超时清掉已知锁态、打开状态页隐式唤醒；省略 `vehicleLockState` 仍可能显示未闭锁，属残留。当前修复仍是这三项，不扩大。

下方 Part 1、Part 2、Conflicts、Class C、Application-layer schema 与两份官方摘要，保留云端清单原文（英文），只更正随后已被事实改写的文档缺口。新增的「材料来源」「本轮产品差距」「Part 3 三方研究报告裁决（含不采信）」由总管撰写。

官方清单当时未连接实车。「What it does on a real car」指由所引协议原文与所引客户端代码推出的后果，不是新的实车抓包。

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

官方云端清单写成时，共识文档尚未写入会话/防重放/唤醒约束。其后 `4b1bd25643cec7eb4e92fc7e071821c75fd33a71` 仅修改根目录 `README.md` 与 `README.zh_CN.md`（各 +11 行），写入五点可执行约束：响应窗口按同一条请求且首包含 counter 0 须接受；官方窗口宽度 32；VCSEC 同时只发一条且无回包不得自动补发；唤醒必须显式，不得为读状态隐式唤醒；proto3 省略的 `vehicleLockState` 不得显示未闭锁。并写明五份仓库角色与 C 类定时器不是官方规范。

该提交不是本完整清单的替代件。`docs/assets/README.md` 仍主要把 `vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` 作为 protobuf schema 出处。`PK_WAKE_SETTLE_MS`（7000）仍是产品源码中的本地停顿（C7），注释已说明它不是安全 TTL。`FoloToy/ai-passport` 在所核 SHA 上没有 `tesla` / `VCSEC` / `vehicle-command` 匹配，不贡献车机交互规则（A0）。

完整目录以本文件为准。不采信的三方结论见 Part 3，不要再写回 README 当官方要求。

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

### A22a. Official client ATT MTU exchange（总管 2026-11-25 复核补记）

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
10. （2026-11-25 总管复核）Official `ExchangeMTU` after Subscribe, with `DefaultMTU - 3` fallback (`ble.go` lines 349–357 at `0a582da` and `a4b43c1`), versus third-party “official does not exchange MTU.” Adopt the official client. Do not force ATT MTU 247, and do not raise the 1024 message cap.

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
9. 五点可执行约束已由 `4b1bd25` 写入根目录 README；本文件才是完整核实清单与不采信记录。云端清单原文写成时尚未改文档，这一条在裁决版中关闭。


---

# 材料来源（本裁决版收录）

| 材料 | 时间（CST） | 角色 | 总管用法 |
| --- | --- | --- | --- |
| 官方云端清单 [bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be) | 2026-11-25 | A 权威底本 | Part 1 / Part 2 几乎原文保留 |
| 对 `1377473` 的官方复审 [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc) | 2026-11-25 | 官方代码独审 | FAIL 两项中等 + 省略锁态残留，决定当前修复范围 |
| Antigravity 初稿 `tesla_interaction_spec_checklist.md` | 2026-11-25 | 三方研究，不完整 | 主干可参考；缺独立踩坑专章，不作完整清单 |
| DeepSeek flash 清单 | 2026-11-25 | 只读对照 | 主干一致；产品 HMAC 长度未核对；状态页唤醒判定不采信为最终策略 |
| DeepSeek v4-pro 复核（对 flash 逐条重写） | 2026-11-25 | 只读对照，优于 flash | 采信其对 flash 的勘误与产品 HMAC/窗口源头/回绕等核对；不采信状态页唤醒为可接受策略 |
| Antigravity 详细踩坑清单 `tesla_protocol_and_pitfalls_detailed.md` | 2026-11-25 | 三方已踩坑补充 | 主干与官方一致；额外嵌入式避开项保留为后续，不扩大本轮修复 |
| cite 风格长报告《车机交互规范与协议核实清单报告》 | 2026-11-25 | 三方综述，引用偏转写 | 主干部分可参考；多处把 B/C 写成 A，见下方不采信表 |
| Word《车机交互规范只读核实清单（对照立项五个参考仓库）》 | 2026-11-25 23:22 | 三方只读对照（当时读不到产品仓，也未直读 session.go / ble.go） | 主干多数与官方一致，P-01–P-12 可作已踩坑索引；「用完即断」和把窗口 32 / 1024 降为未核实，以及 5.1 整节当编码门禁，见 R13–R16；「官方不交换 MTU」与 `session.go:43-53` 窗口误引，见 R14、R17 |
| 总管 2026-11-25 23:50 原文再判 | 2026-11-25 23:50 | 总管亲自对照官方活对象，不默认第一次云审计必然正确 | 核心 A 项仍成立；补记 A22a；订正 R14、新增 R17；开发原则与后续研究计划见文末 |

仓库活对象仍以文首 Sources 表为准。`yoziru/tesla-ble` 仅对照，AGPL，不作运行时。

---

# 本轮产品差距与修复范围（总管裁决）

对照节点：`1377473`。域级共享窗口把新请求 `counter==0` 当重放的这一类，该提交已按 17 字节请求哈希分配窗口，方向正确；首次独审曾 PASS，按本清单复审后因原有中等问题 FAIL。

当前要改、且不得扩大的三项：

1. 超时或无回包时，不得把已经知道的锁态清成未知（`expire_pending` 在 LOCK_VERIFY / UNLOCK_VERIFY / 解锁超时等路径上的问题）。无回包不等于失败，更不得让界面因此变成「查锁态」。
2. 打开状态页时，若车已知在休眠，不得先发 `RKE_ACTION_WAKE_VEHICLE`。这是为读状态而做的隐式唤醒，官方复审按 A23 / A24 / A26 判中等 FAIL。
3. proto3 省略的 `vehicleLockState` 不得显示未闭锁。字段未出现时不得把默认 0 当成已核对的 UNLOCKED。

明确延后、本轮不动：窗口宽度 64 与官方 32 对齐（须改错误注释；是否收窄到 32 另做安全评审）；域窗口回退路径须核实所有签名请求都已 `note_req_id`，但在已识别请求上不应走回退；产品 BLE 与官方 A22a（订阅后 `ExchangeMTU`，失败回退）的对照与对齐。不刷 `1377473` 及后续未独审修复，除非用户另行明确授权。

---

# Part 3 — 三方研究报告裁决（含不采信）

分类沿用官方清单：A 官方真值，B 三方已核实证据（不得覆盖 A），C 未核实或本地定时，不得当协议规范。

下列「采信」是指：可以写进后续避开检查或产品对照，并且不与官方冲突。「不采信」是指：不得再当作官方规范、不得据此扩大本轮修复、不得写回共识文档当必须执行的官方要求。条目本身必须保留，方便以后的智能体查到已经驳回过。

## 3.1 采信（与官方一致，或为官方的已踩坑佐证）

1. 响应防重放必须绑定同一条请求；首包（含 `counter==0`）接受；同请求重复才拒绝；乱序只在窗口内允许。官方窗口宽度是 32，不是 64。三方 flash / v4-pro / AG 详细清单与官方 Part 1 的 A13–A16 一致，采信。
2. 域级共享窗口会把新请求合法首包 0 误判为重放。这是当晚同类问题的根因类别，也是官方 P1。产品 `1377473` 方向正确。采信。
3. 命令计数器与响应计数器不是同一个计数器；不得拿响应 counter 去和发送/会话 counter 比大小。采信（v4-pro P-02，官方 A7）。
4. VCSEC 最多约三帧回包；不得把第一帧当终态；同时只发一条 VCSEC 请求；空 `request_uuid` 必须能归属（常用 16 字节 routing address）。无回包可能已经执行，变动命令不得因超时自动补发。ACK / OK 不等于已执行。采信。
5. 两域两套会话。纯门锁、后备箱、VCSEC 状态读取不得为了这些动作去握手 Infotainment。官方 issue 468 的有效含义是这一条，不是「官方禁止一切轮询」。采信其正确部分。
6. 唤醒必须显式：`RKE_ACTION_WAKE_VEHICLE` 或 Fleet `wake_up`。VCSEC 在 Infotainment 休眠时仍可达；`GET_STATUS` 可以不鉴权，不得当成唤醒手段。后台自动刷新不得唤醒。采信官方 A23–A26；三方 `NO_WAKE_SKIP` / 660 s 作为 B/C 策略可参考，不得标成 A。
7. SessionInfo HMAC 必须完整 32 字节并常量时间比较。v4-pro 核对比 flash 更准：产品已经这样做，0Bu pinned 库接受 16 字节前缀是 0Bu 的偏离，不适用于产品。采信 v4-pro，改正 flash。
8. 64 位窗口的源头是 yoziru `sliding_window.h` 的 `WINDOW_SIZE=64`，0Bu 继承，产品沿用；产品头文件把 64 写成 official `window.go` 是错的。采信 v4-pro。官方值仍是 32。本轮只记下，不先改宽度。
9. 产品同 epoch 的 `clock_time` 回退直接拒绝；随机数失败 fail-closed；请求 counter 到 `UINT32_MAX` 拒绝、不归零。v4-pro 与官方 signer 能对上。采信，且这些已对齐，不必当本轮缺陷来改。
10. 产品分块 1000 ms / 1024 B、握手 4 s（`PK_SESSION_CHALLENGE_MS=4000`）与官方 BLE 参考实现一致。0Bu 的 3000 ms / 2048 B 是刻意偏离，无本机证据不要搬。采信。
11. 明文回包没有加密响应 counter；不得把明文的 0 送进加密窗口。0Bu「一明文一请求」可以更严，但不与官方冲突。采信其区分，不把更严写成官方义务。
12. AG 详细清单中与官方主干一致的避开项采信。其中路由地址轮换、SessionInfo 公钥绑定、AddKey 的 PRESENT_KEY 成帧、nanopb 的 `has_` 与零值、NimBLE / ESP32-C3 内存与断连清理，作为后续嵌入式避开项保留，不纳入本轮三项修复。
13. proto3 省略字段的零值陷阱（锁态、关闭件、睡眠）采信官方 App1–App3，本轮锁态显示必须改。

14. `FLAG_ENCRYPT_RESPONSE` 是位序号，掩码必须是 `1 << 1 = 2`。写成 1 实际置的是 `FLAG_USER_COMMAND`，实车常见 Fault 28（Issue #390）。响应 AAD 即使 `flags==0` 也必须带 `TAG_FLAGS`。与官方 Part 1 摘要一致。这是实现约束，不是本轮三项之一。
15. 响应解密 AAD 的 `TAG_COUNTER` 必须用响应自身的 `AES_GCM_Response_data.counter`，并且必须先完整校验 GCM tag 再入窗。yoziru v5.2.0 曾误用请求 counter、漏验 tag，至 v6.0.0 / 0Bu patch 0006 才修。与官方 A12 一致。该 Word 报告的 P-02 采信。
16. 不要在 BLE 收流上做启发式找帧头；长度非法或超过 1024 就丢弃复位。连上后要丢掉匹配不到本端 routing address 的 31/41 字节被动认证帧（官方 Issue #390；该报告 P-05）。作为后续嵌入式避开项保留，不进本轮三项。
17. 官方维护者在 Issue #368 确认：车辆不是 AWAKE 时，`BodyControllerState` 的充电口状态会恒报关闭，部分字段只是休眠前的锁存。界面不得把这时的「关闭」当成当前插枪或开关真相。这是后续 UI 规则，不进本轮三项（本轮只改省略锁态）。
18. Issue #366：显式唤醒之后不要立刻去握手 Infotainment，车端未就绪可能返回 Fault 11 或一直等到超时；休眠期 Infotainment 握手失败，也不应拆掉仍可用的 BLE。等待步长属于 C，日后实车标定，不进本轮。
19. 不要因为远端 VCSEC 故障或错误文本就自动删除、重建本地私钥；随机数失败必须 fail-closed。与 0Bu 指南 / patch 0007 及上面第 9 条一致。存储事务和公钥绑定是后续项，不重开本轮 NVS 方案（见 R6）。
20. 不要照搬 0Bu 或 TeslaBleHttpProxy 默认的 Charging Manager 角色。官方角色见 A30。Passport 若要门锁和前后备箱，必须按产品最小必要权限选角色。这是配对约束，不进本轮三项。
21. 该 Word 报告的 WAK-06 与官方复审一致：为读状态不得隐式唤醒，打开页面默认只走 VCSEC。这一点采信，并用来对照 R1——DeepSeek 把状态页一次唤醒当成可接受策略，这份报告没有沿用那个错误。


## 3.2 不采信或降级（必须保留，避免再被当规范）

### R1. 打开状态页时唤醒一次，属于可接受的产品策略

出现位置：DeepSeek flash；v4-pro 的 B-38、X-06、5.2「明确为用户显式动作」；部分摘要把 status 页一次唤醒与后台不唤醒写成不矛盾。

不采信原因：官方复审 [bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc) 已按「读状态不得隐式唤醒」判中等 FAIL。用户打开状态页是为了读状态，不是发出「唤醒车辆」这一动作。官方 A23 的显式唤醒是 `RKE_ACTION_WAKE_VEHICLE` 或 Fleet `wake_up`；A24 / A26 写明状态读取不应为此把休眠车叫醒。本轮必须改掉这条产品路径。后台不唤醒仍然成立，但不能用来为状态页隐式唤醒开脱。

### R2. 把 issue 468 写成「官方禁止对休眠车高频轮询」并标为 A

出现位置：cite 风格长报告 1.5、5.1 第 06 条。

不采信原因：issue 468 讲的是 HTTP 代理不要为任意命令同时向 VCSEC 和 Infotainment 握手，以免 Infotainment 休眠时把本来可独立工作的 VCSEC 命令一起拖死。这与 A1、A23 一致，应采信。但官方全文没有把「禁止高频后台轮询」写成 protocol 条文。后台不唤醒是三方实战的 B 策略（以及产品功耗政策），官方未禁止不等于官方规定了间隔，也不得标 A。正确的官方表述是：唤醒必须显式；不得为了刷新状态去唤醒或去轮询 Infotainment。

### R3. 把「命令结束后必须断开 BLE / 15 秒硬超时后 `ble_gap_terminate`」写成官方要求

出现位置：cite 风格长报告 1.4、2.4、5.1 第 07 条；Word 只读清单的 BLE-04、P-08、5.1「用完断开」表述（详见 R13）。

不采信原因：车端 VCSEC 最多约 3 个并发 BLE 连接，是官方事实（本清单 A21，issue 469）。挂起不释放会占槽，三方 Docker 超时杀进程是 B 类教训，可以记。但官方没有要求钥匙客户端每做完一个动作就断开，也没有规定 15 秒硬超时。Passport 是钥匙，保持连接可以是产品策略。不得把「用完必断」写进共识当官方义务，也不纳入本轮修复。

### R4. 把 0Bu 的 16 字节 SessionInfo HMAC 前缀当成产品也必须修的缺口

出现位置：DeepSeek flash 对 0Bu B 项的转写，未核对产品 tag 长度。

不采信原因：v4-pro 已核对产品要求 `tag_len == 32` 且常量时间比较。0Bu 偏离不适用于产品。保持 32 字节，不要学前缀。flash 这一处已被 v4-pro 勘误，以后以 v4-pro 与本裁决为准。

### R5. 把产品窗口宽度 64 写成官方值，或把「本轮必须改成 32」写成已经在修的范围

出现位置：产品头文件注释；部分转写未区分「官方值」与「本轮修复范围」。v4-pro 正确指出了源头和注释错误。

部分采信、范围上不采信：官方是 32，64 是更宽偏离，注释必须改，安全评审再决定是否收窄。这不是当前三项修复之一，不得借本轮改窗口宽度，也不得把 64 继续写成 official `window.go`。

### R6. 把 NVS 双页轮换、CRC32、禁止 Safe Mode 写成必须立刻落地的交互规范

出现位置：cite 风格长报告 2.3。

不采信为协议规范：这是 0Bu / ESP32 存储层踩坑，不是 `vehicle-command` 的会话规则。产品会话目前是 RAM-only，每次连接重握手，不适用官方缓存老化的三条恢复条件。存储层可以日后参考双页与校验，但不进本轮交互修复，不得标 A，也不得要求现在改 NVS 方案。

### R7. 超时、BUSY 或 `INVALID_TOKEN_OR_COUNTER` 就自动重发变动命令

出现位置：cite 风格长报告 1.6 的「自动恢复策略」（例如 TIMEOUT 后重连、INVALID_TOKEN_OR_COUNTER 更新 counter 后重发）。

不采信其笼统自动重发：官方 A29 写明没收到回包时作动器可能已经执行，不得把超时当成干净失败并自动补发变动命令。会话失步类故障可以按 A28，在完成 A6 的会话更新后，用新的命令 counter 再签一条；这与「同一条可能已执行的作动因超时再发一次」不是一回事。配对失败与权限不足应停止，而不是重试。

### R8. 把 7 s、660 s、5 s、15 s、30 s 等定时写成官方规范

出现位置：若干三方摘要把实战定时与官方 BLE 1 s / 4 s 写在一起；cite 报告的 15 s 断连；local-control / yoziru 的 660 s；Word 只读清单 5.1 把「彻底禁用后台定时轮询」写成可直接采用的基线。

不采信为 A：官方有依据的定时主要是 BLE 分块 1 s、官方 BLE 客户端会话信息年龄 4 s、最大帧 1024。7 s 唤醒沉降、660 s 休眠轮询下限、5 s 命令预算、15 s 断连都是 C 或 B 的产品/三方政策，可参考，不得标官方。

### R9. 把 Antigravity 初稿当作已经全面的交互规范清单

出现位置：AG 第一次交付的 `tesla_interaction_spec_checklist.md`。

不采信为完整清单：初稿缺少独立的「三方已踩坑」专章，也没有把「域共享窗口把新请求 counter 0 当重放」写成必须再避开的检查项。硬件 H 项仍是硬件契约，不是车机协议。其后应以官方云端清单为本，并以本 Part 3 收纳后续研究报告。

### R10. 依据过期或记忆中的 ADR-0003，说现行 0Bu 规范是「拒绝 counter 0」

出现位置：立项后的过期转述；官方清单 P9 已指出 ADR-0003 已被取代。

不采信：现行 ADR-0005 并不写「拒绝 counter 0」。它写的是认证成功后的窗口包含 counter 0，重复（包括重复的 0）才丢弃。不得再用被取代文本。

### R11. 把产品比官方更严的做法写成官方要求

出现位置：cite 报告与部分转写中，把强制 16 字节 routing address、命令结束必断、持久化计数器方案等写成 A。

不采信其升格：产品可以更严，且强制 routing address、UUID 可选，与官方 VCSEC 空 UUID 兼容，这是可接受的产品选择。但不得反写「官方要求 UUID 必须存在」或「官方要求用完断开」。官方要求是什么，以 Part 1 为准。

### R12. 用 Google 转写链接和「[cite: N]」代替行号锚点，把转写结论直接标 A

出现位置：cite 风格长报告全文。

不采信其引用形式作为核验依据：其中与官方原文一致的规则可以保留，但核验必须回到 `vehicle-command` 活对象上的路径与行号（以本文件 Part 1 与 v4-pro 带行号的条目为准）。转写增补、升格或省略处，以本不采信表为准。

### R13. 把「按需短连、用完即断」和各 BLE 步骤后的强制断开写成可直接落地的官方基线

出现位置：Word 只读清单 BLE-04、P-08、5.1 第 2 条。

不采信为官方义务：与 R3 相同。车端最多约 3 个并发 BLE 连接是官方事实；iPad 等占满槽位后会表现为「唤不醒」，以及底层连接挂死需要硬超时，这些是该报告和 tesla_ble_mqtt_docker / TeslaBleHttpProxy 的 B 类教训，可以记。但官方没有要求钥匙每做完一个动作就断开，也没有规定空闲断连秒数。该报告自己在 5.2 也把空闲断连时长列为待实车标定的 C。Passport 保持连接可以是产品策略。本轮不改连接策略。

### R14. 因为这份报告没读到 `session.go` / `ble.go`，就把官方窗口宽度 32 或最大帧 1024 B 降回「仍待复核」

出现位置：Word 只读清单开篇边界、WIN-04、CNF-03、5.2；以及该报告把 `internal/dispatcher/session.go:43-53` 写成「精确 windowSize = 32 与滑窗实现」。

不采信其降级，并订正其行号：2026-11-25 23:50 总管亲自核对 `0a582da` 与更早的 `a4b43c1`。`windowSize = 32` 在 `internal/authentication/crypto.go` 第 21 行（注释写明 `Verifier.windowSize` 是 `uint64`，故宽度必须 ≤ 64）；算法在 `internal/authentication/window.go`；按请求使用发生在 `session.decrypt` 对 `handler.antireplay.Update`。这两个提交里，`session.go:43-53` 是 `newSession` / `decrypt` 开头，不是窗口常量或滑窗实现。不得用这份未读到的边界或错误行号，否定已经核过的官方值。本轮仍然不改产品窗口宽度 64。

### R15. 因为当时读不到产品仓，就把「`1377473` 尚未核对与规范的差距」当成对我们仍然开放的未知

出现位置：Word 只读清单开篇、5.2 第一条。

不采信为当前状态：该报告确实没读到产品代码，所以它不能对 `1377473` 的 C 实现下结论。我们这边已有首次独审 PASS（[bc-e5fb9eb2](https://cursor.com/agents/bc-e5fb9eb2-8183-58be-bf29-93cd3379d170)）和按本清单的复审 FAIL（[bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc)）。当前差距以本文件「本轮产品差距与修复范围」为准，不要再用「仓库不可访问」把三项修复说成还没对照过。

### R16. 把 5.1「已核实可直接采用、可直接落地编码」整节当作合入门禁

出现位置：Word 只读清单第 5.1 节；该节导出文本还夹杂了损坏的 `[cite:]` 标记。5.2 把过长 TTL 写成 Fault 209。

不采信其整节效力：与官方一致的条目已经分别采信。混入的「用完断开」「彻底禁用后台定时轮询」不得当官方门禁，见 R13、R2、R8。核验仍须回到仓库路径与本文件 Part 1 的行号，不能靠文末编号或损坏的 cite 标记。过长 TTL 对应的官方枚举是 `TIME_TO_LIVE_TOO_LONG`（20），209 是笔误，不得当新的故障码。

### R17. 把「官方 `ble.go` 默认不发起 ATT MTU 交换」写成官方行为

出现位置：Word 只读清单关于 ESP32-C3 NimBLE / MTU 的待核实项；其依据接近 0Bu ADR-0005 对更早官方客户端的转述，并点名 `ble.go` 约第 349 行。

不采信：在权威提交 `0a582da`，以及该报告建议复核的 `a4b43c1`，`pkg/connector/ble/ble.go` 第 349–357 行都是订阅 RX 之后立刻 `ExchangeMTU(maxBLEMTUSize)`，失败则回退 `ble.DefaultMTU - 3`，成功则 `min(txMtu, 1024) - 3`。第一次官方云审计记下了 write-with-response、1024 与 1 s 分片超时，但漏记了这一段，这是该次审计的缺口，不是「官方不交换 MTU」。247 仍不是 `protocol.md` 义务，也不得把最大帧抬到 2048。产品是否已经这样做，放到后续对照，不纳入本轮三项修复。

## 3.3 DeepSeek flash 相对 v4-pro 的勘误（采信 v4-pro）

以后若只看到 flash 版，以下以 v4-pro 及本裁决为准：

1. 产品 SessionInfo HMAC 已是完整 32 字节常量时间比较；flash 未核产品 tag 长度。
2. 64 位窗口源头是 yoziru，不是官方。
3. 产品拒绝同 epoch 的 `clock_time` 回退；RNG 失败 fail-closed；`UINT32_MAX` 保护与官方 signer 对应。
4. `PK_SESSION_CHALLENGE_MS=4000` 与官方 4 s 对齐；偏离的是 0Bu，不是产品。
5. 核心结论不变，但状态页唤醒不得按 v4-pro / flash 的「C 产品策略、可接受」执行，见 R1。

## 3.4 不要做的事（给后续智能体）

- 不要用三方报告替换官方独审，也不要用本文件替换对具体提交的官方代码独审。第一次云审计可以订正，但订正必须回到官方原文，不能用另一份三方报告直接覆盖。
- 不要把 R1–R17 的不采信结论再写回实现提示词，当作要做的新功能。
- 不要在本轮修改窗口宽度、域窗口回退、NVS 方案、命令结束断连、强制 MTU 247，或引入 yoziru 运行时。
- 不要因为某份报告没读到 `session.go`，或引用了错误的 `session.go:43-53`，就否定官方窗口宽度 32 已经核实。
- 不要再把「官方不交换 MTU」写进规范；官方客户端在订阅后会交换，失败才回退默认 ATT。
- 不要改 `main` 或 `feature/tesla-key`，不要刷机，除非用户另行明确授权。

---

# 总管判断与开发原则（2026-11-25 23:50 CST）

我按全部研究材料，并亲自核对官方 `protocol.md`、`internal/authentication/crypto.go` / `window.go`、`internal/dispatcher/session.go`、`pkg/connector/ble/ble.go` 和 `signer.go` 后，结论如下。

第一次官方云审计（[bc-d7db7c3b](https://cursor.com/agents/bc-d7db7c3b-b1b8-52a7-b6cc-5471aee508be)）在核心判断上是对的，不是“因为官方发起就对”，而是原文支持它：两个域两套会话；命令计数器与响应计数器不是同一套；响应防重放按同一条请求，第一个 counter（包括 0）必须接受，同一请求上复用必须拒绝；官方窗口宽度是 32；VCSEC 同时只发一条、最多约三个回包，无回包可能已经执行，不得自动补发作动；VCSEC 在 Infotainment 休眠时仍可经 BLE 到达，唤醒必须显式，不得为读状态隐式唤醒；`GET_STATUS` 可以无认证；proto3 省略的锁态不得显示未闭锁；车端大约只有 3 个 BLE 连接槽。对 `1377473` 按清单复审的 FAIL（[bc-d18ff0ed](https://cursor.com/agents/bc-d18ff0ed-5706-54e9-8317-e91b47fc82dc)）我仍然接受：超时清掉已知锁态、打开状态页隐式唤醒，加上省略锁态显示，就是当前要修的三项。DeepSeek 把状态页唤醒降成「可接受的产品策略」，我不接受。

它也不是完整无缺。`protocol.md` 有的地方写得含糊，例如响应 AAD 的 `TAG_COUNTER` 写着 “initially session_info.counter”，实现上必须用 `AES_GCM_Response_data.counter`，并且先通过 GCM 再更新窗口；官方源码才是这些地方的最终依据。第一次云审计漏记了官方 `ble.go` 在订阅后 `ExchangeMTU`、失败回退默认 ATT。若干三方报告有真实踩坑，但常把产品策略、过期 ADR、错误行号或没读到的源码升格成官方规范。这些我记下并驳回，免得以后再论证一遍。

交互规范的理解，以及这把蓝牙钥匙该怎么开发：我们要做的是口袋里的钥匙，不是车况轮询器，也不是充电管理器。协议层只做官方规定的事——按域建会话，按请求防重放，作动诚实，状态诚实，唤醒克制。车在睡觉时，用 VCSEC 读写锁和开闭，不要为了看一眼状态就把信息娱乐系统拉起来；按了锁却暂时没有回包，就承认可能已经执行，不要清掉已经知道的锁态，更不要自动再发一次。屏幕上没看到的字段就是未知，不是“默认未锁”或“默认关闭”。链路层跟官方客户端走：write-with-response、1024、1 秒分片超时、订阅后尝试 MTU 交换并允许回退，但不把 247、用完即断、7 秒或 15 秒定时写成 Tesla 的规定。第三方仓库只用来避免别人已经踩过的坑，不拿来当规范，也不把它们的角色或轮询习惯抄进来。产品可以比官方更严，但不能把更严的选择说成官方要求。硬件上这是一块没有 PSRAM 的 ESP32-C3，会话层保持薄实现，按键和功耗让位于“先做对，再做省”。

# 后续研究计划（按 P00–P17，2026-11-25 23:55 CST）

基础版的正式目标仍是 P00 到 P17。P03 是公共基础裁决，不是终点；P17 才是正式固件开发完成的裁决。未过正式门禁的候选，不能叫做正式基础版固件。协议研究与修复纳入这条节奏，不另开平行路线。

## 当前：P03 之前必须先闭合的缺陷

这不是新的 P 编号。候选在实车上已经暴露：超时清掉已知锁态、打开状态页隐式唤醒、省略锁态可能显示未闭锁。这三项正在修，修完后经独立核对和官方独审，再等明确授权后刷入，在车上回归锁/解锁、前盖/后备箱、休眠车打开状态页不唤醒、无回包仍保持已知锁态、缺字段不显示未锁。在此之前不扩大代码范围，不刷机。`47fd967` 上的主机探针和有限 P00 证据，不能直接当作后来提交的 P03 或 P16 证据。

## 之后按 P 级顺序，一次一件

- **P00** 授权刷入独审修复镜像后，重新采集该精确对象的镜像校验、启动版本、ELF 前缀等设备身份证据。旧对象的 P00 记录不自动继承。
- **P01** 硬件指南阅读已交付，只作为适用门禁保留，不等于全仓合规或设备验收。
- **P02** 在该镜像上做真机启动、屏幕与三键、BLE 初始化、存储与重启；并补在家仍待测的拔 USB、30 秒息屏、续航深睡、首钥匙仅唤醒、设置保持。未观察不写 PASS。
- **P03** 总管根据该精确对象的官方独审、构建与 P02 真机证据，裁决公共基础。未过门禁不得进入 P04。
- **P04** 从已接受的公共基础冻结基础版入口、按键、反馈、禁用态和验收清单，不另起炉灶改界面。
- **P05** 从已接受基础拉出基础版独立分支，不覆盖已有独有成果，不改 `main`。
- **P06** 收敛基础版构建配置、模块与后台任务，变更经官方独审后再进入依赖项。
- **P07** 三键导航，最多三台车的选择与切换。
- **P08** 必要配对引导、容量、删除确认与异常界面；模拟验证，实车配对按门禁执行。此时核对接对角色不是 Charging Manager。
- **P09** 接通开锁、锁车、前盖、后备箱四类动作入口与反馈。
- **P10** 官方云端独审误触、重复意图、驻车条件、超时、未知结果与不自动补发。当前三项修复属于这类契约，但 P10 仍须在基础版分支的精确对象上正式过闸，不能用修候选的一次独审代替。窗口宽度是否收到官方 32、请求身份是否都已登记、FLAG 掩码、响应 AAD 使用响应计数器、GCM 先于窗口更新、未匹配认证帧丢弃，作为 P10 前后的单任务，一次一件，先独审再改。
- **P11** 运行、空闲、连接、休眠与按键唤醒状态机；官方独审，真机补证。
- **P12** 测运行、空闲、休眠功耗基线。无仪表不编造续航。
- **P13** 优化显示刷新、背光与熄屏；代码独审后复测响应与功耗。
- **P14** 优化 BLE 窗口、重试、唤醒与休眠恢复。对照产品是否已在订阅后交换 MTU 并在失败时回退，以及 write-with-response、1024、1 秒分片；唤醒后不要立刻与 Infotainment 握手（issue 366）、连接槽是否被占死、空闲断开是否作为产品策略，都在这一级先测量再决定，不提前写成官方义务。不以失联或误触换功耗。
- **P15** 官方云端主导异常、存储故障、断连、反复唤醒与耐久回归。休眠时充电口可能是旧闩锁（issue 368），只影响以后界面，在此核观察、不编造。
- **P16** 对精确基础版候选做官方终审，并补 IDF、归档与真机证据。主机、固件、设备结果分开。
- **P17** 总管做基础版正式固件开发完成裁决，交付可追溯固件、匹配 ELF、说明、回退、功耗测量条件与未测项。未满足正式门禁，不得把候选叫做正式完成。不自动公开发布。基础版完成后再做 Pro。

纪律不变：规范以官方固定提交的原文为准，云审计和三方报告都可以被订正，但订正必须引用活对象路径与行号。一次只做一件边界清楚的事。C 类定时器和连接策略不进实现提示词，除非已经明确写成产品决定。未授权不刷机，不动 `main` 和 `feature/tesla-key`。

<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Tesla Key (`feature/tesla-key`)

Product feature branch for the **Passport Tesla Key** application on the FoloToy AI Passport hardware platform (ESP32-C3, 8 MB Flash, no PSRAM, ESP-IDF 5.5.3).

## Branch Model and Baseline

- **Upstream Baseline**: `FoloToy/ai-passport:main` (`e85197d56abe92c35a1cfb84fad85fba2545c9cc`)
- **Fork Baseline (`main`)**: Kept strictly synchronized with `FoloToy/ai-passport:main` without product code changes.
- **Product Branch (`feature/tesla-key`)**: Branched directly from `upstream/main`. All application-specific UI, BLE key workflow, state machines, and documentation belong on `feature/*` branches per [`docs/fork-guide.md`](docs/fork-guide.md).

## M2 Controlled Migration Scope (`0dc5301`)

This branch hosts the controlled migration of the verified `0dc5301c52fd4d3ee1185227380a9d323e249860` (S0–S3 FIX2) protocol and orchestration core under the P1–P3 adjudication and Gate-A/B/C rules:

1. **Official BSP Baseline (Gate-A / P1)**:
   - Retains the official [`components/bsp/`](components/bsp/include/bsp_pins.h) unchanged: LCD SPI2 at **80 MHz** (`BSP_LCD_PCLK_HZ`), official CW2017 Wanli 500 mAh fuel-gauge profile, ES8311 sleep verification, and official driver-level button voltage lock (**40 ms** confirm / **120 ms** release hold).
   - Application-layer [`pk_btn_adapter`](components/tesla_key/include/pk_btn_adapter.h) and [`pk_policy.h`](components/tesla_key/include/pk_policy.h) align with **40 ms / 120 ms** voltage-lock confirmation/hold while preserving the **180 ms** click/double-click window, **500 ms** long-press threshold, GPIO0 ADC millivolt windows (`{0,150}` / `{150,447}` / `{447,1900}` mV, released `>= 1900 mV`), and the frozen 13-page standalone UI interaction.
2. **Asset & Third-Party License Compliance (Gate-B / P2)**:
   - All 31 unlicensed legacy bitmap icons and the legacy QR bitmap have been stripped (`main/app_icons.c` removed); icon slots and the VIN portal card use plain-text / licensed symbol labels rendered by `main/ui_widgets.c`.
   - Licensed third-party protocol assets and bundled CJK font subsets are registered in [`docs/assets/README.md`](docs/assets/README.md).
3. **Zero-Secret & Configurable Proto Tooling (Gate-C)**:
   - [`tools/gen_tesla_proto.sh`](tools/gen_tesla_proto.sh) is environment-configurable (`NANOPB_PYTHON` / `NANOPB_GENERATOR`) with no hardcoded personal paths and pins `protobuf==5.28.3`, `grpcio-tools==1.68.0`, and `nanopb==0.4.9.1`.
   - S4 candidate `33560de`, cleanup candidate `7e0582c`, and external directories are excluded.

## Basic Edition Development & Gate Plan (2026-11-24 Adjudication)

Per the user-confirmed Basic Edition workflow update on 2026-11-24, subsequent Basic Edition work follows a continuous-development and single consolidated acceptance schedule (supplemental tracking table in [`docs/assets/README.md`](docs/assets/README.md#basic-edition-development-review-and-consolidated-device-testing-plan-2026-11-24)):

1. **Continuous Development Before Consolidated Device Testing**: Because repeated interim hardware sessions are impractical, Basic Edition development proceeds continuously first and shifts to a single consolidated on-device test pass at the end.
2. **Interim Device Gates Retired (`P02`, `P03`)**: Former `P02` and `P03` are no longer used as mid-stream development gates; agents do not flash firmware or block waiting for hardware acceptance after each small increment (`Device tests: NOT RUN` with pending checks listed in `Unverified` per [`AGENTS.md`](AGENTS.md#required-validation-and-delivery)).
3. **Candidate Continuation & Deferred Branch Naming (`P04`, `P05`)**: Continue appending increments directly onto the current candidate line; `P04` executes against the established Basic Edition scope without a separate interim gate, and `P05` does not create a separate formal Basic Edition branch yet—formal naming is deferred until closeout.
4. **Single Consolidated Official Independent Review (`P10`, `P15`, `P16`)**: Interim official cloud reviews are no longer development gates; former `P10`, `P15`, and `P16` are merged into one official independent review of the final candidate after development finishes, and all review findings must be resolved before producing the testable firmware image.
5. **One-Pass Consolidated On-Device Test (`P12`)**: The user runs a single consolidated hardware test pass by flashing the post-review merged image and verifying boot, display rendering, three-button input, double-click feel, BLE behavior, NVS persistence across reboot, pairing UI, and sleep/wake; power draw (former `P12`) is measured when metering instruments are available.
6. **Post-Test Formal Adjudication & Delivery (`P03`, `P17`)**: Formal adjudication and delivery are performed after the consolidated device test, covering former `P03` and `P17`.
7. **Targeted Retest Only on Must-Fix Issues**: If the consolidated device test uncovers must-fix defects, ship a targeted fix build for retest rather than testing repeatedly during development.
8. **Single-Item Dispatch & Vehicle Safety Boundary**: Task dispatch remains strictly one item at a time; live-vehicle pairing and live-vehicle control are not executed.
9. **Double-Click Mis-Trigger Status (Option E Jointly Confirmed)**: Jointly confirmed Option E (`PK_DOUBLE_MS = 300 ms`, symmetric `40 ms` clean-release confirmation for `●` at valid `>= 1900 mV` while preserving `120 ms` cross-window / side-key / invalid-ADC hold, `5 ms` `esp_timer` median-filtered sampling with real sample timestamps and pre-deep-sleep timer teardown, and preserving active gesture windows across unrelated background expiry); on-device feel verification remains deferred to the consolidated hardware test pass.

## Custom 8 MB Partition Layout and Flashing / Stored Data Policy

Per [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md), this application uses a validated custom 8 MB [`partitions.csv`](partitions.csv):

| Partition | Type / Subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | `data` / `nvs` | `0x9000` | `0x6000` | ESP-IDF and application NVS key-value storage |
| `phy_init` | `data` / `phy` | `0xF000` | `0x1000` | RF PHY initialization data |
| `factory` | `app` / `factory` | `0x10000` | `0x7B0000` | Single factory application image |
| `bblog` | `data` / `0x40` | `0x7C0000` | `0x40000` | Application-specific bounded black-box diagnostic ring (optional to this application, not a mandatory template contract) |

### Flashing and Stored Data Policy

Follow [`docs/development/engineering/firmware-layout.md#flashing-and-stored-data`](docs/development/engineering/firmware-layout.md#flashing-and-stored-data):

- **No original-firmware backup is required** before flashing new firmware, but that does **not** guarantee preservation of user data or authorize a full-chip erase (`idf.py erase-flash`).
- Flashing the verified merged image `build/FoloToy-AI-Passport-full.bin` at offset `0x0` pads gaps between partitions and **can reset NVS and `bblog` regions** (including paired vehicle keys and settings). Use `0x0` merged flashing for blank-device provisioning or an intentional complete refresh.
- To preserve existing NVS state during development, use a compatible segmented flash workflow (`idf.py flash`) only when authorized.

## Validation Gates

```bash
./tools/validate.sh --static                  # repo checks, glyphs, actionlint, official + product host tests
./tools/gen_tesla_proto.sh --check            # verify generated nanopb protobuf sources
./tools/validate.sh --firmware                # isolated ESP-IDF 5.5.3 build, merge-bin, layout & archive verification
python3 tools/archive_firmware.py verify <archive-dir>
```

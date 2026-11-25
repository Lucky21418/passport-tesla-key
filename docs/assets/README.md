<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fork Supplemental Documentation and Asset Registry (`docs/assets/`)

## Purpose

This directory holds fork-owned supplemental documentation, architecture notes, and asset-licensing records supporting the root [`README.md`](../../README.md) on the `feature/tesla-key` branch.

## Structure Convention

- Keep bilingual Markdown documents paired as `<name>.md` (English default) and `<name>.zh_CN.md` (Simplified Chinese) with reciprocal language links at the top.
- Store reusable firmware binary assets under repository-root [`assets/`](../../assets/README.md), not in `docs/assets/`.

## M2 Asset and Third-Party Material Registry (Gate-B / P2 Adjudication)

Per parent adjudication P2 and [`assets/README.md`](../../assets/README.md), materials without a closed redistribution license chain are prohibited from entering this repository:

| Category | Path / Location | Source & Version / Commit | License | Status in `feature/tesla-key` |
| --- | --- | --- | --- | --- |
| **Stripped legacy icons & QR** | *Removed* (`main/app_icons.c` excluded) | Legacy 31 monochromatic 20×20 icons and 58×58 QR bitmap | Unverified redistribution chain | **Stripped in M2 (P2 Option 1)**. Replaced by plain-text / licensed symbol labels in [`main/ui_widgets.c`](../../main/ui_widgets.c) and [`main/app_icons.h`](../../main/app_icons.h). |
| **Tesla BLE Protocol Schema** | [`components/tesla_key/proto/schema/`](../../components/tesla_key/proto/schema/LICENSE) | `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` | Apache-2.0 License (included at `components/tesla_key/proto/schema/LICENSE`) | **Retained & Registered**. Verified against [`tools/gen_tesla_proto.sh`](../../tools/gen_tesla_proto.sh). |
| **nanopb Runtime & Generator Options** | [`components/tesla_key/proto/nanopb/`](../../components/tesla_key/proto/nanopb/LICENSE.txt) | `nanopb` `0.4.9.1` (`protobuf==5.28.3`, `grpcio-tools==1.68.0`) | zlib License (included at `components/tesla_key/proto/nanopb/LICENSE.txt`) | **Retained & Registered**. Preserves `oneof` speed presence (`car_server.options`). |
| **Application CJK Bitmap Font Subsets** | [`assets/fonts/app_font_source_han_sans_sc_12.c`](../../assets/fonts/app_font_source_han_sans_sc_12.c), [`assets/fonts/app_font_source_han_sans_sc_16.c`](../../assets/fonts/app_font_source_han_sans_sc_16.c), [`assets/fonts/app_font_cjk_chars.txt`](../../assets/fonts/app_font_cjk_chars.txt), [`tools/build_cjk_fonts.py`](../../tools/build_cjk_fonts.py) | Adobe Source Han Sans SC Regular (`adobe-fonts/source-han-sans` tag `2.005R`, source SHA-256 `f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256`), 475-glyph 4-bpp uncompressed LVGL subsets (12 px & 16 px) generated with pinned `lv_font_conv` `1.5.3` | SIL Open Font License 1.1 (included at [`assets/fonts/OFL-1.1-SourceHanSans.txt`](../../assets/fonts/OFL-1.1-SourceHanSans.txt)) | **Rebuilt & Registered (M2-FIX Option B)**. Exposed via thin wrappers `app_font_cjk_12()` / `app_font_cjk_16()` in [`main/app_font_cjk.c`](../../main/app_font_cjk.c); verified by `tools/check_glyphs.py` and `tests/test_ui_text_widths.py`. |
| **Official Battery Profile** | [`assets/battery/wanli-500mah-profile2-xil40x-20261020.txt`](../../assets/battery/wanli-500mah-profile2-xil40x-20261020.txt) | Official upstream Wanli New Energy 500 mAh CW2017 profile | Upstream project-supplied data file | **Unchanged from `upstream/main`** (P5 adjudication). |

## Custom Partition and Flashing Summary

The `feature/tesla-key` branch configures a 256 KB `bblog` data partition (`0x7C0000`–`0x800000`) alongside `nvs` (`0x9000`), `phy_init` (`0xF000`), and `factory` (`0x10000`, `0x7B0000` bytes) within the 8 MB Flash boundary. This layout is validated by `./tools/validate.sh --firmware` and is specific to this branch, not a mandatory template contract. See [`docs/development/engineering/firmware-layout.md`](../development/engineering/firmware-layout.md#flashing-and-stored-data) for the full flashing and stored-data policy.

## Basic Edition Development, Review, and Consolidated Device-Testing Plan (2026-11-24)

In alignment with the root [`README.md`](../../README.md#basic-edition-development--gate-plan-2026-11-24-adjudication) and [`AGENTS.md`](../../AGENTS.md#required-validation-and-delivery), the 2026-11-24 user-confirmed Basic Edition plan restructures former phase gates (`P02`–`P17`) and active agent rules as follows:

| Item / Gate Reference | 2026-11-24 Updated Disposition & Agent Rule |
| --- | --- |
| **Development cadence** | Develop Basic Edition increments continuously first; defer on-device hardware acceptance to a single consolidated test pass at the end because repeated mid-stream device testing is impractical. |
| **Former `P02`, `P03` (interim gates)** | Retired as mid-stream development gates. Do not flash firmware or pause waiting for on-device acceptance after each increment; report `Device tests: NOT RUN` and carry pending hardware checks in `Unverified`. |
| **`P04` (Basic Edition scope) & `P05` (branching)** | Continue appending commits on the current candidate line. Execute `P04` per the established Basic Edition target without a separate interim gate; defer opening or naming a separate formal Basic Edition branch (`P05`) until closeout. |
| **Former `P10`, `P15`, `P16` (official cloud reviews)** | Interim official cloud reviews are no longer development gates. Merge `P10`, `P15`, and `P16` into one official independent review of the final candidate once development completes; fix all review findings before building the testable firmware image. |
| **Consolidated device test & `P12` (power)** | Run one consolidated user device-test pass on the post-review merged image covering boot, screen rendering, three-button input, double-click feel, BLE, NVS storage across reboot, pairing UI, and sleep/wake; measure power consumption (`P12`) when metering equipment is available. |
| **Post-test closeout (`P03`, `P17`) & retest rule** | Perform formal adjudication and delivery after the consolidated device test, covering former `P03` and `P17`. Only if must-fix defects surface during consolidated testing is a follow-up fix image built for retest, rather than testing repeatedly during development. |
| **Dispatch & vehicle safety red lines** | Keep dispatch strictly one task item at a time. Never execute live-vehicle pairing or live-vehicle control. |
| **Double-click mis-trigger status** | Jointly confirmed Option E (`PK_DOUBLE_MS = 300 ms`, `40 ms` clean `●` release at valid `>= 1900 mV` with `120 ms` cross-window / side-key / invalid-ADC hold preserved, `5 ms` `esp_timer` timestamped sampling with pre-deep-sleep teardown, and active-gesture preservation); on-device verification stays in the final consolidated device test pass. |

## Boundary

Content in `docs/assets/` is private to this fork and must not be proposed back to upstream `FoloToy/ai-passport:main`. Upstream keeps `docs/assets/` empty except for `.gitkeep`.

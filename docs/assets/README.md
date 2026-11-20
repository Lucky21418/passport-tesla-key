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
| **Tesla BLE Protocol Schema** | [`components/tesla_key/proto/schema/`](../../components/tesla_key/proto/schema/LICENSE) | `teslamotors/vehicle-command` @ `0a582da430c4aca6d397fb6cbed5b4d6bb380125` | MIT License (included at `components/tesla_key/proto/schema/LICENSE`) | **Retained & Registered**. Verified against [`tools/gen_tesla_proto.sh`](../../tools/gen_tesla_proto.sh). |
| **nanopb Runtime & Generator Options** | [`components/tesla_key/proto/nanopb/`](../../components/tesla_key/proto/nanopb/LICENSE.txt) | `nanopb` `0.4.9.1` (`protobuf==5.28.3`, `grpcio-tools==1.68.0`) | zlib License (included at `components/tesla_key/proto/nanopb/LICENSE.txt`) | **Retained & Registered**. Preserves `oneof` speed presence (`car_server.options`). |
| **Application CJK Bitmap Font Subset** | [`main/app_font_cjk.c`](../../main/app_font_cjk.c), [`tools/gen_cjk_font.py`](../../tools/gen_cjk_font.py) | 459-glyph 4-bpp uncompressed LVGL bitmap subset (12 px and 16 px) covering fixed UI and [`pk_copy.c`](../../components/tesla_key/src/pk_copy.c) strings | Generated project subset; verified by `tests/test_ui_text_widths.py` | **Retained & Registered**. Covers 100% of application strings without modifying read-only LVGL fonts. |
| **Official Battery Profile** | [`assets/battery/wanli-500mah-profile2-xil40x-20261020.txt`](../../assets/battery/wanli-500mah-profile2-xil40x-20261020.txt) | Official upstream Wanli New Energy 500 mAh CW2017 profile | Upstream project-supplied data file | **Unchanged from `upstream/main`** (P5 adjudication). |

## Custom Partition and Flashing Summary

The `feature/tesla-key` branch configures a 256 KB `bblog` data partition (`0x7C0000`–`0x800000`) alongside `nvs` (`0x9000`), `phy_init` (`0xF000`), and `factory` (`0x10000`, `0x7B0000` bytes) within the 8 MB Flash boundary. This layout is validated by `./tools/validate.sh --firmware` and is specific to this branch, not a mandatory template contract. See [`docs/development/engineering/firmware-layout.md`](../development/engineering/firmware-layout.md#flashing-and-stored-data) for the full flashing and stored-data policy.

## Boundary

Content in `docs/assets/` is private to this fork and must not be proposed back to upstream `FoloToy/ai-passport:main`. Upstream keeps `docs/assets/` empty except for `.gitkeep`.

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

## Boundary

Content in `docs/assets/` is private to this fork and must not be proposed back to upstream `FoloToy/ai-passport:main`. Upstream keeps `docs/assets/` empty except for `.gitkeep`.

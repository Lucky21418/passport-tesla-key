#!/usr/bin/env python3
"""Reproducibly generate 20x20 LV_COLOR_FORMAT_A8 icon bitmaps in main/app_icons_data.c.

All 21 active BASIC icons are rasterized directly from the official upstream
CBI (elax46/custom-brand-icons, MIT License) and MDI (Templarian/MaterialDesign-SVG,
Pictogrammers, Apache-2.0 License) SVG files tracked under assets/icons/{cbi,mdi}/
using rsvg-convert (20x20 PNG) and ImageMagick convert (8-bit Alpha channel).

The 58x58 QR code bitmap for http://192.168.4.1 (ISO/IEC 18004 Version 2,
25x25 modules @ 2x2 px + 4 px quiet zone) is generated via LVGL's bundled
Project Nayuki qrcodegen.c (managed_components/lvgl__lvgl/src/libs/qrcode/qrcodegen.c,
MIT License) and preserved unchanged.
"""

from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS_DIR = REPO_ROOT / "assets" / "icons"
OUT_C_PATH = REPO_ROOT / "main" / "app_icons_data.c"

# (enum_name, array_name, official_id, license_spdx, rel_svg_path, html_inline_provenance)
ICON_SPECS: tuple[tuple[str, str, str, str, str, str], ...] = (
    (
        "APP_ICON_BT_CONNECT",
        "k_ico_bt_connect",
        "mdi:bluetooth-connect",
        "Apache-2.0",
        "assets/icons/mdi/bluetooth-connect.svg",
        "ui-approved/html/01-main.html inline <svg> & Templarian/MaterialDesign-SVG svg/bluetooth-connect.svg",
    ),
    (
        "APP_ICON_BT_OFF",
        "k_ico_bt_off",
        "mdi:bluetooth-off",
        "Apache-2.0",
        "assets/icons/mdi/bluetooth-off.svg",
        "ui-approved/html/02-main-offline.html inline <svg> & Templarian/MaterialDesign-SVG svg/bluetooth-off.svg",
    ),
    (
        "APP_ICON_BATT_60",
        "k_ico_batt_60",
        "mdi:battery-60",
        "Apache-2.0",
        "assets/icons/mdi/battery-60.svg",
        "ui-approved/html/01-main.html inline <svg> & Templarian/MaterialDesign-SVG svg/battery-60.svg",
    ),
    (
        "APP_ICON_BATT_10",
        "k_ico_batt_10",
        "mdi:battery-alert-variant-outline",
        "Apache-2.0",
        "assets/icons/mdi/battery-alert-variant-outline.svg",
        "ui-approved/icons/ICON-SOURCES.md & Templarian/MaterialDesign-SVG svg/battery-alert-variant-outline.svg",
    ),
    (
        "APP_ICON_TESLA_LOCK",
        "k_ico_tesla_lock",
        "cbi:tesla-lock",
        "MIT",
        "assets/icons/cbi/tesla-lock.svg",
        "ui-approved/html/01-main.html inline <svg> & elax46/custom-brand-icons icon-svg/tesla-lock.svg",
    ),
    (
        "APP_ICON_LOCK_OPEN",
        "k_ico_lock_open",
        "mdi:lock-open-variant",
        "Apache-2.0",
        "assets/icons/mdi/lock-open-variant.svg",
        "ui-approved/html/04-main-unlocked.html inline <svg> & Templarian/MaterialDesign-SVG svg/lock-open-variant.svg",
    ),
    (
        "APP_ICON_TESLA_CAR",
        "k_ico_tesla_car",
        "cbi:tesla-car",
        "MIT",
        "assets/icons/cbi/tesla-car.svg",
        "ui-approved/html/06-cars.html inline <svg> & elax46/custom-brand-icons icon-svg/tesla-car.svg",
    ),
    (
        "APP_ICON_TESLA_HOOD",
        "k_ico_tesla_hood",
        "cbi:tesla-hood",
        "MIT",
        "assets/icons/cbi/tesla-hood.svg",
        "ui-approved/html/01-main.html inline <svg> & elax46/custom-brand-icons icon-svg/tesla-hood.svg",
    ),
    (
        "APP_ICON_TESLA_TRUNK",
        "k_ico_tesla_trunk",
        "cbi:tesla-trunk",
        "MIT",
        "assets/icons/cbi/tesla-trunk.svg",
        "ui-approved/html/01-main.html inline <svg> & elax46/custom-brand-icons icon-svg/tesla-trunk.svg",
    ),
    (
        "APP_ICON_ALERT",
        "k_ico_alert",
        "mdi:alert-circle-outline",
        "Apache-2.0",
        "assets/icons/mdi/alert-circle-outline.svg",
        "ui-approved/html/02-main-offline.html inline <svg> & Templarian/MaterialDesign-SVG svg/alert-circle-outline.svg",
    ),
    (
        "APP_ICON_CHECK_CIRCLE",
        "k_ico_check_circle",
        "mdi:check-circle-outline",
        "Apache-2.0",
        "assets/icons/mdi/check-circle-outline.svg",
        "ui-approved/html/07-status.html inline <svg> & Templarian/MaterialDesign-SVG svg/check-circle-outline.svg",
    ),
    (
        "APP_ICON_PLUS",
        "k_ico_plus",
        "mdi:plus-circle-outline",
        "Apache-2.0",
        "assets/icons/mdi/plus-circle-outline.svg",
        "ui-approved/html/06-cars.html inline <svg> & Templarian/MaterialDesign-SVG svg/plus-circle-outline.svg",
    ),
    (
        "APP_ICON_CLIPBOARD",
        "k_ico_clipboard",
        "mdi:clipboard-text-outline",
        "Apache-2.0",
        "assets/icons/mdi/clipboard-text-outline.svg",
        "ui-approved/html/06-cars.html inline <svg> & Templarian/MaterialDesign-SVG svg/clipboard-text-outline.svg",
    ),
    (
        "APP_ICON_COG",
        "k_ico_cog",
        "mdi:cog-outline",
        "Apache-2.0",
        "assets/icons/mdi/cog-outline.svg",
        "ui-approved/html/08-settings.html inline <svg> & Templarian/MaterialDesign-SVG svg/cog-outline.svg",
    ),
    (
        "APP_ICON_MOON",
        "k_ico_moon",
        "mdi:weather-night",
        "Apache-2.0",
        "assets/icons/mdi/weather-night.svg",
        "ui-approved/icons/ICON-SOURCES.md & Templarian/MaterialDesign-SVG svg/weather-night.svg",
    ),
    (
        "APP_ICON_BRIGHTNESS",
        "k_ico_brightness",
        "mdi:white-balance-sunny",
        "Apache-2.0",
        "assets/icons/mdi/white-balance-sunny.svg",
        "ui-approved/html/08-settings.html inline <svg> & Templarian/MaterialDesign-SVG svg/white-balance-sunny.svg",
    ),
    (
        "APP_ICON_THERMOMETER",
        "k_ico_thermometer",
        "mdi:thermometer",
        "Apache-2.0",
        "assets/icons/mdi/thermometer.svg",
        "ui-approved/icons/ICON-SOURCES.md & Templarian/MaterialDesign-SVG svg/thermometer.svg",
    ),
    (
        "APP_ICON_WIFI",
        "k_ico_wifi",
        "mdi:wifi",
        "Apache-2.0",
        "assets/icons/mdi/wifi.svg",
        "ui-approved/html/08-settings.html inline <svg> & Templarian/MaterialDesign-SVG svg/wifi.svg",
    ),
    (
        "APP_ICON_TRASH",
        "k_ico_trash",
        "mdi:trash-can-outline",
        "Apache-2.0",
        "assets/icons/mdi/trash-can-outline.svg",
        "ui-approved/html/08-settings.html inline <svg> & Templarian/MaterialDesign-SVG svg/trash-can-outline.svg",
    ),
    (
        "APP_ICON_CELLPHONE",
        "k_ico_cellphone",
        "mdi:cellphone",
        "Apache-2.0",
        "assets/icons/mdi/cellphone.svg",
        "ui-approved/html/05-vin-ap.html inline <svg> & Templarian/MaterialDesign-SVG svg/cellphone.svg",
    ),
    (
        "APP_ICON_REFRESH",
        "k_ico_refresh",
        "mdi:refresh",
        "Apache-2.0",
        "assets/icons/mdi/refresh.svg",
        "ui-approved/icons/ICON-SOURCES.md & Templarian/MaterialDesign-SVG svg/refresh.svg",
    ),
)


def rasterize_svg_to_a8(svg_path: pathlib.Path) -> tuple[bytes, str]:
    raw_svg = svg_path.read_bytes()
    svg_sha256 = hashlib.sha256(raw_svg).hexdigest()
    p1 = subprocess.run(
        ["rsvg-convert", "-w", "20", "-h", "20", "-f", "png", str(svg_path)],
        capture_output=True,
        check=True,
    )
    p2 = subprocess.run(
        ["convert", "png:-", "-channel", "A", "-separate", "-depth", "8", "gray:-"],
        input=p1.stdout,
        capture_output=True,
        check=True,
    )
    if len(p2.stdout) != 400:
        raise RuntimeError(f"{svg_path}: expected 400 A8 bytes, got {len(p2.stdout)}")
    return p2.stdout, svg_sha256


def extract_existing_qr_block(c_text: str) -> str:
    marker = "/* 58x58 QR Code for http://192.168.4.1"
    idx = c_text.find(marker)
    if idx < 0:
        raise RuntimeError("Could not find existing k_vin_qr_px_58 block in main/app_icons_data.c")
    return c_text[idx:]


def main() -> int:
    existing_c = OUT_C_PATH.read_text(encoding="utf-8")
    qr_tail = extract_existing_qr_block(existing_c)

    lines: list[str] = [
        "/*",
        " * Official 20x20 LV_COLOR_FORMAT_A8 icon bitmaps and 58x58 QR code bitmap.",
        " *",
        " * Provenance & License Compliance:",
        " *   - Generated reproducibly by tools/build_app_icons.py from the official",
        " *     upstream SVG files stored under assets/icons/cbi/ and assets/icons/mdi/:",
        " *       rsvg-convert -w 20 -h 20 -f png <svg> | convert png:- -channel A -separate -depth 8 gray:-",
        " *   - No procedural or geometric approximation script is used.",
        " *   - CBI (custom-brand-icons, elax46/custom-brand-icons): MIT License",
        " *     License file: assets/icons/MIT-CBI.txt",
        " *     Icons (4): cbi:tesla-lock, cbi:tesla-car, cbi:tesla-hood, cbi:tesla-trunk",
        " *   - MDI (@mdi/svg, Templarian/MaterialDesign-SVG, Pictogrammers): Apache-2.0 License",
        " *     License file: assets/icons/Apache-2.0-MDI.txt",
        " *     Icons (17): mdi:bluetooth-connect, mdi:bluetooth-off, mdi:battery-60,",
        " *     mdi:battery-alert-variant-outline, mdi:lock-open-variant,",
        " *     mdi:alert-circle-outline, mdi:check-circle-outline,",
        " *     mdi:plus-circle-outline, mdi:clipboard-text-outline, mdi:cog-outline,",
        " *     mdi:weather-night, mdi:white-balance-sunny, mdi:thermometer,",
        " *     mdi:wifi, mdi:trash-can-outline, mdi:cellphone, mdi:refresh",
        " *   - 58x58 QR Code for http://192.168.4.1: generated via LVGL Project Nayuki",
        " *     qrcodegen (managed_components/lvgl__lvgl/src/libs/qrcode/LICENSE.txt, MIT License).",
        " */",
        "",
        '#include "app_icons.h"',
        "",
    ]

    for enum_name, arr_name, official_id, lic, rel_svg, provenance in ICON_SPECS:
        svg_path = REPO_ROOT / rel_svg
        if not svg_path.is_file():
            print(f"ERROR: missing SVG file {rel_svg}", file=sys.stderr)
            return 1
        a8_bytes, svg_sha = rasterize_svg_to_a8(svg_path)
        lines.append(
            f"/* {enum_name} — Official ID: {official_id} ({lic})\n"
            f" * Source SVG: {rel_svg} (SHA-256: {svg_sha})\n"
            f" * Provenance: {provenance}\n"
            f" */"
        )
        lines.append(f"static const uint8_t {arr_name}[400] = {{")
        for r in range(20):
            row = a8_bytes[r * 20 : (r + 1) * 20]
            hex_row = ", ".join(f"0x{b:02X}" for b in row)
            lines.append(f"    {hex_row},")
        lines.append("};")
        lines.append("")

    lines.append("static const lv_image_dsc_t k_app_icon_dscs[APP_ICON_COUNT] = {")
    for enum_name, arr_name, *_ in ICON_SPECS:
        lines.extend(
            [
                f"    [{enum_name}] = {{",
                "        .header = {",
                "            .magic = LV_IMAGE_HEADER_MAGIC,",
                "            .cf = LV_COLOR_FORMAT_A8,",
                "            .flags = 0,",
                "            .w = 20,",
                "            .h = 20,",
                "            .stride = 20,",
                "            .reserved_2 = 0,",
                "        },",
                "        .data_size = 400,",
                f"        .data = {arr_name},",
                "    },",
            ]
        )
    lines.append("};")
    lines.append("")

    full_output = "\n".join(lines) + qr_tail
    OUT_C_PATH.write_text(full_output, encoding="utf-8")
    print(f"Generated {OUT_C_PATH.relative_to(REPO_ROOT)} with {len(ICON_SPECS)} official SVG icons.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

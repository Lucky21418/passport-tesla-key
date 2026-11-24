#!/usr/bin/env python3
"""Reproducibly generate 20x20 LV_COLOR_FORMAT_A8 icon bitmaps in main/app_icons_data.c.

All 21 active BASIC icons use the exact official <path d="..."/> strings from the
approved HTML files under ui-approved/html/ in passport-handoff-v2-complete.md
(SHA-256: 623ffb773857152c7f8db388e8db692ed62e3f002fc1d60ded4d3417e7abbba5)
and ICON-SOURCES.md:
  - 4 CBI icons (elax46/custom-brand-icons, MIT License)
  - 17 filled MDI icons (Templarian/MaterialDesign-SVG, Pictogrammers, Apache-2.0 License)
Tracked under assets/icons/{cbi,mdi}/ and rasterized reproducibly using
rsvg-convert (20x20 PNG) and ImageMagick convert (8-bit Alpha channel).

The 58x58 QR code bitmap for http://192.168.4.1 (ISO/IEC 18004 Version 2,
25x25 modules @ 2x2 px + 4 px quiet zone) is generated via LVGL's bundled
Project Nayuki qrcodegen.c (managed_components/lvgl__lvgl/src/libs/qrcode/qrcodegen.c,
MIT License) and preserved unchanged.
"""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS_DIR = REPO_ROOT / "assets" / "icons"
OUT_C_PATH = REPO_ROOT / "main" / "app_icons_data.c"

# (enum_name, array_name, official_id, license_spdx, rel_svg_path, approved_html_provenance)
ICON_SPECS: tuple[tuple[str, str, str, str, str, str], ...] = (
    (
        "APP_ICON_BT_CONNECT",
        "k_ico_bt_connect",
        "mdi:bluetooth-connect",
        "Apache-2.0",
        "assets/icons/mdi/bluetooth-connect.svg",
        "ui-approved/html/main-locked.html (and unlocked.html, main-lowbatt.html, driving.html, status.html, cars.html, actuate.html, actuate-trunk-preview.html, delete.html)",
    ),
    (
        "APP_ICON_BT_OFF",
        "k_ico_bt_off",
        "mdi:bluetooth-off",
        "Apache-2.0",
        "assets/icons/mdi/bluetooth-off.svg",
        "ui-approved/html/cars.html & ui-approved/html/standby.html",
    ),
    (
        "APP_ICON_BATT_60",
        "k_ico_batt_60",
        "mdi:battery-60",
        "Apache-2.0",
        "assets/icons/mdi/battery-60.svg",
        "ui-approved/html/main-locked.html (and unlocked.html, main-unknown.html, driving.html, status.html, actuate.html, actuate-trunk-preview.html, delete.html, retry.html, standby.html, vin.html)",
    ),
    (
        "APP_ICON_BATT_10",
        "k_ico_batt_10",
        "mdi:battery-10",
        "Apache-2.0",
        "assets/icons/mdi/battery-10.svg",
        "ui-approved/html/main-lowbatt.html",
    ),
    (
        "APP_ICON_TESLA_LOCK",
        "k_ico_tesla_lock",
        "cbi:tesla-lock",
        "MIT",
        "assets/icons/cbi/tesla-lock.svg",
        "ui-approved/html/main-locked.html (and driving.html, cars.html)",
    ),
    (
        "APP_ICON_LOCK_OPEN",
        "k_ico_lock_open",
        "mdi:lock-open",
        "Apache-2.0",
        "assets/icons/mdi/lock-open.svg",
        "ui-approved/html/unlocked.html (and main-lowbatt.html)",
    ),
    (
        "APP_ICON_TESLA_CAR",
        "k_ico_tesla_car",
        "cbi:tesla-car",
        "MIT",
        "assets/icons/cbi/tesla-car.svg",
        "ui-approved/html/cars.html",
    ),
    (
        "APP_ICON_TESLA_HOOD",
        "k_ico_tesla_hood",
        "cbi:tesla-hood",
        "MIT",
        "assets/icons/cbi/tesla-hood.svg",
        "ui-approved/html/actuate.html (and trunks-icon-preview.html)",
    ),
    (
        "APP_ICON_TESLA_TRUNK",
        "k_ico_tesla_trunk",
        "cbi:tesla-trunk",
        "MIT",
        "assets/icons/cbi/tesla-trunk.svg",
        "ui-approved/html/actuate-trunk-preview.html (and trunks-icon-preview.html)",
    ),
    (
        "APP_ICON_ALERT",
        "k_ico_alert",
        "mdi:alert",
        "Apache-2.0",
        "assets/icons/mdi/alert.svg",
        "ui-approved/html/main-unknown.html & ui-approved/html/retry.html",
    ),
    (
        "APP_ICON_CHECK_CIRCLE",
        "k_ico_check_circle",
        "mdi:check-circle",
        "Apache-2.0",
        "assets/icons/mdi/check-circle.svg",
        "ui-approved/html/pair.html",
    ),
    (
        "APP_ICON_PLUS",
        "k_ico_plus",
        "mdi:plus",
        "Apache-2.0",
        "assets/icons/mdi/plus.svg",
        "ui-approved/html/cars.html",
    ),
    (
        "APP_ICON_CLIPBOARD",
        "k_ico_clipboard",
        "mdi:clipboard-text",
        "Apache-2.0",
        "assets/icons/mdi/clipboard-text.svg",
        "ui-approved/html/cars.html & ui-approved/html/settings.html (检查驻车资格)",
    ),
    (
        "APP_ICON_COG",
        "k_ico_cog",
        "mdi:cog",
        "Apache-2.0",
        "assets/icons/mdi/cog.svg",
        "ui-approved/html/cars.html & ui-approved/html/settings.html (设置)",
    ),
    (
        "APP_ICON_MOON",
        "k_ico_moon",
        "mdi:moon-waning-crescent",
        "Apache-2.0",
        "assets/icons/mdi/moon-waning-crescent.svg",
        "ui-approved/html/settings.html (省电策略)",
    ),
    (
        "APP_ICON_BRIGHTNESS",
        "k_ico_brightness",
        "mdi:brightness-6",
        "Apache-2.0",
        "assets/icons/mdi/brightness-6.svg",
        "ui-approved/html/settings.html (屏幕亮度)",
    ),
    (
        "APP_ICON_THERMOMETER",
        "k_ico_thermometer",
        "mdi:thermometer",
        "Apache-2.0",
        "assets/icons/mdi/thermometer.svg",
        "ui-approved/html/status.html",
    ),
    (
        "APP_ICON_WIFI",
        "k_ico_wifi",
        "mdi:wifi",
        "Apache-2.0",
        "assets/icons/mdi/wifi.svg",
        "ui-approved/html/settings.html (热点)",
    ),
    (
        "APP_ICON_TRASH",
        "k_ico_trash",
        "mdi:trash-can",
        "Apache-2.0",
        "assets/icons/mdi/trash-can.svg",
        "ui-approved/html/delete.html & ui-approved/html/settings.html (恢复出厂)",
    ),
    (
        "APP_ICON_CELLPHONE",
        "k_ico_cellphone",
        "mdi:cellphone",
        "Apache-2.0",
        "assets/icons/mdi/cellphone.svg",
        "ui-approved/html/vin.html",
    ),
    (
        "APP_ICON_REFRESH",
        "k_ico_refresh",
        "mdi:refresh",
        "Apache-2.0",
        "assets/icons/mdi/refresh.svg",
        "Templarian/MaterialDesign-SVG svg/refresh.svg (authorized fallback for active wait page slot not standalone in approved HTML)",
    ),
)


def rasterize_svg_to_a8(svg_path: pathlib.Path) -> tuple[bytes, str, str]:
    raw_svg = svg_path.read_bytes()
    svg_sha256 = hashlib.sha256(raw_svg).hexdigest()
    svg_text = raw_svg.decode("utf-8")
    m = re.search(r'<path[^>]*\bd="([^"]+)"', svg_text)
    if not m:
        raise RuntimeError(f"{svg_path}: missing <path d=...>")
    path_d = m.group(1)
    path_d_sha256 = hashlib.sha256(path_d.encode("utf-8")).hexdigest()
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
    return p2.stdout, svg_sha256, path_d_sha256


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
        " *     approved HTML <path d=\"...\"/> SVGs stored under assets/icons/cbi/ and assets/icons/mdi/:",
        " *       rsvg-convert -w 20 -h 20 -f png <svg> | convert png:- -channel A -separate -depth 8 gray:-",
        " *   - Every <path> d attribute is byte-for-byte identical to the approved HTML",
        " *     in passport-handoff-v2-complete.md (ui-approved/html/ & ICON-SOURCES.md).",
        " *   - No procedural, geometric approximation, or outline variant substitution is used.",
        " *   - CBI (custom-brand-icons, elax46/custom-brand-icons): MIT License",
        " *     License file: assets/icons/MIT-CBI.txt",
        " *     Icons (4): cbi:tesla-lock, cbi:tesla-car, cbi:tesla-hood, cbi:tesla-trunk",
        " *   - MDI (@mdi/svg, Templarian/MaterialDesign-SVG, Pictogrammers): Apache-2.0 License",
        " *     License file: assets/icons/Apache-2.0-MDI.txt",
        " *     Icons (17): mdi:bluetooth-connect, mdi:bluetooth-off, mdi:battery-60,",
        " *     mdi:battery-10, mdi:lock-open, mdi:alert, mdi:check-circle, mdi:plus,",
        " *     mdi:clipboard-text, mdi:cog, mdi:moon-waning-crescent, mdi:brightness-6,",
        " *     mdi:thermometer, mdi:wifi, mdi:trash-can, mdi:cellphone, mdi:refresh",
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
        a8_bytes, svg_sha, path_d_sha = rasterize_svg_to_a8(svg_path)
        lines.append(
            f"/* {enum_name} — Official ID: {official_id} ({lic})\n"
            f" * Approved HTML Provenance: {provenance}\n"
            f" * Source SVG: {rel_svg} (SHA-256: {svg_sha})\n"
            f" * <path> d attribute SHA-256: {path_d_sha}\n"
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
    print(f"Generated {OUT_C_PATH.relative_to(REPO_ROOT)} with {len(ICON_SPECS)} official approved HTML SVG icons.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

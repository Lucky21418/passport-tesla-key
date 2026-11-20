#!/usr/bin/env python3
"""Regenerate main/app_font_cjk.c covering 100% of glyphs used by app_ui.c and pk_copy.c."""

import json
import pathlib
import re
import subprocess
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT_FILE = REPO_ROOT / "main/app_font_cjk.c"
SOURCE_FILES = [
    REPO_ROOT / "main/ui_theme.h",
    REPO_ROOT / "main/ui_widgets.c",
    REPO_ROOT / "main/app_ui_pages.c",
    REPO_ROOT / "main/app_ui.c",
    REPO_ROOT / "components/tesla_key/src/pk_copy.c",
]

SWIFT_RASTERIZER = r'''
import AppKit
import CoreGraphics
import CoreText
import Foundation

struct GlyphOut: Encodable {
    let cp: UInt32
    let adv_w: Int
    let box_w: Int
    let box_h: Int
    let ofs_x: Int
    let ofs_y: Int
    let nibbles: [UInt8]
}

func renderSet(cps: [UInt32], fontSize: CGFloat, baseLine: Int) -> [GlyphOut] {
    let font = CTFontCreateWithName("PingFangSC-Regular" as CFString, fontSize, nil)
    let colorSpace = CGColorSpaceCreateDeviceGray()
    var out: [GlyphOut] = []

    for cp in cps {
        guard let scalar = UnicodeScalar(cp) else { continue }
        let str = String(scalar) as CFString
        let attrStr = CFAttributedStringCreate(nil, str, [kCTFontAttributeName: font] as CFDictionary)!
        let line = CTLineCreateWithAttributedString(attrStr)
        let runs = CTLineGetGlyphRuns(line) as! [CTRun]
        var glyph: CGGlyph = 0
        var adv = CGSize.zero
        if let run = runs.first, CTRunGetGlyphCount(run) > 0 {
            CTRunGetGlyphs(run, CFRangeMake(0, 1), &glyph)
            CTRunGetAdvances(run, CFRangeMake(0, 1), &adv)
        }
        var bbox = CGRect.zero
        CTFontGetBoundingRectsForGlyphs(font, .horizontal, &glyph, &bbox, 1)

        let advW = Int((adv.width * 16.0).rounded())
        if glyph == 0 || bbox.width <= 0.01 || bbox.height <= 0.01 {
            out.append(GlyphOut(cp: cp, adv_w: advW, box_w: 0, box_h: 0, ofs_x: 0, ofs_y: 0, nibbles: []))
            continue
        }

        let x0 = Int(floor(bbox.minX))
        let y0 = Int(floor(bbox.minY))
        let x1 = Int(ceil(bbox.maxX))
        let y1 = Int(ceil(bbox.maxY))
        let w = max(1, x1 - x0)
        let h = max(1, y1 - y0)

        var buf = [UInt8](repeating: 0, count: w * h)
        let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                            space: colorSpace, bitmapInfo: CGImageAlphaInfo.none.rawValue)!
        ctx.setAllowsAntialiasing(true)
        ctx.setShouldAntialias(true)
        ctx.setAllowsFontSubpixelPositioning(false)
        ctx.setShouldSubpixelPositionFonts(false)
        ctx.setAllowsFontSubpixelQuantization(true)
        ctx.setShouldSubpixelQuantizeFonts(true)
        ctx.setFillColor(gray: 0.0, alpha: 1.0)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(gray: 1.0, alpha: 1.0)

        var pos = CGPoint(x: CGFloat(-x0), y: CGFloat(-y0))
        CTFontDrawGlyphs(font, &glyph, &pos, 1, ctx)

        var nibbles: [UInt8] = []
        nibbles.reserveCapacity(w * h)
        for row in 0..<h {
            // CGBitmapContext backing buffer is already top-down in memory
            let srcRow = row
            for col in 0..<w {
                let raw = Double(buf[srcRow * w + col]) / 255.0
                let gamma = pow(raw, 0.65)
                let q = min(15, max(0, Int((gamma * 15.0).rounded())))
                nibbles.append(UInt8(q))
            }
        }
        out.append(GlyphOut(cp: cp, adv_w: advW, box_w: w, box_h: h, ofs_x: x0, ofs_y: y0, nibbles: nibbles))
    }
    return out
}

struct FontBundle: Encodable {
    let f12: [GlyphOut]
    let f16: [GlyphOut]
}

let args = CommandLine.arguments
let jsonData = try Data(contentsOf: URL(fileURLWithPath: args[1]))
let cps = try JSONDecoder().decode([UInt32].self, from: jsonData)
let bundle = FontBundle(
    f12: renderSet(cps: cps, fontSize: 13.0, baseLine: 4),
    f16: renderSet(cps: cps, fontSize: 16.0, baseLine: 5)
)
let outData = try JSONEncoder().encode(bundle)
try outData.write(to: URL(fileURLWithPath: args[2]))
'''


def collect_codepoints() -> list[int]:
    cps = set(range(32, 127))
    for path in SOURCE_FILES:
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if s.startswith("#") or s.startswith("//") or s.startswith("/*") or s.startswith("*"):
                continue
            for m in re.finditer(r'"([^"]+)"', line):
                for ch in m.group(1):
                    cp = ord(ch)
                    if cp >= 32:
                        cps.add(cp)
    return sorted(cps)


def parse_existing_font(font_src: str, suffix: str) -> dict[int, dict]:
    bmp_m = re.search(rf"k_glyph_bitmap_{suffix}\[\d+\]\s*=\s*\{{(.*?)\}};", font_src, re.S)
    dsc_m = re.search(rf"k_glyph_dsc_{suffix}\[\d+\]\s*=\s*\{{(.*?)\}};", font_src, re.S)
    ul_m = re.search(rf"k_unicode_list_{suffix}\[\d+\]\s*=\s*\{{(.*?)\}};", font_src, re.S)
    if not (bmp_m and dsc_m and ul_m):
        return {}
    bmp = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", bmp_m.group(1))]
    dscs = [
        {
            "bitmap_index": int(m.group(1)),
            "adv_w": int(m.group(2)),
            "box_w": int(m.group(3)),
            "box_h": int(m.group(4)),
            "ofs_x": int(m.group(5)),
            "ofs_y": int(m.group(6)),
        }
        for m in re.finditer(
            r"\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),"
            r"\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\}",
            dsc_m.group(1),
        )
    ]
    cps = [32 + int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", ul_m.group(1))]
    out = {}
    for idx, cp in enumerate(cps):
        d = dscs[idx + 1]
        start = d["bitmap_index"]
        end = dscs[idx + 2]["bitmap_index"] if (idx + 2) < len(dscs) else len(bmp)
        out[cp] = {
            "cp": cp,
            "adv_w": d["adv_w"],
            "box_w": d["box_w"],
            "box_h": d["box_h"],
            "ofs_x": d["ofs_x"],
            "ofs_y": d["ofs_y"],
            "packed": bmp[start:end],
        }
    return out


def pack_nibbles(nibbles: list[int]) -> list[int]:
    out = []
    for i in range(0, len(nibbles), 2):
        hi = nibbles[i] & 0x0F
        lo = (nibbles[i + 1] & 0x0F) if (i + 1 < len(nibbles)) else 0
        out.append((hi << 4) | lo)
    return out


def emit_font_arrays(suffix: str, glyphs: list[dict], line_height: int, base_line: int) -> str:
    bitmap_bytes = []
    dsc_lines = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"]
    for g in glyphs:
        idx = len(bitmap_bytes)
        packed = g["packed"] if "packed" in g else pack_nibbles(g["nibbles"])
        bitmap_bytes.extend(packed)
        dsc_lines.append(
            f"    {{.bitmap_index = {idx}, .adv_w = {g['adv_w']}, .box_w = {g['box_w']}, "
            f".box_h = {g['box_h']}, .ofs_x = {g['ofs_x']}, .ofs_y = {g['ofs_y']}}},"
        )

    bmp_lines = []
    for i in range(0, len(bitmap_bytes), 24):
        chunk = ", ".join(f"0x{b:02x}" for b in bitmap_bytes[i : i + 24])
        bmp_lines.append(f"    {chunk},")

    offsets = [g["cp"] - 32 for g in glyphs]
    ul_lines = []
    for i in range(0, len(offsets), 12):
        chunk = ", ".join(f"0x{o:04x}" for o in offsets[i : i + 12])
        ul_lines.append(f"    {chunk},")

    n_glyphs = len(glyphs)
    return "\n".join(
        [
            f"static const uint8_t k_glyph_bitmap_{suffix}[{len(bitmap_bytes)}] = {{",
            *bmp_lines,
            "};",
            "",
            f"static const lv_font_fmt_txt_glyph_dsc_t k_glyph_dsc_{suffix}[{n_glyphs + 1}] = {{",
            *dsc_lines,
            "};",
            "",
            f"static const uint16_t k_unicode_list_{suffix}[{n_glyphs}] = {{",
            *ul_lines,
            "};",
            "",
            f"static const lv_font_fmt_txt_cmap_t k_cmaps_{suffix}[1] = {{",
            "    {",
            "        .range_start = 32,",
            "        .range_length = 65500,",
            "        .glyph_id_start = 1,",
            f"        .unicode_list = k_unicode_list_{suffix},",
            "        .glyph_id_ofs_list = NULL,",
            f"        .list_length = {n_glyphs},",
            "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,",
            "    },",
            "};",
            "",
            f"static const lv_font_fmt_txt_dsc_t k_font_dsc_{suffix} = {{",
            f"    .glyph_bitmap = k_glyph_bitmap_{suffix},",
            f"    .glyph_dsc = k_glyph_dsc_{suffix},",
            f"    .cmaps = k_cmaps_{suffix},",
            "    .kern_dsc = NULL,",
            "    .kern_scale = 0,",
            "    .cmap_num = 1,",
            "    .bpp = 4,",
            "    .kern_classes = 0,",
            "    .bitmap_format = 0,",
            "};",
            "",
            f"static const lv_font_t k_font_{suffix} = {{",
            "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,",
            "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,",
            f"    .line_height = {line_height},",
            f"    .base_line = {base_line},",
            "    .subpx = LV_FONT_SUBPX_NONE,",
            "    .underline_position = -1,",
            "    .underline_thickness = 1,",
            f"    .dsc = &k_font_dsc_{suffix},",
            "    .fallback = NULL,",
            "    .user_data = NULL,",
            "};",
        ]
    )


def main() -> None:
    cps = collect_codepoints()
    existing_12: dict[int, dict] = {}
    existing_16: dict[int, dict] = {}
    if FONT_FILE.exists():
        font_src = FONT_FILE.read_text(encoding="utf-8")
        existing_12 = parse_existing_font(font_src, "12")
        existing_16 = parse_existing_font(font_src, "16")

    if all(cp in existing_12 and cp in existing_16 for cp in cps):
        bundle = {
            "f12": [existing_12[cp] for cp in cps],
            "f16": [existing_16[cp] for cp in cps],
        }
    else:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = pathlib.Path(tmp)
            swift_file = tmp_path / "gen.swift"
            in_json = tmp_path / "cps.json"
            out_json = tmp_path / "out.json"
            swift_file.write_text(SWIFT_RASTERIZER, encoding="utf-8")
            in_json.write_text(json.dumps(cps), encoding="utf-8")
            subprocess.run(["swift", str(swift_file), str(in_json), str(out_json)], check=True)
            bundle = json.loads(out_json.read_text(encoding="utf-8"))

    content = "\n".join(
        [
            '#include "app_font_cjk.h"',
            "",
            "/* Auto-generated 4-bpp LVGL fonts covering 100% of application CJK & symbol glyphs */",
            "",
            emit_font_arrays("12", bundle["f12"], line_height=17, base_line=4),
            "",
            emit_font_arrays("16", bundle["f16"], line_height=21, base_line=5),
            "const lv_font_t *app_font_cjk_12(void)",
            "{",
            "    return &k_font_12;",
            "}",
            "",
            "const lv_font_t *app_font_cjk_16(void)",
            "{",
            "    return &k_font_16;",
            "}",
            "",
        ]
    )
    FONT_FILE.write_text(content, encoding="utf-8")
    print(f"Generated {FONT_FILE} with {len(cps)} glyphs.")


if __name__ == "__main__":
    main()

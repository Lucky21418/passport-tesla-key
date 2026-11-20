#!/usr/bin/env python3
"""Reproducibly generate the 12 px and 16 px LVGL CJK font subsets in assets/fonts/.

Source font:
  Adobe Source Han Sans SC Regular (release tag 2.005R, SIL Open Font License 1.1)
  URL: https://github.com/adobe-fonts/source-han-sans/raw/2.005R/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf
  SHA-256: f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256

Converter:
  Official lv_font_conv pinned at version 1.5.3
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS_FONTS_DIR = REPO_ROOT / "assets" / "fonts"
CHARS_FILE = ASSETS_FONTS_DIR / "app_font_cjk_chars.txt"

SOURCE_TAG = "2.005R"
SOURCE_URL = (
    "https://github.com/adobe-fonts/source-han-sans/raw/"
    f"{SOURCE_TAG}/OTF/SimplifiedChinese/SourceHanSansSC-Regular.otf"
)
SOURCE_SHA256 = "f1d8611151880c6c336aabeac4640ef434fa13cbfbf1ffe82d0a71b2a5637256"
PINNED_LV_FONT_CONV_VERSION = "1.5.3"

TARGETS = (
    (12, "app_font_source_han_sans_sc_12", ASSETS_FONTS_DIR / "app_font_source_han_sans_sc_12.c"),
    (16, "app_font_source_han_sans_sc_16", ASSETS_FONTS_DIR / "app_font_source_han_sans_sc_16.c"),
)


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def collect_inventory() -> str:
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import check_glyphs  # noqa: PLC0415

    cps = set(range(0x20, 0x7F))
    for path in check_glyphs.iter_files(REPO_ROOT, ("main",), check_glyphs.SOURCE_SUFFIXES):
        if path.name == "app_font_cjk.c":
            continue
        for lit in check_glyphs.scan_source(path.read_text(encoding="utf-8")):
            for cp in lit.codepoints:
                if cp >= 0x20:
                    cps.add(cp)

    pk_copy = REPO_ROOT / "components" / "tesla_key" / "src" / "pk_copy.c"
    for lit in check_glyphs.scan_source(pk_copy.read_text(encoding="utf-8")):
        for cp in lit.codepoints:
            if cp >= 0x20:
                cps.add(cp)

    return "".join(chr(cp) for cp in sorted(cps))


def verify_converter(lv_font_conv: str) -> None:
    res = subprocess.run(
        [lv_font_conv, "--version"],
        check=True,
        capture_output=True,
        text=True,
    )
    version = res.stdout.strip()
    if version != PINNED_LV_FONT_CONV_VERSION:
        raise SystemExit(
            f"ERROR: expected lv_font_conv {PINNED_LV_FONT_CONV_VERSION}, got {version!r}"
        )


def sanitize_generated_header(
    out_path: pathlib.Path,
    source_font_arg: str,
    source_font_name: str,
    rel_out_path: str,
) -> None:
    """Replace host-specific paths in lv_font_conv's Opts comment with neutral paths."""
    content = out_path.read_text(encoding="utf-8")
    lines = content.splitlines(keepends=True)
    for idx, line in enumerate(lines[:10]):
        if line.startswith(" * Opts:"):
            updated = line.replace(f"--font {source_font_arg}", f"--font {source_font_name}")
            updated = re.sub(r"--font\s+\S+", f"--font {source_font_name}", updated, count=1)
            updated = re.sub(r"--output\s+\S+", f"--output {rel_out_path}", updated, count=1)
            lines[idx] = updated
            break

    sanitized = "".join(lines)
    forbidden = ["/Users/", "/home/", "/tmp/", str(REPO_ROOT)]
    home_dir = os.environ.get("HOME")
    if home_dir:
        forbidden.append(home_dir)
    header_window = "".join(lines[:15])
    for marker in forbidden:
        if marker and marker in header_window:
            raise SystemExit(
                f"ERROR: host-specific path {marker!r} remained in {out_path.relative_to(REPO_ROOT)}"
            )
    out_path.write_text(sanitized, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build 12/16 px Source Han Sans SC LVGL fonts.")
    parser.add_argument(
        "--font",
        type=pathlib.Path,
        required=True,
        help="Path to local SourceHanSansSC-Regular.otf (2.005R)",
    )
    parser.add_argument(
        "--lv-font-conv",
        default=shutil.which("lv_font_conv") or "lv_font_conv",
        help="Path to pinned lv_font_conv 1.5.3 executable",
    )
    parser.add_argument(
        "--update-chars",
        action="store_true",
        help="Regenerate assets/fonts/app_font_cjk_chars.txt from main/ and pk_copy.c before building",
    )
    args = parser.parse_args()

    if not args.font.is_file():
        print(f"ERROR: source font not found: {args.font}", file=sys.stderr)
        print(f"Download from: {SOURCE_URL}", file=sys.stderr)
        return 1

    actual_sha = sha256_file(args.font)
    if actual_sha != SOURCE_SHA256:
        print(
            f"ERROR: source font SHA-256 mismatch:\n"
            f"  expected: {SOURCE_SHA256}\n"
            f"  actual:   {actual_sha}",
            file=sys.stderr,
        )
        return 1

    verify_converter(args.lv_font_conv)

    ASSETS_FONTS_DIR.mkdir(parents=True, exist_ok=True)
    if args.update_chars or not CHARS_FILE.is_file():
        inventory = collect_inventory()
        CHARS_FILE.write_text(inventory + "\n", encoding="utf-8")
    else:
        inventory = CHARS_FILE.read_text(encoding="utf-8").rstrip("\r\n")

    for size, font_name, out_path in TARGETS:
        rel_out = out_path.relative_to(REPO_ROOT).as_posix()
        cmd = [
            args.lv_font_conv,
            "--font",
            str(args.font),
            "--symbols",
            inventory,
            "--size",
            str(size),
            "--bpp",
            "4",
            "--format",
            "lvgl",
            "--no-compress",
            "--lv-font-name",
            font_name,
            "--lv-include",
            "lvgl.h",
            "--output",
            rel_out,
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True)
        sanitize_generated_header(out_path, str(args.font), args.font.name, rel_out)
        print(f"Generated {rel_out} ({size} px, {len(inventory)} glyphs)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

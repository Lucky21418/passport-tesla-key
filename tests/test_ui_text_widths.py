import pathlib
import re
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
import check_glyphs  # noqa: E402


FONT_12_PATH = REPO_ROOT / "assets/fonts/app_font_source_han_sans_sc_12.c"
FONT_16_PATH = REPO_ROOT / "assets/fonts/app_font_source_han_sans_sc_16.c"


def _parse_lvgl_font(path: pathlib.Path):
    text = path.read_text(encoding="utf-8")
    clean = check_glyphs.COMMENT_RE.sub(" ", text)

    dsc_m = re.search(
        r"lv_font_fmt_txt_glyph_dsc_t\s+\w+\s*\[\s*\]\s*=\s*\{(.*?)\};",
        clean,
        re.S,
    )
    if not dsc_m:
        raise ValueError(f"missing glyph_dsc table in {path}")
    advs = [int(x) / 16.0 for x in re.findall(r"\.adv_w\s*=\s*(\d+)", dsc_m.group(1))]

    def named_list(pattern, name):
        match = re.search(
            pattern.pattern.replace(r"(\w+)", re.escape(name), 1),
            clean,
            re.S,
        )
        return check_glyphs.parse_ints(match.group(1)) if match else None

    cp_to_gid = {}
    for _, body in check_glyphs.CMAP_ARRAY_RE.findall(clean):
        for entry in check_glyphs.split_entries(body):
            fields = dict(check_glyphs.FIELD_RE.findall(entry))
            start = int(fields["range_start"], 0)
            length = int(fields["range_length"], 0)
            gid_start = int(fields["glyph_id_start"], 0)
            kind = fields["type"]
            if kind.endswith("FORMAT0_TINY"):
                for i in range(length):
                    cp_to_gid[start + i] = gid_start + i
            elif kind.endswith("FORMAT0_FULL"):
                ofs = named_list(check_glyphs.OFFSET_LIST_RE, fields["glyph_id_ofs_list"])
                for i, o in enumerate(ofs[:length]):
                    if o or i == 0:
                        cp_to_gid[start + i] = gid_start + o
            elif kind.endswith("SPARSE_TINY"):
                deltas = named_list(check_glyphs.UNICODE_LIST_RE, fields["unicode_list"])
                cnt = int(fields.get("list_length", str(len(deltas))), 0)
                for i, d in enumerate(deltas[:cnt]):
                    cp_to_gid[start + d] = gid_start + i
            elif kind.endswith("SPARSE_FULL"):
                deltas = named_list(check_glyphs.UNICODE_LIST_RE, fields["unicode_list"])
                ofs = named_list(check_glyphs.OFFSET_LIST_RE, fields["glyph_id_ofs_list"])
                cnt = int(fields.get("list_length", str(len(deltas))), 0)
                for i, d in enumerate(deltas[:cnt]):
                    cp_to_gid[start + d] = gid_start + ofs[i]
            else:
                raise ValueError(f"unsupported cmap type {kind} in {path}")
    return advs, cp_to_gid


def load_font_widths():
    adv12, cp_to_gid_12 = _parse_lvgl_font(FONT_12_PATH)
    adv16, cp_to_gid_16 = _parse_lvgl_font(FONT_16_PATH)
    if set(cp_to_gid_12.keys()) != set(cp_to_gid_16.keys()):
        raise AssertionError("12 px and 16 px font subsets have mismatched codepoint coverage")
    return adv12, adv16, (cp_to_gid_12, cp_to_gid_16)


def measure_px(text, is16, adv12, adv16, cp_to_gid):
    advs = adv16 if is16 else adv12
    cmap = cp_to_gid[1] if is16 else cp_to_gid[0]
    total = 0.0
    for ch in text:
        gid = cmap.get(ord(ch))
        if gid is None or gid >= len(advs):
            total += 16.0 if is16 else 13.0
        else:
            total += advs[gid]
    return total


UI_SOURCE_FILES = (
    "main/ui_theme.h",
    "main/ui_widgets.c",
    "main/app_ui_pages.c",
    "main/app_ui.c",
)


def load_ui_sources():
    return "\n".join((REPO_ROOT / rel).read_text(encoding="utf-8") for rel in UI_SOURCE_FILES)


class UiTextWidthTest(unittest.TestCase):
    def test_single_line_labels_fit_container_widths(self):
        adv12, adv16, cp_to_gid = load_font_widths()
        ui_src = load_ui_sources()
        overflows = []

        pattern = re.compile(
            r"create_lbl\([^,]+,\s*(font_12|font_16)\(\),\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*(\d+),\s*\"([^\"]+)\""
        )
        for m in pattern.finditer(ui_src):
            fn, box_w, txt = m.group(1), int(m.group(2)), m.group(3)
            w_px = measure_px(txt, fn == "font_16", adv12, adv16, cp_to_gid)
            if w_px > box_w:
                line = ui_src[: m.start()].count("\n") + 1
                overflows.append(f"L{line}: box={box_w}px < text={w_px:.1f}px ({fn}) | {txt!r}")

        self.assertEqual(overflows, [], "Single-line label overflows:\n" + "\n".join(overflows))

    def test_critical_dynamic_labels_fit_container_widths(self):
        adv12, adv16, cp_to_gid = load_font_widths()
        cases = [
            ("add-car right val", 110, "录入VIN → 配对", False),
            ("add-car left ttl", 76, "添加车辆", False),
            ("topbar right 100%", 76, "本机 100%", False),
            ("topbar left", 76, "未添加车辆", False),
            ("topbar left rssi", 76, "已连接·强", False),
            ("topbar left protect", 76, "已连接·保活", False),
            ("pair step desc 0", 182, "确认车架号与实车一致", False),
            ("pair step desc 1", 182, "平放水杯架一侧，勿放无线充", False),
            ("pair step desc 2", 182, "按下一步不能代替车端批准", False),
            ("pair step desc 3", 182, "验证双域会话与本机钥匙", False),
            ("offline drive warn", 164, "驾驶中蓝牙断开", True),
            ("offline drive sub", 164, "勿依赖本设备，注意安全", False),
        ]
        for name, box_w, txt, is16 in cases:
            w_px = measure_px(txt, is16, adv12, adv16, cp_to_gid)
            self.assertLessEqual(
                w_px,
                box_w,
                f"{name}: {txt!r} ({w_px:.1f}px) exceeds box {box_w}px",
            )

    def test_zone_card_with_desc_has_sufficient_height(self):
        ui_src = load_ui_sources()
        too_short = []
        for m in re.finditer(r"build_zone_card\([^;]+?\);", ui_src, re.S):
            call = " ".join(m.group(0).split())
            args_m = re.match(r"build_zone_card\((.*)\);", call)
            if not args_m:
                continue
            parts, cur, inq = [], "", False
            for ch in args_m.group(1):
                if ch == '"':
                    inq = not inq
                if ch == "," and not inq:
                    parts.append(cur.strip())
                    cur = ""
                else:
                    cur += ch
            parts.append(cur.strip())
            if len(parts) >= 9 and parts[7] != "NULL" and parts[2].isdigit():
                h = int(parts[2])
                if h < 42:
                    line = ui_src[: m.start()].count("\n") + 1
                    too_short.append(f"L{line}: h={h} < 42 for desc={parts[7]}")
        self.assertEqual(too_short, [], "Two-line build_zone_card too short:\n" + "\n".join(too_short))

    def test_glance_cards_fit_allocated_height(self):
        adv12, adv16, cp_to_gid = load_font_widths()
        ui_src = load_ui_sources()
        issues = []
        for m in re.finditer(r"build_glance_card\([^;]+?\);", ui_src, re.S):
            call = " ".join(m.group(0).split())
            args_m = re.match(r"build_glance_card\((.*)\);", call)
            if not args_m:
                continue
            parts, cur, inq = [], "", False
            for ch in args_m.group(1):
                if ch == '"':
                    inq = not inq
                if ch == "," and not inq:
                    parts.append(cur.strip())
                    cur = ""
                else:
                    cur += ch
            parts.append(cur.strip())
            if len(parts) < 7 or not parts[2].isdigit():
                continue
            h = int(parts[2])
            line = ui_src[: m.start()].count("\n") + 1
            for s in re.findall(r'"([^"]+)"', parts[5]):
                w16 = measure_px(s, True, adv12, adv16, cp_to_gid)
                if w16 > 164:
                    issues.append(f"L{line}: title {s!r} ({w16:.1f}px) > 164px")
            sub_limit = 320 if h >= 66 else 164
            for s in re.findall(r'"([^"]+)"', parts[6]):
                w12 = measure_px(s, False, adv12, adv16, cp_to_gid)
                if w12 > sub_limit:
                    issues.append(f"L{line}: sub {s!r} ({w12:.1f}px) > {sub_limit}px (h={h})")
        self.assertEqual(issues, [], "build_glance_card layout issues:\n" + "\n".join(issues))

    def test_pk_copy_strings_fit_two_line_glance_card(self):
        adv12, adv16, cp_to_gid = load_font_widths()
        copy_src = (REPO_ROOT / "components/tesla_key/src/pk_copy.c").read_text(encoding="utf-8")
        too_wide = []
        for m in re.finditer(r"\[(PK_MSG_\w+)\]\s*=\s*\"([^\"]+)\"", copy_src):
            msg_id, txt = m.group(1), m.group(2)
            w = measure_px(txt, False, adv12, adv16, cp_to_gid)
            if w > 310:
                too_wide.append(f"{msg_id}: {w:.1f}px > 310px | {txt!r}")
        self.assertEqual(too_wide, [], "pk_copy strings exceed 2-line limit:\n" + "\n".join(too_wide))

    def test_all_ui_and_copy_glyphs_exist_in_font(self):
        _, _, (cp_to_gid_12, cp_to_gid_16) = load_font_widths()
        missing = []
        for rel in (*UI_SOURCE_FILES, "components/tesla_key/src/pk_copy.c"):
            for idx, line in enumerate((REPO_ROOT / rel).read_text(encoding="utf-8").splitlines(), 1):
                s = line.strip()
                if s.startswith("#") or s.startswith("//") or s.startswith("/*") or s.startswith("*"):
                    continue
                for m in re.finditer(r'"([^"]+)"', line):
                    for ch in m.group(1):
                        cp = ord(ch)
                        if cp >= 32 and (cp not in cp_to_gid_12 or cp not in cp_to_gid_16):
                            missing.append(f"{rel}:L{idx}: U+{cp:04X} {ch!r} in {m.group(1)!r}")
        self.assertEqual(missing, [], "Missing glyphs in bundled Source Han Sans SC fonts:\n" + "\n".join(missing[:20]))

    def test_icon_badges_fit_20px_slot(self):
        adv12, adv16, cp_to_gid = load_font_widths()
        widgets_src = (REPO_ROOT / "main/ui_widgets.c").read_text(encoding="utf-8")
        icon_block = re.search(
            r"const char \*app_icon_text\(app_icon_id_t id\)\s*\{(.*?)\n\}",
            widgets_src,
            re.S,
        )
        self.assertIsNotNone(icon_block, "app_icon_text() not found in main/ui_widgets.c")
        overflows = []
        for m in re.finditer(r'return\s+"([^"]+)"', icon_block.group(1)):
            txt = m.group(1)
            w = measure_px(txt, False, adv12, adv16, cp_to_gid)
            if w > 20.0:
                overflows.append(f"{txt!r}: {w:.2f}px > 20px")
        self.assertEqual(overflows, [], "Icon badge overflows 20px slot:\n" + "\n".join(overflows))


if __name__ == "__main__":
    unittest.main()

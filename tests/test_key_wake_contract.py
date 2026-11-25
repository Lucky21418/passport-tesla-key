#!/usr/bin/env python3
"""Static contracts for the deep-sleep key-wake guard wiring in main.c."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class KeyWakeGuardWiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.main = read("main/main.c")

    def test_wake_guard_gates_button_polling_in_main_loop(self) -> None:
        self.assertIn("bsp_button_init(NULL, NULL)", self.main)
        guard_body = function_body(self.main, "poll_key_wake_guard")
        self.assertIn("key_guard_consume(", guard_body)
        self.assertIn("app_ports_pop_button_sample(", guard_body)
        self.assertLess(guard_body.index("app_ports_pop_button_sample("),
                        guard_body.index("bsp_button_read_mv("))
        main_body = function_body(self.main, "app_main")
        self.assertLess(main_body.index("poll_key_wake_guard()"),
                        main_body.index("pk_btn_adapter_feed_mv("))

    def test_unknown_lock_state_main_ui_copy(self) -> None:
        ui = read("main/app_ui.c")
        pages = read("main/app_ui_pages.c")
        footer_body = function_body(ui, "footer_for_main")
        self.assertIn("PK_ACTION_VCSEC_GET_STATUS_ONCE", footer_body)
        self.assertLess(footer_body.index("PK_ACTION_VCSEC_GET_STATUS_ONCE"),
                        footer_body.index("!pk_app_parked("))
        self.assertIn("● 查锁态", footer_body)
        self.assertIn("锁态未确认", footer_body)

        main_page = function_body(pages, "app_ui_render_page_main")
        self.assertIn("PK_ACTION_VCSEC_GET_STATUS_ONCE", main_page)
        branch = main_page[main_page.index("PK_ACTION_VCSEC_GET_STATUS_ONCE"):]
        branch = branch[:branch.index("} else {")]
        self.assertIn("锁态未确认", branch)
        self.assertIn("查锁态", branch)
        self.assertNotIn("闭锁", branch)


if __name__ == "__main__":
    unittest.main()

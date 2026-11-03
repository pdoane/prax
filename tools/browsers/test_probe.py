#!/usr/bin/env python3
"""Tests what tools/browsers/probe.py sends to press a page's keys.

Usage: python3 -m tools.browsers.test_probe
"""

import unittest

from tools.browsers import probe


class KeysTest(unittest.TestCase):
    def test_a_key_is_pressed_inside_its_modifiers(self):
        actions = probe.webdriver_actions("ArrowLeft", ["Alt", "Shift"])[0]["actions"]
        self.assertEqual([(a["type"], a["value"]) for a in actions], [
            ("keyDown", probe.WEBDRIVER_KEYS["Alt"]),
            ("keyDown", probe.WEBDRIVER_KEYS["Shift"]),
            ("keyDown", probe.WEBDRIVER_KEYS["ArrowLeft"]),
            ("keyUp", probe.WEBDRIVER_KEYS["ArrowLeft"]),
            ("keyUp", probe.WEBDRIVER_KEYS["Shift"]),
            ("keyUp", probe.WEBDRIVER_KEYS["Alt"]),
        ])

    def test_every_key_of_chrome_has_a_webdriver_code(self):
        for key in probe.CHROME_KEY_CODES:
            with self.subTest(key=key):
                self.assertIn(key, probe.WEBDRIVER_KEYS)

    def test_every_modifier_of_chrome_has_a_webdriver_code(self):
        for modifier in probe.CHROME_MODIFIERS:
            with self.subTest(modifier=modifier):
                self.assertIn(modifier, probe.WEBDRIVER_KEYS)

    def test_every_key_of_a_mac_command_is_known(self):
        for key, modifier in probe.MAC_COMMANDS:
            with self.subTest(key=key, modifier=modifier):
                self.assertIn(key, probe.CHROME_KEY_CODES)
                if modifier:
                    self.assertIn(modifier, probe.CHROME_MODIFIERS)


if __name__ == "__main__":
    unittest.main()

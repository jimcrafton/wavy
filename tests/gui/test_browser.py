import os
import sys
import tempfile
import unittest

sys.path.insert(0, r"D:\code\newui\tools\guitest")
import newui_guitest
from newui_guitest.app import App

EXE = r"D:\code\wavy\out\build\x64-release\wavyEditor.exe"
CWD = r"D:\code\wavy\out\build\x64-release"
ROW = 20


class BrowserScanTests(unittest.TestCase):
    # Sends no input: the editor scans the installed plugins at start and fills the browser.
    def test_start_scans_the_plugins_and_writes_the_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = os.path.join(folder, "plugins.cache")
            app = App(EXE, ["--plugin-cache", cache, "--state", os.path.join(folder, "s.state")], cwd=CWD)
            try:
                status = app.prop("statusText", "text")
                self.assertIn("plugins", status)
                self.assertTrue(os.path.isfile(cache), "the scan did not write the cache")
                with open(cache, encoding="utf-8") as f:
                    self.assertIn("module\t", f.read())
                print(status)
            finally:
                app.close()


class BrowserLoadTests(unittest.TestCase):
    # Sends real input: double-clicks the first Audio Effects entry to put it on the selected track.
    def test_double_click_loads_an_effect_on_the_selected_track(self):
        with tempfile.TemporaryDirectory() as folder:
            app = App(EXE, ["--plugin-cache", os.path.join(folder, "plugins.cache"), "--state", os.path.join(folder, "s.state")], cwd=CWD)
            try:
                categories = app.find("browserCategories")
                app.click_at((int(categories.x + 20), int(categories.y + ROW + 10)))  # "Audio Effects"
                lst = app.find("browserList")
                point = (int(lst.x + 20), int(lst.y + 10))
                app.double_click(point)
                app.wait_until(lambda: "was added to" in app.prop("statusText", "text"), "the effect to be added")
            finally:
                app.close()


if __name__ == "__main__":
    sys.exit(newui_guitest.main())

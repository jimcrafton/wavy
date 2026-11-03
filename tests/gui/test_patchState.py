import ctypes
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, r"D:\code\newui\tools\guitest")
import newui_guitest
from newui_guitest.app import App

EXE = r"D:\code\wavy\out\build\x64-release\wavyEditor.exe"
CWD = r"D:\code\wavy\out\build\x64-release"
WM_CLOSE = 0x0010


def closeNormally(app):
    window = app._main_window()
    if not window:
        raise AssertionError("the editor has no window to close")
    ctypes.windll.user32.PostMessageW(window, WM_CLOSE, 0, 0)
    deadline = time.monotonic() + 15
    while app.running:
        if time.monotonic() > deadline:
            raise AssertionError("the editor did not exit after WM_CLOSE")
        time.sleep(0.05)


class PatchStateTests(unittest.TestCase):
    def test_patch_is_written_on_exit_and_restored_on_start(self):
        with tempfile.TemporaryDirectory() as folder:
            state = os.path.join(folder, "instruments.state")

            first = App(EXE, ["--state", state], cwd=CWD)
            try:
                self.assertNotIn("Patch restored", first.prop("statusText", "text"))
                closeNormally(first)
            finally:
                first.close()
            self.assertTrue(os.path.isfile(state), "closing the editor did not write the state file")
            self.assertGreater(os.path.getsize(state), 100)

            second = App(EXE, ["--state", state], cwd=CWD)
            try:
                self.assertIn("Patch restored", second.prop("statusText", "text"))
                closeNormally(second)
            finally:
                second.close()


if __name__ == "__main__":
    sys.exit(newui_guitest.main())

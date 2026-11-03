import ctypes
import sys
from ctypes import wintypes

sys.path.insert(0, r"D:\code\newui\tools\guitest")
import newui_guitest
from newui_guitest import AppTestCase

user32 = ctypes.windll.user32
user32.GetWindow.restype = wintypes.HWND
user32.GetWindow.argtypes = [wintypes.HWND, wintypes.UINT]
GW_OWNER = 4
LANE = 72
ARP_TRACK = 3
FX_BUTTON_Y = 42


def visibleWindows():
    """Visible top-level windows as (hwnd, title), topmost first."""
    found = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(proc(visit), 0)
    return found


def findWindow(text):
    for hwnd, title in visibleWindows():
        if text in title:
            return hwnd
    return None


def zIndex(hwnd):
    for i, (h, _) in enumerate(visibleWindows()):
        if h == hwnd:
            return i
    raise AssertionError("window is not visible")


class EffectEditButtonTests(AppTestCase):
    exe = r"D:\code\wavy\out\build\x64-debug-asio\wavyEditor.exe"
    cwd = r"D:\code\wavy\out\build\x64-debug-asio"

    def test_fx_button_opens_the_effects_window_above_the_editor(self):
        headers = self.app.find("trackHeaders")
        main = self.app._main_window()
        self.app.click_at((int(headers.x + headers.w - 20), int(headers.y + ARP_TRACK * LANE + FX_BUTTON_Y)))
        self.app.wait_until(lambda: findWindow("Effects") is not None, "the Surge XT Effects editor window")
        fx = findWindow("Effects")

        self.assertEqual(user32.GetWindow(fx, GW_OWNER), main, "the plugin window is not owned by the editor's main window")
        self.assertLess(zIndex(fx), zIndex(main), "the plugin window opened behind the editor's main window")

        # Clicking the editor again must not bury the plugin window.
        ruler = self.app.find("rulerCanvas")
        self.app.click_at((int(ruler.x + ruler.w / 2), int(ruler.y + ruler.h / 2)))
        self.assertLess(zIndex(fx), zIndex(main), "clicking the editor put it above the plugin window")


if __name__ == "__main__":
    sys.exit(newui_guitest.main())

import ctypes
import sys
from ctypes import wintypes

sys.path.insert(0, r"D:\code\newui\tools\guitest")
import newui_guitest
from newui_guitest import AppTestCase

user32 = ctypes.windll.user32
LANE = 72
ARP_TRACK = 3


def windowTitles():
    titles = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetWindowTextW(hwnd, buf, 256)
            titles.append(buf.value)
        return True

    user32.EnumWindows(proc(visit), 0)
    return titles


class EditButtonTests(AppTestCase):
    exe = r"D:\code\wavy\out\build\x64-release\wavyEditor.exe"
    cwd = r"D:\code\wavy\out\build\x64-release"

    def test_edit_button_opens_the_plugin_window(self):
        headers = self.app.find("trackHeaders")
        point = (int(headers.x + headers.w - 20), int(headers.y + ARP_TRACK * LANE + 16))
        self.app.click_at(point)
        self.app.wait_until(lambda: any("Surge" in t for t in windowTitles()), "the Surge XT editor window")


if __name__ == "__main__":
    sys.exit(newui_guitest.main())

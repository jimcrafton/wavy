#include "wavy/pluginEditor.h"

#include <windows.h>

#include <atomic>

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

namespace wavy {

using namespace Steinberg;

namespace {

constexpr wchar_t className[] = L"WavyPluginEditor";
constexpr DWORD exStyle = WS_EX_APPWINDOW;

ViewRect toViewRect(const RECT& r) { return {r.left, r.top, r.right, r.bottom}; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void registerClass() {
    static const bool done = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = className;
        return RegisterClassExW(&wc) != 0;
    }();
    (void)done;
}

}  // namespace

struct PluginEditorWindow::Impl : IPlugFrame {
    HWND hwnd = nullptr;
    IPtr<IPlugView> view;
    DWORD style = WS_OVERLAPPEDWINDOW;
    bool resizable = false;
    bool resizing = false;  // a size change we are applying ourselves: do not echo it back to the plugin
    std::atomic<uint32> refs{1};

    UINT dpi() const { return hwnd ? GetDpiForWindow(hwnd) : 96; }

    // Window size for a client area of `client` pixels.
    SIZE windowSizeFor(const ViewRect& client) const {
        RECT r{0, 0, client.getWidth(), client.getHeight()};
        AdjustWindowRectExForDpi(&r, style, FALSE, exStyle, dpi());
        return {r.right - r.left, r.bottom - r.top};
    }

    void applyClientSize(const ViewRect& client) {
        const SIZE s = windowSizeFor(client);
        resizing = true;
        SetWindowPos(hwnd, nullptr, 0, 0, s.cx, s.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        resizing = false;
    }

    void onWindowSized() {
        if (resizing || !view || !hwnd) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        ViewRect r = toViewRect(rc);
        if (resizable) {
            ViewRect constrained = r;
            if (view->checkSizeConstraint(&constrained) == kResultTrue && (constrained.getWidth() != r.getWidth() || constrained.getHeight() != r.getHeight())) {
                applyClientSize(constrained);
                r = constrained;
            }
        }
        resizing = true;
        view->onSize(&r);
        resizing = false;
    }

    void teardown() {
        if (view) {
            view->setFrame(nullptr);
            view->removed();
            view = nullptr;
        }
        if (hwnd) {
            HWND h = hwnd;
            hwnd = nullptr;
            DestroyWindow(h);
        }
    }

    tresult PLUGIN_API resizeView(IPlugView* v, ViewRect* newSize) override {
        if (!hwnd || !newSize || v != view.get()) return kInvalidArgument;
        if (resizing) return kResultOk;
        applyClientSize(*newSize);
        // The plugin may be mid-resize already; telling it the size it asked for keeps both sides in step.
        resizing = true;
        view->onSize(newSize);
        resizing = false;
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override {
        if (FUnknownPrivate::iidEqual(queried, FUnknown::iid) || FUnknownPrivate::iidEqual(queried, IPlugFrame::iid)) {
            addRef();
            *obj = static_cast<IPlugFrame*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // The window owns this object, so references taken by the plugin never delete it.
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override { return --refs; }
};

namespace {

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* impl = reinterpret_cast<PluginEditorWindow::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_SIZE:
            if (impl && wp != SIZE_MINIMIZED) impl->onWindowSized();
            return 0;
        case WM_CLOSE:
            if (impl) {
                ShowWindow(hwnd, SW_HIDE);
                impl->teardown();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (impl && impl->hwnd == hwnd) impl->hwnd = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

PluginEditorWindow::PluginEditorWindow() : impl_(std::make_unique<Impl>()) {}

PluginEditorWindow::~PluginEditorWindow() {
    if (impl_) impl_->teardown();
}

std::unique_ptr<PluginEditorWindow> PluginEditorWindow::open(VstPlugin& plugin, std::string& error, void* owner) {
    Vst::IEditController* controller = plugin.editController();
    if (!controller) {
        error = "the plugin has no editor";
        return nullptr;
    }
    IPtr<IPlugView> view = owned(controller->createView(Vst::ViewType::kEditor));
    if (!view) {
        error = "the plugin has no editor";
        return nullptr;
    }
    if (view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        error = "the plugin's editor does not support HWND";
        return nullptr;
    }

    registerClass();
    std::unique_ptr<PluginEditorWindow> w(new PluginEditorWindow());
    Impl& d = *w->impl_;
    d.view = view;
    d.resizable = view->canResize() == kResultTrue;
    d.style = d.resizable ? WS_OVERLAPPEDWINDOW : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);

    const std::wstring title = widen(plugin.info().name);
    // An owned window stays above its owner and minimizes with it, and gets no taskbar button of its own.
    HWND ownerWindow = owner ? GetAncestor(static_cast<HWND>(owner), GA_ROOT) : nullptr;
    d.hwnd = CreateWindowExW(ownerWindow ? 0 : exStyle, className, title.c_str(), d.style, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                             ownerWindow, nullptr, GetModuleHandleW(nullptr), &d);
    if (!d.hwnd) {
        error = "cannot create the editor window";
        return nullptr;
    }

    if (auto scale = FUnknownPtr<IPlugViewContentScaleSupport>(d.view)) scale->setContentScaleFactor(static_cast<float>(d.dpi()) / 96.0f);

    d.view->setFrame(&d);
    ViewRect size{};
    if (d.view->getSize(&size) == kResultOk) d.applyClientSize(size);
    if (d.view->attached(d.hwnd, kPlatformTypeHWND) != kResultOk) {
        error = "the plugin's editor refused to attach";
        return nullptr;  // ~PluginEditorWindow tears the window down
    }
    // Some plugins only know their final size once attached.
    if (d.view->getSize(&size) == kResultOk) d.applyClientSize(size);

    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    return w;
}

bool PluginEditorWindow::isOpen() const { return impl_->hwnd != nullptr; }

void PluginEditorWindow::bringToFront() {
    if (!impl_->hwnd) return;
    if (IsIconic(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE);
    SetForegroundWindow(impl_->hwnd);
}

void PluginEditorWindow::close() { impl_->teardown(); }

}  // namespace wavy

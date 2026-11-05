#include <vector>

#include <dxgi1_6.h>
#include <spdlog/spdlog.h>

#include "Flat3DOutputWindow.hpp"

using namespace Microsoft::WRL;

namespace vrmod {
std::atomic<bool> Flat3DOutputWindow::s_creating{false};
bool Flat3DOutputWindow::s_focusable{false};
std::atomic<bool> Flat3DOutputWindow::s_focus_spoof{true};
HWND Flat3DOutputWindow::s_game_window{nullptr};
Flat3DOutputWindow* Flat3DOutputWindow::s_instance{nullptr};

void Flat3DOutputWindow::set_focus_spoof(bool enabled) {
    s_focus_spoof.store(enabled);
}

// Answer with the game's window, but only when the window actually in front is ours. Any other
// foreground window - another application, a dialog - gets the real answer, so alt-tabbing away
// still takes input away from the game exactly as the user expects.
HWND Flat3DOutputWindow::spoof(HWND real) {
    if (!s_focus_spoof.load() || s_game_window == nullptr || s_instance == nullptr) {
        return real;
    }

    if (real != nullptr && real == s_instance->m_hwnd) {
        return s_game_window;
    }

    return real;
}

HWND WINAPI Flat3DOutputWindow::get_foreground_window_hook() {
    auto* const self = s_instance;
    const auto real = self != nullptr && self->m_get_foreground_window_hook != nullptr
        ? self->m_get_foreground_window_hook->get_original<decltype(GetForegroundWindow)>()()
        : nullptr;

    return spoof(real);
}

HWND WINAPI Flat3DOutputWindow::get_active_window_hook() {
    auto* const self = s_instance;
    const auto real = self != nullptr && self->m_get_active_window_hook != nullptr
        ? self->m_get_active_window_hook->get_original<decltype(GetActiveWindow)>()()
        : nullptr;

    return spoof(real);
}

HWND WINAPI Flat3DOutputWindow::get_focus_hook() {
    auto* const self = s_instance;
    const auto real = self != nullptr && self->m_get_focus_hook != nullptr
        ? self->m_get_focus_hook->get_original<decltype(GetFocus)>()()
        : nullptr;

    return spoof(real);
}

void Flat3DOutputWindow::install_focus_hooks(HWND game_window) {
    s_instance = this;
    s_game_window = game_window;

    if (m_get_foreground_window_hook != nullptr) {
        return;
    }

    auto* const user32 = GetModuleHandleW(L"user32.dll");

    if (user32 == nullptr) {
        return;
    }

    const auto hook_one = [&](const char* name, void* destination) -> std::unique_ptr<FunctionHook> {
        auto* const fn = GetProcAddress(user32, name);

        if (fn == nullptr) {
            spdlog::error("[Flat3D][OutputWindow] {} not found", name);
            return nullptr;
        }

        auto hook = std::make_unique<FunctionHook>((uintptr_t)fn, (uintptr_t)destination);

        if (!hook->create()) {
            spdlog::error("[Flat3D][OutputWindow] failed to hook {}", name);
            return nullptr;
        }

        spdlog::info("[Flat3D][OutputWindow] hooked {}", name);

        return hook;
    };

    m_get_foreground_window_hook = hook_one("GetForegroundWindow", (void*)&Flat3DOutputWindow::get_foreground_window_hook);
    m_get_active_window_hook = hook_one("GetActiveWindow", (void*)&Flat3DOutputWindow::get_active_window_hook);
    m_get_focus_hook = hook_one("GetFocus", (void*)&Flat3DOutputWindow::get_focus_hook);
}

// Keep the output window in front (Lossless Scaling needs that) but push keyboard focus back to
// the game, so key messages are delivered somewhere useful. Focus is per input queue, so the two
// threads have to share one for the duration of the call.
void Flat3DOutputWindow::hand_focus_to_game() {
    if (!s_focus_spoof.load() || s_game_window == nullptr) {
        return;
    }

    const auto our_thread = GetCurrentThreadId();
    const auto game_thread = GetWindowThreadProcessId(s_game_window, nullptr);

    if (game_thread == 0 || game_thread == our_thread) {
        return;
    }

    if (AttachThreadInput(our_thread, game_thread, TRUE)) {
        SetFocus(s_game_window);
        AttachThreadInput(our_thread, game_thread, FALSE);
    }
}

static const wchar_t* const s_window_class = L"REF_Flat3D_FullRes_Output";

Flat3DOutputWindow& Flat3DOutputWindow::get() {
    static Flat3DOutputWindow instance{};
    return instance;
}

LRESULT CALLBACK Flat3DOutputWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    // Unless the window is meant to be clickable (so Lossless Scaling's auto-scale can latch on),
    // never take focus and never eat a click: input keeps going to the game behind us.
    case WM_MOUSEACTIVATE:
        if (!s_focusable) {
            return MA_NOACTIVATE;
        }
        break;
    case WM_NCHITTEST:
        if (!s_focusable) {
            return HTTRANSPARENT;
        }
        break;
    // Clicked or alt-tabbed to: stay in front for Lossless Scaling, but give the keyboard back.
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            hand_focus_to_game();
        }
        break;
    case WM_SETFOCUS:
        hand_focus_to_game();
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

struct MonitorEntry {
    RECT rect{};
    bool primary{false};
};

static BOOL CALLBACK enum_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO info{sizeof(info)};

    if (GetMonitorInfoW(monitor, &info)) {
        ((std::vector<MonitorEntry>*)param)->push_back({info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }

    return TRUE;
}

RECT Flat3DOutputWindow::pick_monitor(HWND game_window, int monitor_index) {
    std::vector<MonitorEntry> monitors{};
    EnumDisplayMonitors(nullptr, nullptr, enum_monitor, (LPARAM)&monitors);

    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& r = monitors[i].rect;
        spdlog::info("[Flat3D][OutputWindow] monitor {}: {}x{} at ({},{}){}", (int)i + 1, r.right - r.left,
            r.bottom - r.top, r.left, r.top, monitors[i].primary ? " primary" : "");
    }

    if (monitors.empty()) {
        return RECT{0, 0, 3840, 1080};
    }

    if (monitor_index >= 1 && monitor_index <= (int)monitors.size()) {
        return monitors[monitor_index - 1].rect;
    }

    // Auto: prefer a very wide panel - SBS XR glasses in 3D mode present 3840x1080 (32:9), and
    // that is exactly the display this mode exists for.
    for (const auto& m : monitors) {
        const auto aspect = (float)(m.rect.right - m.rect.left) / (float)(m.rect.bottom - m.rect.top);

        if (aspect > 2.5f) {
            return m.rect;
        }
    }

    // Otherwise any monitor that is not the one holding the game window, else the primary.
    auto* const game_monitor = game_window != nullptr ? MonitorFromWindow(game_window, MONITOR_DEFAULTTONEAREST) : nullptr;
    MONITORINFO game_info{sizeof(game_info)};

    if (game_monitor != nullptr && GetMonitorInfoW(game_monitor, &game_info)) {
        for (const auto& m : monitors) {
            if (!EqualRect(&m.rect, &game_info.rcMonitor)) {
                return m.rect;
            }
        }
    }

    for (const auto& m : monitors) {
        if (m.primary) {
            return m.rect;
        }
    }

    return monitors[0].rect;
}

DWORD WINAPI Flat3DOutputWindow::window_thread(LPVOID param) {
    auto* const self = (Flat3DOutputWindow*)param;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Flat3DOutputWindow::wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = s_window_class;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    // WS_EX_APPWINDOW (and no WS_EX_TOOLWINDOW) puts it in the taskbar under its own name, so it
    // is tellable from the game's window and can be picked out of Alt+Tab. WS_EX_NOACTIVATE still
    // keeps it from stealing focus.
    auto ex_style = WS_EX_TOPMOST | WS_EX_APPWINDOW;

    if (!s_focusable) {
        ex_style |= WS_EX_NOACTIVATE;
    }

    self->m_hwnd = CreateWindowExW(ex_style, s_window_class,
        L"AFR SBS Output", WS_POPUP, self->m_rect.left, self->m_rect.top,
        self->m_rect.right - self->m_rect.left, self->m_rect.bottom - self->m_rect.top, nullptr, nullptr,
        wc.hInstance, nullptr);

    SetEvent(self->m_ready);

    if (self->m_hwnd == nullptr) {
        return 0;
    }

    // The window needs its own message pump: the game's pump never sees it. shutdown() posts
    // WM_QUIT here, which is the only way out - without it this thread blocks in GetMessage and
    // the game's process stays alive after the user quits.
    MSG msg{};

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(self->m_hwnd);
    self->m_hwnd = nullptr;

    return 0;
}

void Flat3DOutputWindow::shutdown() {
    reset();

    if (m_thread != nullptr) {
        if (m_hwnd != nullptr) {
            PostMessageW(m_hwnd, WM_QUIT, 0, 0);
        }

        if (WaitForSingleObject(m_thread, 2000) == WAIT_TIMEOUT) {
            spdlog::warn("[Flat3D][OutputWindow] message thread did not exit in time");
        }

        CloseHandle(m_thread);
        m_thread = nullptr;
    }

    if (m_ready != nullptr) {
        CloseHandle(m_ready);
        m_ready = nullptr;
    }

    m_hwnd = nullptr;
    m_visible = false;
    m_failed = false;
}

bool Flat3DOutputWindow::create_window(HWND game_window, int monitor_index) {
    s_focusable = m_focusable; // the window thread reads this while creating the window
    m_rect = pick_monitor(game_window, monitor_index);
    m_width = (uint32_t)(m_rect.right - m_rect.left);
    m_height = (uint32_t)(m_rect.bottom - m_rect.top);

    if (m_width < 64 || m_height < 64) {
        spdlog::error("[Flat3D][OutputWindow] bad output size {}x{}", m_width, m_height);
        return false;
    }

    m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = CreateThread(nullptr, 0, &Flat3DOutputWindow::window_thread, this, 0, nullptr);
    WaitForSingleObject(m_ready, 3000);

    if (m_hwnd == nullptr) {
        spdlog::error("[Flat3D][OutputWindow] window creation failed ({})", GetLastError());
        return false;
    }

    return true;
}

bool Flat3DOutputWindow::create_swapchain(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format) {
    ComPtr<IDXGIFactory4> factory{};

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        spdlog::error("[Flat3D][OutputWindow] CreateDXGIFactory2 failed");
        return false;
    }

    BOOL tearing = FALSE;
    ComPtr<IDXGIFactory5> factory5{};

    if (SUCCEEDED(factory.As(&factory5))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    }

    m_tearing = tearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = (UINT)m_backbuffers.size();
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    ComPtr<IDXGISwapChain1> swapchain1{};

    s_creating.store(true);
    const auto hr = factory->CreateSwapChainForHwnd(queue, m_hwnd, &desc, nullptr, nullptr, &swapchain1);
    s_creating.store(false);

    if (FAILED(hr) || swapchain1 == nullptr) {
        spdlog::error("[Flat3D][OutputWindow] CreateSwapChainForHwnd failed 0x{:08X} (format={})", (uint32_t)hr, (int)format);
        return false;
    }

    // Alt+Enter on our own window would hand it exclusive fullscreen behind the game's back.
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    if (FAILED(swapchain1.As(&m_swapchain)) || m_swapchain == nullptr) {
        spdlog::error("[Flat3D][OutputWindow] swapchain QueryInterface failed");
        return false;
    }

    for (uint32_t i = 0; i < (uint32_t)m_backbuffers.size(); ++i) {
        if (FAILED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers[i])))) {
            spdlog::error("[Flat3D][OutputWindow] GetBuffer({}) failed", i);
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = (UINT)m_last_rtv_textures.size();

    if (FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_rtv_heap)))) {
        spdlog::error("[Flat3D][OutputWindow] RTV heap creation failed");
        return false;
    }

    m_rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_format = format;

    spdlog::info("[Flat3D][OutputWindow] {}x{} at ({},{}) format={} tearing={}", m_width, m_height, m_rect.left,
        m_rect.top, (int)format, m_tearing);

    return true;
}

bool Flat3DOutputWindow::ensure(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, HWND game_window,
    int monitor_index)
{
    if (m_failed || device == nullptr || queue == nullptr) {
        return false;
    }

    // The game can change backbuffer format (HDR toggle); rebuild rather than present a mismatch.
    if (m_swapchain != nullptr && m_format != format) {
        spdlog::info("[Flat3D][OutputWindow] format changed {} -> {}, rebuilding", (int)m_format, (int)format);
        reset();
    }

    if (m_swapchain == nullptr) {
        if (m_hwnd == nullptr && !create_window(game_window, monitor_index)) {
            m_failed = true;
            return false;
        }

        if (!create_swapchain(device, queue, format)) {
            m_failed = true;
            return false;
        }
    }

    if (m_focusable) {
        install_focus_hooks(game_window);
    }

    if (!m_visible) {
        ShowWindowAsync(m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        m_visible = true;
    }

    return true;
}

void Flat3DOutputWindow::hide() {
    if (m_hwnd != nullptr && m_visible) {
        ShowWindowAsync(m_hwnd, SW_HIDE);
        m_visible = false;
    }
}

void Flat3DOutputWindow::reset() {
    m_backbuffers = {};
    m_last_rtv_textures = {};
    m_rtv_heap.Reset();
    m_swapchain.Reset();
    m_format = DXGI_FORMAT_UNKNOWN;
}

ID3D12Resource* Flat3DOutputWindow::backbuffer(uint32_t& out_width, uint32_t& out_height) {
    if (m_swapchain == nullptr) {
        return nullptr;
    }

    out_width = m_width;
    out_height = m_height;

    return m_backbuffers[m_swapchain->GetCurrentBackBufferIndex() % m_backbuffers.size()].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Flat3DOutputWindow::rtv(ID3D12Device* device, ID3D12Resource* backbuffer) {
    const auto slot = m_swapchain->GetCurrentBackBufferIndex() % m_last_rtv_textures.size();

    auto handle = m_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += slot * m_rtv_descriptor_size;

    if (m_last_rtv_textures[slot] != backbuffer) {
        device->CreateRenderTargetView(backbuffer, nullptr, handle);
        m_last_rtv_textures[slot] = backbuffer;
    }

    return handle;
}

void Flat3DOutputWindow::present() {
    if (m_swapchain == nullptr) {
        return;
    }

    const auto hr = m_swapchain->Present(0, m_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);

    if (FAILED(hr)) {
        static int errors = 0;

        if (errors++ < 5) {
            spdlog::error("[Flat3D][OutputWindow] Present failed 0x{:08X}", (uint32_t)hr);
        }
    }
}

bool Flat3DOutputWindow::is_own(void* swapchain) const {
    return swapchain != nullptr && swapchain == (void*)m_swapchain.Get();
}
} // namespace vrmod

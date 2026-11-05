#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <windows.h>

#include <utility/FunctionHook.hpp>

namespace vrmod {
// Full-resolution stereo output window.
//
// The game keeps rendering at its own 16:9 resolution and the normal compose still packs both
// eyes into the game's backbuffer (half width per eye). This class owns a SECOND window, sized
// to a chosen monitor (e.g. 3840x1080 for SBS XR glasses), with its own swapchain. The compose
// is recorded a second time into that swapchain at the window's full size, so each eye keeps
// its full width and nothing is squeezed.
//
// Ported from the LAD8/DWO AFR mod's `outwin` module (OutputMode 6 there).
class Flat3DOutputWindow {
public:
    static Flat3DOutputWindow& get();

    // Creates the window + swapchain on first call. Returns false (once, permanently) if
    // anything fails, so the caller silently falls back to the in-backbuffer compose.
    bool ensure(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, HWND game_window, int monitor_index);

    // Lossless Scaling's auto-scale attaches to the window that comes to the foreground, so the
    // output window has to be clickable for it to latch on. That costs the game its focus, which
    // is what stops controller input - see VR::on_message's focus spoof.
    void set_focusable(bool focusable) { m_focusable = focusable; }
    bool is_focusable() const { return m_focusable; }

    HWND window() const { return m_hwnd; }
    bool exists() const { return m_hwnd != nullptr; }

    // Making the window focusable solves Lossless Scaling but breaks input twice over: the game
    // gates reading devices on believing it is in front, and keystrokes land on whichever window
    // holds focus - ours. Two separate pieces are needed.
    //
    //  - the hooks below answer "which window is in front" with the game's window, but only while
    //    the real answer is this output window; everything else gets the truth.
    //  - on activation the window hands keyboard focus straight back to the game via
    //    AttachThreadInput, so it stays the foreground window for Lossless Scaling while key
    //    messages go where they are useful.
    void set_focus_spoof(bool enabled);
    void set_keyboard_handoff(bool enabled);
    void install_focus_hooks(HWND game_window);

    void hide();
    void reset();

    // Tears the window down and joins its message-loop thread. Must run on shutdown: the loop
    // blocks in GetMessage forever otherwise and the game's process never exits.
    void shutdown();

    ID3D12Resource* backbuffer(uint32_t& out_width, uint32_t& out_height);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(ID3D12Device* device, ID3D12Resource* backbuffer);

    void present();

    // The present hook must let our own swapchain pass straight through: it shares the vtable
    // with the game's swapchain, so the hook would otherwise recurse into the compose.
    bool is_own(void* swapchain) const;

    // Set while we call CreateSwapChainForHwnd so D3D12Hook::create_swapchain forwards it
    // untouched instead of adopting it as the game's swapchain.
    static std::atomic<bool> s_creating;

private:
    Flat3DOutputWindow() = default;

    bool create_window(HWND game_window, int monitor_index);
    bool create_swapchain(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format);

    static RECT pick_monitor(HWND game_window, int monitor_index);
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static DWORD WINAPI window_thread(LPVOID param);

    HWND m_hwnd{nullptr};
    HANDLE m_thread{nullptr};
    HANDLE m_ready{nullptr};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> m_backbuffers{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtv_heap{};
    std::array<ID3D12Resource*, 2> m_last_rtv_textures{}; // change tracking only, not owning
    uint32_t m_rtv_descriptor_size{0};

    RECT m_rect{};
    uint32_t m_width{0};
    uint32_t m_height{0};
    DXGI_FORMAT m_format{DXGI_FORMAT_UNKNOWN};
    bool m_tearing{false};
    bool m_failed{false};
    bool m_visible{false};
    bool m_focusable{false};
    static bool s_focusable; // read from the window thread, which has no instance pointer yet

    static void hand_focus_to_game();

    static HWND WINAPI get_foreground_window_hook();
    static HWND WINAPI get_active_window_hook();
    static HWND WINAPI get_focus_hook();
    static HWND spoof(HWND real);

    static std::atomic<bool> s_focus_spoof;
    static std::atomic<bool> s_keyboard_handoff;
    static HWND s_game_window;
    static Flat3DOutputWindow* s_instance;

    std::unique_ptr<FunctionHook> m_get_foreground_window_hook{};
    std::unique_ptr<FunctionHook> m_get_active_window_hook{};
    std::unique_ptr<FunctionHook> m_get_focus_hook{};
};
} // namespace vrmod

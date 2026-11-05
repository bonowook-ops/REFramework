#pragma once

#include <array>
#include <atomic>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <windows.h>

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

    void hide();
    void reset();

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
};
} // namespace vrmod

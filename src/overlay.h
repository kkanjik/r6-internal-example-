#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#ifdef OVERLAY_BUILD
#define OVERLAY_API __declspec(dllexport)
#else
#define OVERLAY_API __declspec(dllimport)
#endif
// Initialize, Present and Shutdown use one presentation thread.
// Window messages may arrive on a separate window thread; ImGui access is serialized.
// Host must serialize its swap-chain resize/presentation operations.
// Supply the DIRECT queue used to create this swap chain; it must belong to device.
// Stop host rendering/window callbacks before Shutdown. Never call under DllMain.
extern "C" {
OVERLAY_API bool OverlayInitialize(HWND window, ID3D12Device* device,
    ID3D12CommandQueue* queue, IDXGISwapChain3* swapchain, bool clearBackground);
OVERLAY_API void OverlayShutdown();
OVERLAY_API const char* OverlayLastError();
OVERLAY_API bool OverlaySelfTest();
// Injection into RainbowSix.exe starts this outside loader lock automatically.
// Other DX12 hosts can opt in explicitly. Initial rendering starts in Present.
OVERLAY_API bool OverlayStartAutomatic();
OVERLAY_API bool OverlayIsReady();
OVERLAY_API const char* OverlayBootstrapStatus();
// Deprecated ABI: setter is ignored and getter returns 0. Overlay drawing must
// run on every presented frame; skipping submissions causes visible flicker.
OVERLAY_API void OverlaySetFpsLimit(unsigned fps);
OVERLAY_API unsigned OverlayFpsLimit();
}

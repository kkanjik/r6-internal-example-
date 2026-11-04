#pragma once
#include "overlay.h"
#include <wrl/client.h>
#include <string>
namespace dx12_hooks {
using Present=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using Resize=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
bool Install(IDXGISwapChain3*,void* present,void* resize,void* present1);
bool StartAutomatic(void* present,void* resize,void* present1);
void Shutdown();
Present OriginalPresent();
Present1 OriginalPresent1();
Resize OriginalResize();
bool HasShadow(IDXGISwapChain3*);
// Tracks the DIRECT queue whose submitted lists transition these back buffers.
Microsoft::WRL::ComPtr<ID3D12CommandQueue> ObservePresent(IDXGISwapChain*);
void BeforeResize(IDXGISwapChain*);
std::string Status();
void Report(const std::string&);
// Periodic render statistics are written by the existing data worker so file
// I/O cannot stall Present. Only the most recent unflushed sample is retained.
void QueueDiagnostics(std::string);
void FlushDiagnostics();
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
void AllowHiddenValidation(bool);
#endif
}

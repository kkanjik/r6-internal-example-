#include "dx12_hooks.h"
#include "emulation.h"
#include "copied_emulation.h"
#include "shadow_vtable.h"
#include <dxgi1_6.h>
#include <MinHook.h>
#include <Zydis/Zydis.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cwctype>
using Microsoft::WRL::ComPtr;
namespace dx12_hooks { namespace {
using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
using Barrier=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
using Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
std::recursive_mutex installMutex;std::mutex captureMutex;
std::mutex diagnosticsMutex;std::string pendingDiagnostics;
std::atomic<Present> originalPresent{};std::atomic<Resize> originalResize{};
std::atomic<Present1> originalPresent1{};
std::atomic<Execute> originalExecute{};std::atomic<Barrier> originalBarrier{};std::atomic<Reset> originalReset{};
std::vector<void*> targets;bool ownsMinHook=false;std::jthread bootstrap;
std::string status="Automatic DX12 capture not started";
void* presentTarget=nullptr;void* resizeTarget=nullptr;
void* present1Target=nullptr;
ComPtr<IDXGISwapChain3> shadowOwner;
ShadowVtable swapShadow;
struct Queue {ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Device> device;};
std::vector<Queue> queues;std::set<ID3D12Resource*> backbuffers;
std::set<ID3D12CommandList*> touching;ComPtr<ID3D12CommandQueue> matched;
IDXGISwapChain* tracked=nullptr;bool stopping=false;
std::atomic<bool> captureNeeded=true;
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
std::atomic<bool> allowHiddenValidation=false;
#endif
void Message(const std::string& text){
 std::lock_guard lock(captureMutex);status=text;
 HMODULE module=nullptr;wchar_t path[32768]{};
 if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Status),&module)&&GetModuleFileNameW(module,path,32768)){
  std::wstring destination=path,lower=destination;std::transform(lower.begin(),lower.end(),lower.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});
  // Keep diagnostics alongside the delivered DLL, only inside Downloads.
  if(lower.find(L"\\downloads\\")!=std::wstring::npos){
   destination.resize(destination.find_last_of(L'\\')+1);destination+=L"overlay-bootstrap.log";
   HANDLE file=CreateFileW(destination.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
   if(file!=INVALID_HANDLE_VALUE){const auto line=std::to_string(GetTickCount64())+" "+text+"\r\n";DWORD wrote=0;WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&wrote,nullptr);CloseHandle(file);}
  }
 }
}
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Temporary DX12 discovery objects could not be created");}
void HookCheck(MH_STATUS error){if(error!=MH_OK)throw std::runtime_error(MH_StatusToString(error));}
void InitializeHooks(){auto result=MH_Initialize();if(result==MH_OK)ownsMinHook=true;else if(result!=MH_ERROR_ALREADY_INITIALIZED)HookCheck(result);}
void ValidateCode(void* address){
 std::array<unsigned char,15> bytes{};SIZE_T got=0;
 if(!ReadProcessMemory(GetCurrentProcess(),address,bytes.data(),bytes.size(),&got)||!got)throw std::runtime_error("DX12 hook entry unreadable");
 ZydisDisassembledInstruction ins{};
 if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,reinterpret_cast<std::uint64_t>(address),bytes.data(),got,&ins)))throw std::runtime_error("Zydis could not decode DX12 hook entry");
}
template<class T>void Create(void* target,void* detour,std::atomic<T>& slot){
 ValidateCode(target);T trampoline=nullptr;HookCheck(MH_CreateHook(target,detour,reinterpret_cast<void**>(&trampoline)));slot.store(trampoline);targets.push_back(target);HookCheck(MH_QueueEnableHook(target));
}
void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists){
 if(!captureNeeded.load(std::memory_order_relaxed)){if(auto original=originalExecute.load())original(queue,count,lists);return;}
 try{
  if(queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT){
   ComPtr<ID3D12Device> device;
   if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))){
    std::lock_guard lock(captureMutex);
    if(!stopping){
     if(queues.size()<16&&std::none_of(queues.begin(),queues.end(),[&](const auto& q){return q.queue.Get()==queue;}))queues.push_back({queue,device});
     for(UINT i=0;lists&&i<count;++i)if(touching.erase(lists[i])){matched=queue;status="Captured DIRECT queue submitting swap-chain buffer barriers";}
    }
   }
  }
 }catch(...){}
 if(auto original=originalExecute.load())original(queue,count,lists);
}
void STDMETHODCALLTYPE BarrierHook(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers){
 if(!captureNeeded.load(std::memory_order_relaxed)){if(auto original=originalBarrier.load())original(list,count,barriers);return;}
 try{std::lock_guard lock(captureMutex);if(!stopping&&touching.size()<1024)for(UINT i=0;barriers&&i<count;++i)if(barriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION&&backbuffers.contains(barriers[i].Transition.pResource)){touching.insert(list);break;}}catch(...){}
 if(auto original=originalBarrier.load())original(list,count,barriers);
}
HRESULT STDMETHODCALLTYPE ResetHook(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline){
 if(captureNeeded.load(std::memory_order_relaxed)){std::lock_guard lock(captureMutex);touching.erase(list);}
 auto original=originalReset.load();return original?original(list,allocator,pipeline):E_FAIL;
}
LRESULT CALLBACK DiscoveryProc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
struct Discovery {
 HWND window{};HINSTANCE instance{};ATOM atom{};
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
 ComPtr<IDXGISwapChain3> swap;
 Discovery() try {
  instance=GetModuleHandleW(nullptr);WNDCLASSW cls{};cls.lpfnWndProc=DiscoveryProc;cls.hInstance=instance;cls.lpszClassName=L"OfflineDx12HookDiscovery";
  atom=RegisterClassW(&cls);if(!atom)throw std::runtime_error("DX12 discovery window class registration failed");
  window=CreateWindowExW(0,cls.lpszClassName,L"DX12 discovery",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,instance,nullptr);
  if(!window)throw std::runtime_error("DX12 discovery window creation failed");
  Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
  Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));Check(list->Close());
  ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=64;desc.Height=64;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> first;Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&first));Check(first.As(&swap));
 }catch(...){if(window)DestroyWindow(window);if(atom)UnregisterClassW(L"OfflineDx12HookDiscovery",instance);throw;}
 ~Discovery(){swap.Reset();list.Reset();allocator.Reset();queue.Reset();device.Reset();if(window)DestroyWindow(window);if(atom)UnregisterClassW(L"OfflineDx12HookDiscovery",instance);}
};
}
bool Install(IDXGISwapChain3* swap,void* present,void* resize,void* present1){
 std::lock_guard lock(installMutex);
 try{
  if(swapShadow.ActiveFor(swap))return true;
  if(shadowOwner)throw std::runtime_error("A different swap chain already owns the shadow hooks");
  auto** vtable=*reinterpret_cast<void***>(swap);
  if(presentTarget){
   if(presentTarget!=vtable[8]||resizeTarget!=vtable[13]||present1Target!=vtable[22])throw std::runtime_error("Discovered DXGI implementation differs from active swap chain");
   // The copied DXGI slots point at bootstrap-detoured entry points. Their
   // original calls must use the MinHook trampolines, avoiding recursion.
  }else{
   originalPresent.store(reinterpret_cast<Present>(vtable[8]));
   originalResize.store(reinterpret_cast<Resize>(vtable[13]));
   originalPresent1.store(reinterpret_cast<Present1>(vtable[22]));
  }
  std::size_t entries=40; // IDXGISwapChain3, including all inherited slots.
  ComPtr<IDXGISwapChain4> fourth;
  if(SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&fourth)))&&fourth.Get()==static_cast<void*>(swap))entries=41;
  const std::array<ShadowVtable::Patch,3> patches={{{8,present},{13,resize},{22,present1}}};
  shadowOwner=swap;
  try{swapShadow.Install(swap,entries,patches);}catch(...){shadowOwner.Reset();throw;}
  Message("Instance shadow-vtable Present/Present1/ResizeBuffers hooks installed");return true;
 }catch(const std::exception& error){Message(error.what());return false;}
}
bool StartAutomatic(void* present,void* resize,void* present1){
 std::lock_guard lock(installMutex);if(bootstrap.joinable()||presentTarget||shadowOwner)return true;
 {std::lock_guard capture(captureMutex);stopping=false;captureNeeded=true;status="Preparing automatic DX12 hooks";}
 bootstrap=std::jthread([=](std::stop_token stop){
  try{
   if(stop.stop_requested())return;
   if(!emulation::SelfTest()||!emulation::CopiedMatrixSelfTest())throw std::runtime_error("Unicorn self-check failed");
   Discovery temporary;
   std::lock_guard install(installMutex);if(stop.stop_requested())return;
   InitializeHooks();auto** q=*reinterpret_cast<void***>(temporary.queue.Get());auto** l=*reinterpret_cast<void***>(temporary.list.Get());
   Create(q[10],reinterpret_cast<void*>(&ExecuteHook),originalExecute);
   Create(l[26],reinterpret_cast<void*>(&BarrierHook),originalBarrier);
   Create(l[10],reinterpret_cast<void*>(&ResetHook),originalReset);
   auto** s=*reinterpret_cast<void***>(temporary.swap.Get());
   Create(s[8],present,originalPresent);presentTarget=s[8];
   Create(s[13],resize,originalResize);resizeTarget=s[13];
   Create(s[22],present1,originalPresent1);present1Target=s[22];
   HookCheck(MH_ApplyQueued());
   Message("Hooks installed; waiting for a DX12 Present and DIRECT queue");
  }catch(const std::exception& error){Message(std::string("DX12 bootstrap: ")+error.what());}
 });return true;
}
ComPtr<ID3D12CommandQueue> ObservePresent(IDXGISwapChain* swap){
 DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(swap->GetDesc(&desc))||!IsWindow(desc.OutputWindow))return {};
 DWORD process=0;GetWindowThreadProcessId(desc.OutputWindow,&process);
 bool visible=IsWindowVisible(desc.OutputWindow)||GetForegroundWindow()==desc.OutputWindow;
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
 visible|=allowHiddenValidation.load();
#endif
 if(process!=GetCurrentProcessId()||!visible)return {};
 ComPtr<ID3D12Device> device;if(FAILED(swap->GetDevice(IID_PPV_ARGS(&device))))return {};
 std::lock_guard lock(captureMutex);if(stopping)return {};
 if(tracked!=swap){tracked=swap;matched.Reset();backbuffers.clear();touching.clear();}
 if(backbuffers.empty())for(UINT i=0;i<desc.BufferCount&&i<16;++i){ComPtr<ID3D12Resource> buffer;if(SUCCEEDED(swap->GetBuffer(i,IID_PPV_ARGS(&buffer))))backbuffers.insert(buffer.Get());}
 if(matched){ComPtr<ID3D12Device> qdev;if(SUCCEEDED(matched->GetDevice(IID_PPV_ARGS(&qdev)))&&qdev.Get()==device.Get()){captureNeeded=false;return matched;}}
 unsigned matches=0;
 for(const auto& candidate:queues)if(candidate.device.Get()==device.Get()){++matches;}
 status=matches?"Waiting for a DIRECT queue confirmed by the presenting back buffers":"Waiting for the game's DIRECT command queue";return {};
}
void BeforeResize(IDXGISwapChain* swap){std::lock_guard lock(captureMutex);if(tracked==swap){backbuffers.clear();touching.clear();}}
Present OriginalPresent(){return originalPresent.load();}
Present1 OriginalPresent1(){return originalPresent1.load();}
Resize OriginalResize(){return originalResize.load();}
bool HasShadow(IDXGISwapChain3* swap){std::lock_guard lock(installMutex);return swapShadow.ActiveFor(swap);}
std::string Status(){std::lock_guard lock(captureMutex);return status;}
void Report(const std::string& text){Message(text);}
void QueueDiagnostics(std::string text){std::lock_guard lock(diagnosticsMutex);pendingDiagnostics=std::move(text);}
void FlushDiagnostics(){std::string text;{std::lock_guard lock(diagnosticsMutex);text.swap(pendingDiagnostics);}if(!text.empty())Message(text);}
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
void AllowHiddenValidation(bool enabled){allowHiddenValidation=enabled;}
#endif
void Shutdown(){
 // Never join while holding installMutex: the discovery worker also takes it.
 if(bootstrap.joinable()){bootstrap.request_stop();bootstrap.join();}
 std::lock_guard install(installMutex);
 if(!swapShadow.Restore()){Message("Shutdown refused: another owner replaced the shadow vfptr; do not unload");return;}
 shadowOwner.Reset();
 {std::lock_guard capture(captureMutex);stopping=true;}
 for(auto target:targets)MH_QueueDisableHook(target);if(!targets.empty())MH_ApplyQueued();
 for(auto target:targets)MH_RemoveHook(target);targets.clear();presentTarget=resizeTarget=present1Target=nullptr;
 if(ownsMinHook){MH_Uninitialize();ownsMinHook=false;}
 std::lock_guard capture(captureMutex);queues.clear();matched.Reset();touching.clear();backbuffers.clear();tracked=nullptr;status="DX12 hooks stopped";
}
}

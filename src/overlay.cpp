#include "overlay.h"
#include "game_data.h"
#include "projection.h"
#include "esp.h"
#include "dx12_hooks.h"
#include <wrl/client.h>
#include <Zydis/Zydis.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <array>
#include <algorithm>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cwchar>
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace {
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
struct Frame { ComPtr<ID3D12Resource> buffer; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}; };
struct FrameContext {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;UINT64 fence=0;};
struct State {
 HWND window{}; DWORD thread{}; WNDPROC oldProc{};
 ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
 ComPtr<IDXGISwapChain3> swap; ComPtr<ID3D12DescriptorHeap> rtvs,srvs;
 ComPtr<ID3D12Fence> fence;
 HANDLE event{}; UINT64 sequence=0, presents=0, present1s=0, submissions=0, draws=0, lastDiagnostic=0; UINT lastBuffer=0; std::vector<Frame> frames;std::vector<FrameContext> contexts;
 std::array<bool,64> descriptors{};
 ImGuiContext* context{}; bool win32=false,dx12=false,ready=false,clear=false,visible=true;
 std::string error,decoded;
 std::chrono::steady_clock::time_point lastRender{};
} g;
std::recursive_mutex renderMutex;
std::atomic<bool> automatic=false;
thread_local bool insidePresent=false;
std::uint64_t nextAutomaticAttempt=0;
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
bool validationMarker=false,validationPixelGood=false;
game::Vec3 validationProbe{};bool validationProbeGood=false;
#endif
void Check(HRESULT hr) { if(FAILED(hr)){char b[64];std::snprintf(b,sizeof(b),"DX12 HRESULT 0x%08lX",static_cast<unsigned long>(hr));throw std::runtime_error(b);} }
struct ContextScope {
 std::unique_lock<std::recursive_mutex> lock;
 ImGuiContext* previous;
 ContextScope():lock(renderMutex),previous(ImGui::GetCurrentContext()){ImGui::SetCurrentContext(g.context);}
 ~ContextScope(){ImGui::SetCurrentContext(previous);}
};
void Wait(UINT64 value) {
 if(!value)return;
 auto done=g.fence->GetCompletedValue();
 if(done==UINT64_MAX)throw std::runtime_error("Device removed");
 if(done<value){Check(g.fence->SetEventOnCompletion(value,g.event));
  if(WaitForSingleObject(g.event,2000)!=WAIT_OBJECT_0)throw std::runtime_error("Fence wait failed");
  if(g.fence->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Device removed during wait");}
}
void Flush(){ if(!g.queue||!g.fence)return;Check(g.queue->Signal(g.fence.Get(),++g.sequence));Wait(g.sequence); }
void BuildFrames(){
 DXGI_SWAP_CHAIN_DESC desc{};Check(g.swap->GetDesc(&desc));
 if(desc.BufferCount<2||desc.BufferCount>16||desc.SampleDesc.Count!=1)throw std::runtime_error("Requires a non-MSAA flip swap chain with at least two buffers");
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=desc.BufferCount;
 Check(g.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&g.rtvs)));
 g.frames.resize(desc.BufferCount);g.contexts.resize(desc.BufferCount);g.submissions=0;
 auto cpu=g.rtvs->GetCPUDescriptorHandleForHeapStart();auto stride=g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 for(UINT i=0;i<desc.BufferCount;++i){auto& f=g.frames[i];Check(g.swap->GetBuffer(i,IID_PPV_ARGS(&f.buffer)));
  auto& context=g.contexts[i];Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&context.allocator)));
  Check(g.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,context.allocator.Get(),nullptr,IID_PPV_ARGS(&context.list)));Check(context.list->Close());
  f.rtv=cpu;g.device->CreateRenderTargetView(f.buffer.Get(),nullptr,cpu);cpu.ptr+=stride;}
}
void InitRenderer(){
 DXGI_SWAP_CHAIN_DESC d{};Check(g.swap->GetDesc(&d));
 ImGui_ImplDX12_InitInfo info{};info.Device=g.device.Get();info.CommandQueue=g.queue.Get();
 info.NumFramesInFlight=static_cast<int>(g.frames.size());info.RTVFormat=d.BufferDesc.Format;
 info.SrvDescriptorHeap=g.srvs.Get();
 info.SrvDescriptorAllocFn=[](ImGui_ImplDX12_InitInfo*,D3D12_CPU_DESCRIPTOR_HANDLE* cpu,D3D12_GPU_DESCRIPTOR_HANDLE* gpu){
  for(size_t i=0;i<g.descriptors.size();++i)if(!g.descriptors[i]){g.descriptors[i]=true;
   const auto stride=g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   *cpu=g.srvs->GetCPUDescriptorHandleForHeapStart();cpu->ptr+=i*stride;
   *gpu=g.srvs->GetGPUDescriptorHandleForHeapStart();gpu->ptr+=i*stride;return;}
  throw std::runtime_error("Overlay SRV heap exhausted");};
 info.SrvDescriptorFreeFn=[](ImGui_ImplDX12_InitInfo*,D3D12_CPU_DESCRIPTOR_HANDLE cpu,D3D12_GPU_DESCRIPTOR_HANDLE){
  auto base=g.srvs->GetCPUDescriptorHandleForHeapStart().ptr;
  auto stride=g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto index=(cpu.ptr-base)/stride;if(index<g.descriptors.size())g.descriptors[index]=false;};
 if(!ImGui_ImplDX12_Init(&info))throw std::runtime_error("ImGui DX12 initialization failed");g.dx12=true;
}
LRESULT CALLBACK WindowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
 WNDPROC original=nullptr;
 {
  ContextScope context;original=g.oldProc;
  if(msg==WM_KEYUP&&wp==VK_INSERT){g.visible=!g.visible;return 0;}
  if(g.context&&g.visible){
   if(ImGui_ImplWin32_WndProcHandler(h,msg,wp,lp))return 1;
   auto& io=ImGui::GetIO();
   if(io.WantCaptureMouse&&(msg>=WM_MOUSEFIRST&&msg<=WM_MOUSELAST))return 1;
   if(io.WantCaptureKeyboard&&((msg>=WM_KEYFIRST&&msg<=WM_KEYLAST)||msg==WM_CHAR))return 1;
  }
 }
 return original?CallWindowProcW(original,h,msg,wp,lp):DefWindowProcW(h,msg,wp,lp);
}
void Render(){
 // ImGui and frame contexts are serialized even if the host changes its Present thread.
 g.thread=GetCurrentThreadId();
 ContextScope context;
 auto index=g.swap->GetCurrentBackBufferIndex();if(index>=g.frames.size())throw std::runtime_error("Unexpected back buffer index");
 auto& f=g.frames[index];auto& gpu=g.contexts[g.submissions%g.contexts.size()];Wait(gpu.fence);
 ImGui_ImplDX12_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
 esp::Draw(g.visible);
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
 if(validationMarker)ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(8,8),ImVec2(24,24),IM_COL32(255,0,255,255));
#endif
 ImGui::Render();Check(gpu.allocator->Reset());Check(gpu.list->Reset(gpu.allocator.Get(),nullptr));
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
 b.Transition.pResource=f.buffer.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
 b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
 gpu.list->ResourceBarrier(1,&b);gpu.list->OMSetRenderTargets(1,&f.rtv,FALSE,nullptr);
 if(g.clear){const float color[]={0.045f,0.055f,0.08f,1};gpu.list->ClearRenderTargetView(f.rtv,color,0,nullptr);}
 ID3D12DescriptorHeap* heaps[]={g.srvs.Get()};gpu.list->SetDescriptorHeaps(1,heaps);
 ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),gpu.list.Get());
 std::swap(b.Transition.StateBefore,b.Transition.StateAfter);gpu.list->ResourceBarrier(1,&b);Check(gpu.list->Close());
 ID3D12CommandList* lists[]={gpu.list.Get()};g.queue->ExecuteCommandLists(1,lists);
 Check(g.queue->Signal(g.fence.Get(),++g.sequence));gpu.fence=g.sequence;++g.submissions;++g.draws;g.lastBuffer=index;
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
 if(validationMarker){
  // Read actual rendered pixels before DXGI can discard the presented buffer.
  // This deliberately stalls only the test host, never the production DLL.
  Wait(gpu.fence);
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256*32;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;Check(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
  Check(gpu.allocator->Reset());Check(gpu.list->Reset(gpu.allocator.Get(),nullptr));
  b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;gpu.list->ResourceBarrier(1,&b);
  D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=f.buffer.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,16,16,1,256};
  const D3D12_BOX box{8,8,0,24,24,1};gpu.list->CopyTextureRegion(&to,0,0,0,&from,&box);
  const auto frame=game::LatestShared();game::ScreenPoint probe{};const auto display=ImGui::GetIO().DisplaySize;
  if(!game::Project(validationProbe,frame->viewProjection,display.x,display.y,probe)||probe.x<8||probe.y<8)throw std::runtime_error("Validation probe outside viewport");
  const auto probeX=static_cast<UINT>(probe.x),probeY=static_cast<UINT>(probe.y);
  to.PlacedFootprint.Offset=256*16;const D3D12_BOX probeBox{probeX-8,probeY-8,0,probeX+8,probeY+8,1};gpu.list->CopyTextureRegion(&to,0,0,0,&from,&probeBox);
  std::swap(b.Transition.StateBefore,b.Transition.StateAfter);gpu.list->ResourceBarrier(1,&b);Check(gpu.list->Close());g.queue->ExecuteCommandLists(1,lists);
  Check(g.queue->Signal(g.fence.Get(),++g.sequence));gpu.fence=g.sequence;Wait(gpu.fence);
  void* bytes=nullptr;const D3D12_RANGE range{0,256*32};Check(readback->Map(0,&range,&bytes));
  const auto* pixel=static_cast<unsigned char*>(bytes)+8*256+8*4;
  validationPixelGood=pixel[0]>240&&pixel[1]<16&&pixel[2]>240&&pixel[3]>240;
  validationProbeGood=false;
  for(unsigned y=7;y<=9;++y)for(unsigned x=7;x<=9;++x){const auto* p=static_cast<unsigned char*>(bytes)+256*16+y*256+x*4;if(p[0]<160&&p[1]>160&&p[2]>160)validationProbeGood=true;}
  const D3D12_RANGE written{0,0};readback->Unmap(0,&written);
  if(!validationPixelGood)throw std::runtime_error("Overlay marker pixels missing from rendered back buffer");
 }
#endif
}
HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain*,UINT,UINT);
HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
struct PresentScope {bool previous=insidePresent;PresentScope(){insidePresent=true;}~PresentScope(){insidePresent=previous;}};
bool BeforePresent(IDXGISwapChain* swap,UINT flags,bool nested,bool modern){
 bool drawn=false;
 {
  std::lock_guard lock(renderMutex);
  if(automatic.load()&&!g.thread&&!nested&&!(flags&DXGI_PRESENT_TEST)&&GetTickCount64()>=nextAutomaticAttempt){
   try{
    auto queue=dx12_hooks::ObservePresent(swap);
    if(queue){
     ComPtr<ID3D12Device> device;ComPtr<IDXGISwapChain3> chain;DXGI_SWAP_CHAIN_DESC desc{};
     Check(swap->GetDevice(IID_PPV_ARGS(&device)));Check(swap->QueryInterface(IID_PPV_ARGS(&chain)));Check(swap->GetDesc(&desc));
     if(!OverlayInitialize(desc.OutputWindow,device.Get(),queue.Get(),chain.Get(),false))nextAutomaticAttempt=GetTickCount64()+2000;
    }
   }catch(const std::exception& error){g.error=error.what();}
  }
  if(swap==static_cast<IDXGISwapChain*>(g.swap.Get())&&g.ready&&!nested&&!(flags&DXGI_PRESENT_TEST)){
   ++g.presents;
   if(modern)++g.present1s;
   // Flip-model buffers are redrawn by the host. Skipping an overlay submission
   // produces a visible overlay-free frame, not a persistent capped overlay.
   try{Render();drawn=true;}catch(const std::exception& e){g.error=e.what();g.ready=false;dx12_hooks::Report("Rendering stopped: "+g.error);}
   const auto now=GetTickCount64();
   if(now-g.lastDiagnostic>=2000){
    g.lastDiagnostic=now;auto frame=game::LatestShared();
    const auto poses=std::count_if(frame->characters.begin(),frame->characters.end(),[](const auto& actor){return actor.skeleton.worldValid;});
    std::string text="build=pose-r5 pid="+std::to_string(GetCurrentProcessId())+" presents="+std::to_string(g.presents)+" present1="+std::to_string(g.present1s)+" draws="+std::to_string(g.draws)+" buffer="+std::to_string(g.lastBuffer)+" actors="+std::to_string(frame->characters.size())+" poses="+std::to_string(poses)+" data="+frame->status;
    for(const auto& actor:frame->characters)if(!actor.skeleton.worldValid){text+=" first_pose_error="+actor.skeleton.status;break;}
    const auto metrics=esp::LastDrawMetrics();text+=" segments="+std::to_string(metrics.segments)+" boxes="+std::to_string(metrics.boxes);
    std::uint64_t maxAge=0,maxCapture=0;
    for(const auto& actor:frame->characters)if(!actor.bones.empty()){maxAge=std::max(maxAge,game::AgeMs(now,actor.skeleton.capturedMs));maxCapture=std::max(maxCapture,actor.skeleton.captureDurationMs);}
    text+=" jobs="+std::to_string(frame->scheduledPoses)+" workers="+std::to_string(frame->poseWorkers)+" max_pose_age_ms="+std::to_string(maxAge)+" max_capture_ms="+std::to_string(maxCapture);
    dx12_hooks::QueueDiagnostics(std::move(text));
   }
  }
 }
 return drawn;
}
HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swap,UINT sync,UINT flags){
 auto original=dx12_hooks::OriginalPresent();PresentScope scope;
 BeforePresent(swap,flags,scope.previous,false);
 return original?original(swap,sync,flags):DXGI_ERROR_INVALID_CALL;
}
HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* swap,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters){
 auto original=dx12_hooks::OriginalPresent1();PresentScope scope;
 const bool drawn=BeforePresent(swap,flags,scope.previous,true);
 // The overlay changes pixels outside the host's dirty rectangles. Present
 // the whole surface whenever we added draw commands; preserve all parameters
 // verbatim for test/nested/non-owned presentations.
 const DXGI_PRESENT_PARAMETERS fullSurface{};
 return original?original(swap,sync,flags,drawn?&fullSurface:parameters):DXGI_ERROR_INVALID_CALL;
}
HRESULT STDMETHODCALLTYPE ResizeHook(IDXGISwapChain* swap,UINT count,UINT width,UINT height,DXGI_FORMAT format,UINT flags){
 dx12_hooks::BeforeResize(swap);auto original=dx12_hooks::OriginalResize();if(!original)return DXGI_ERROR_INVALID_CALL;
 {
  ContextScope context;
  if(swap!=static_cast<IDXGISwapChain*>(g.swap.Get()))return original(swap,count,width,height,format,flags);
  g.ready=false;
  try{Flush();if(g.dx12){ImGui_ImplDX12_Shutdown();g.dx12=false;}g.frames.clear();g.contexts.clear();g.rtvs.Reset();}
  catch(const std::exception& e){g.error=e.what();return DXGI_ERROR_DEVICE_REMOVED;}
 }
 // DXGI may synchronously send window messages. WndProc must be free to take
 // renderMutex while the host's ResizeBuffers implementation is executing.
 const HRESULT hr=original(swap,count,width,height,format,flags);
 {
  ContextScope context;
  try{BuildFrames();InitRenderer();g.ready=true;}catch(const std::exception& e){g.error=e.what();}
 }
 return hr;
}

std::string Decode(void* address){
 MEMORY_BASIC_INFORMATION mbi{};if(!VirtualQuery(address,&mbi,sizeof(mbi)))throw std::runtime_error("Cannot query Present code");
 auto remaining=static_cast<size_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize-reinterpret_cast<uintptr_t>(address));
 ZydisDisassembledInstruction instruction{};
 if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,reinterpret_cast<uint64_t>(address),address,(std::min)(remaining,size_t(15)),&instruction)))throw std::runtime_error("Zydis could not decode Present");
 return instruction.text;
}
}
extern "C" const char* OverlayLastError(){std::lock_guard lock(renderMutex);thread_local std::string copy;copy=g.error;return copy.c_str();}
extern "C" const char* OverlayBootstrapStatus(){thread_local std::string copy;copy=dx12_hooks::Status();return copy.c_str();}
extern "C" bool OverlayIsReady(){std::lock_guard lock(renderMutex);return g.ready;}
extern "C" bool OverlayStartAutomatic(){automatic=true;return dx12_hooks::StartAutomatic(reinterpret_cast<void*>(&PresentHook),reinterpret_cast<void*>(&ResizeHook),reinterpret_cast<void*>(&Present1Hook));}
extern "C" void OverlayShutdown(){
 std::lock_guard lock(renderMutex);
 if(g.thread&&GetCurrentThreadId()!=g.thread){g.error="Shutdown must run on the initialization thread";return;}
 g.ready=false;
 if(!insidePresent)automatic=false;
 game::Stop();
 // Caller guarantees no concurrent Present/ResizeBuffers/window callbacks.
 // On automatic initialization failure the current Present still needs its
 // trampoline. Keep hooks installed until that callback has returned.
 if(!insidePresent)dx12_hooks::Shutdown();
 if(g.oldProc){SetWindowLongPtrW(g.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(g.oldProc));g.oldProc=nullptr;}
 try{Flush();}catch(const std::exception& e){g.error=e.what();}
 if(g.context){auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(g.context);
  if(g.dx12)ImGui_ImplDX12_Shutdown();if(g.win32)ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(g.context);ImGui::SetCurrentContext(previous==g.context?nullptr:previous);}
 if(g.event)CloseHandle(g.event);
 auto error=g.error;g=State{};g.error=std::move(error);
}
extern "C" bool OverlayInitialize(HWND hwnd,ID3D12Device* device,ID3D12CommandQueue* queue,IDXGISwapChain3* swap,bool clear){
 std::lock_guard lock(renderMutex);
 if(g.thread){g.error="Overlay is already initialized";return false;}g.error.clear();
 if(!IsWindow(hwnd)||!device||!queue||!swap){g.error="Invalid initialization arguments";return false;}
 DWORD process=0;GetWindowThreadProcessId(hwnd,&process);
 if(process!=GetCurrentProcessId()){g.error="Window must belong to this process";return false;}
 auto thread=GetCurrentThreadId();
 if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT){g.error="A DIRECT command queue is required";return false;}
 try{
  ComPtr<ID3D12Device> qdev,sdev;Check(queue->GetDevice(IID_PPV_ARGS(&qdev)));Check(swap->GetDevice(IID_PPV_ARGS(&sdev)));
  if(qdev.Get()!=device||sdev.Get()!=device)throw std::runtime_error("Device/queue/swap-chain mismatch");
  DXGI_SWAP_CHAIN_DESC desc{};Check(swap->GetDesc(&desc));if(desc.OutputWindow!=hwnd)throw std::runtime_error("Window/swap-chain mismatch");
  g.thread=thread;g.window=hwnd;g.device=device;g.queue=queue;g.swap=swap;g.clear=clear;
  BuildFrames();
  Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g.fence)));
  g.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!g.event)throw std::runtime_error("CreateEvent failed");
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=64;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&g.srvs)));
  auto* previous=ImGui::GetCurrentContext();g.context=ImGui::CreateContext();ImGui::SetCurrentContext(previous);
  {ContextScope context;ImGui::StyleColorsDark();ImGui::GetIO().IniFilename=nullptr;
   if(!ImGui_ImplWin32_Init(hwnd))throw std::runtime_error("ImGui Win32 initialization failed");g.win32=true;InitRenderer();}
  auto** table=*reinterpret_cast<void***>(swap);g.decoded=Decode(table[8]);
  if(!dx12_hooks::Install(swap,reinterpret_cast<void*>(&PresentHook),reinterpret_cast<void*>(&ResizeHook),reinterpret_cast<void*>(&Present1Hook)))throw std::runtime_error(dx12_hooks::Status());
  SetLastError(0);auto old=SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&WindowProc));
  if(!old)throw std::runtime_error("Window subclass failed");g.oldProc=reinterpret_cast<WNDPROC>(old);
  game::Start();
  g.ready=true;dx12_hooks::Report("ImGui Win32/DX12 initialized; overlay is rendering. Insert toggles the menu.");return true;
 }catch(const std::exception& e){g.error=e.what();dx12_hooks::Report("ImGui initialization failed: "+g.error);OverlayShutdown();return false;}
}

extern "C" bool OverlaySelfTest(){return game::SelfTest();}
// Retain old exports for binary compatibility; never skip a presented frame.
extern "C" void OverlaySetFpsLimit(unsigned){}
extern "C" unsigned OverlayFpsLimit(){return 0;}
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
extern "C" __declspec(dllexport) unsigned long long OverlayValidationPresentCount(){return g.presents;}
extern "C" __declspec(dllexport) unsigned long long OverlayValidationDrawCount(){return g.draws;}
extern "C" __declspec(dllexport) void OverlayValidationMarker(bool enabled){validationMarker=enabled;validationPixelGood=false;}
extern "C" __declspec(dllexport) bool OverlayValidationPixelGood(){return validationPixelGood;}
extern "C" __declspec(dllexport) void OverlayValidationProbe(float x,float y,float z){validationProbe={x,y,z};}
extern "C" __declspec(dllexport) bool OverlayValidationProbeGood(){return validationProbeGood;}
extern "C" __declspec(dllexport) unsigned OverlayValidationSegmentCount(){return esp::LastDrawMetrics().segments;}
extern "C" __declspec(dllexport) unsigned OverlayValidationBoxCount(){return esp::LastDrawMetrics().boxes;}
#endif

namespace {
DWORD WINAPI BootstrapEntry(void*){
 wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
 const auto* leaf=wcsrchr(executable,L'\\');leaf=leaf?leaf+1:executable;
 if(_wcsicmp(leaf,L"RainbowSix.exe")==0)OverlayStartAutomatic();
 return 0;
}
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
 if(reason==DLL_PROCESS_ATTACH){
  DisableThreadLibraryCalls(module);
  // The thread entry runs after loader initialization. DX12, ImGui, MinHook,
  // Unicorn and the data worker are initialized there or in Present, not here.
  if(HANDLE thread=CreateThread(nullptr,0,BootstrapEntry,nullptr,0,nullptr))CloseHandle(thread);
 }
 return TRUE;
}

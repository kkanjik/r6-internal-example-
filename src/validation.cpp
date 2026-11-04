#include "overlay.h"
#include "game_data.h"
#include "emulation.h"
#include "skeleton_reader.h"
#include "skeleton_profile.h"
#include "dx12_hooks.h"
#include "pose_math.h"
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <span>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>
#include <exception>
#include <d3d12sdklayers.h>
#include <map>
#include <chrono>
#include <cstddef>
#include <mutex>
using Microsoft::WRL::ComPtr;
extern "C" unsigned long long OverlayValidationPresentCount();
extern "C" unsigned long long OverlayValidationDrawCount();
extern "C" void OverlayValidationMarker(bool);
extern "C" bool OverlayValidationPixelGood();
extern "C" unsigned OverlayValidationSegmentCount();
extern "C" unsigned OverlayValidationBoxCount();
extern "C" void OverlayValidationProbe(float,float,float);
extern "C" bool OverlayValidationProbeGood();
namespace {
LRESULT CALLBACK ValidationProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(hwnd,message,w,l);}
void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Validation DX12 call failed");}
std::string validationMessage;
bool validationBodyOnly=false;
}
extern "C" __declspec(dllexport) const char* OverlayValidationMessage(){return validationMessage.c_str();}
extern "C" __declspec(dllexport) void OverlayValidationBodyOnly(bool enabled){validationBodyOnly=enabled;}
bool RunGraphicsValidation(bool automatic){
 HWND window=nullptr;bool initialized=false;
 try{
  if(!OverlaySelfTest())throw std::runtime_error("Zydis/Unicorn/skeleton/projection CPU checks failed");
  ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
  auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=ValidationProc;wc.hInstance=instance;wc.lpszClassName=L"OfflineOverlayValidation";
  if(!RegisterClassW(&wc))throw std::runtime_error("Validation window class failed");
  window=CreateWindowW(wc.lpszClassName,L"Overlay validation",WS_OVERLAPPEDWINDOW,0,0,960,640,nullptr,nullptr,instance,nullptr);if(!window)throw std::runtime_error("Validation window failed");
  ComPtr<IDXGIFactory4> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<ID3D12Device> device;
  if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))){ComPtr<IDXGIAdapter> warp;Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));}
  ComPtr<ID3D12InfoQueue> info;device.As(&info);
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> queue;Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
  DXGI_SWAP_CHAIN_DESC1 desc{};desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=3;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> first;Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&first));ComPtr<IDXGISwapChain3> swap;Check(first.As(&swap));
  auto** originalTable=*reinterpret_cast<void***>(swap.Get());
  if(info)info->ClearStoredMessages();
  game::Snapshot synthetic;synthetic.ready=true;synthetic.status="Synthetic renderer validation";synthetic.cameraPosition={0,-5,1.6f};synthetic.viewProjection={1,0,0,0,0,0,1,1,0,1,0,0,0,-1.6f,4.99f,5};synthetic.characters={{1,{-2,0,0}},{2,{3,3,0}}};
  const std::array<game::Vec3,23> body={{{0,0,.9f},{0,0,.98f},{0,0,1.06f},{0,0,1.14f},{0,0,1.4f},{0,0,1.5f},{0,0,1.65f},{.08f,0,1.4f},{.25f,0,1.4f},{.45f,0,1.15f},{.5f,-.1f,1.05f},{-.08f,0,1.4f},{-.25f,0,1.4f},{-.45f,0,1.15f},{-.5f,-.1f,1.05f},{.1f,0,.85f},{.1f,0,.45f},{.1f,0,.05f},{.1f,.12f,0},{-.1f,0,.85f},{-.1f,0,.45f},{-.1f,0,.05f},{-.1f,.12f,0}}};
  for(auto& actor:synthetic.characters)for(std::size_t i=0;i<body.size();++i){const auto p=actor.position;const auto b=body[i];actor.bones.push_back({{p.x+b.x,p.y+b.y,p.z+b.z},game::pose::Body[i].displayParent});}
  game::SetValidationSnapshot(&synthetic);
  std::atomic<bool> done=false;std::exception_ptr renderFailure;
  std::thread renderThread([&]{
   try{
    ComPtr<ID3D12CommandQueue> noise;ComPtr<ID3D12CommandAllocator> allocator,noiseAllocator;ComPtr<ID3D12GraphicsCommandList> list,noiseList;ComPtr<ID3D12Fence> hostFence;
    struct Event{HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(handle)CloseHandle(handle);}} event;
    UINT64 serial=0;
    if(automatic){
     Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&noise)));
     Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
     Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&noiseAllocator)));
     Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));Check(list->Close());
     Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,noiseAllocator.Get(),nullptr,IID_PPV_ARGS(&noiseList)));Check(noiseList->Close());
     Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&hostFence)));
     if(!event.handle)throw std::runtime_error("Validation event failed");
    }
    auto submit=[&]{if(!automatic)return;
     Check(allocator->Reset());Check(list->Reset(allocator.Get(),nullptr));ComPtr<ID3D12Resource> buffer;Check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)));
     D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=buffer.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;list->ResourceBarrier(1,&b);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);Check(list->Close());
     ID3D12CommandList* empty[]={noiseList.Get()};noise->ExecuteCommandLists(1,empty);
     Check(noise->Signal(hostFence.Get(),++serial));Check(hostFence->SetEventOnCompletion(serial,event.handle));if(WaitForSingleObject(event.handle,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Validation secondary queue timed out");
     ID3D12CommandList* work[]={list.Get()};queue->ExecuteCommandLists(1,work);
     Check(queue->Signal(hostFence.Get(),++serial));Check(hostFence->SetEventOnCompletion(serial,event.handle));if(WaitForSingleObject(event.handle,5000)!=WAIT_OBJECT_0)throw std::runtime_error("Validation host fence timed out");
    };
    if(automatic){
     dx12_hooks::AllowHiddenValidation(true);if(!OverlayStartAutomatic())throw std::runtime_error(OverlayBootstrapStatus());initialized=true;
     const auto deadline=GetTickCount64()+10000;
     const DXGI_PRESENT_PARAMETERS full{};
     while(!OverlayIsReady()&&GetTickCount64()<deadline){submit();Check(swap->Present1(0,0,&full));Sleep(5);}
     if(!OverlayIsReady())throw std::runtime_error(std::string("Automatic initialization failed: ")+OverlayBootstrapStatus()+" / "+OverlayLastError());
    }else{if(!OverlayInitialize(window,device.Get(),queue.Get(),swap.Get(),true))throw std::runtime_error(OverlayLastError());initialized=true;}
    const auto before=OverlayValidationPresentCount();
    const auto drawsBefore=OverlayValidationDrawCount();
    if(!dx12_hooks::HasShadow(swap.Get())||*reinterpret_cast<void***>(swap.Get())==originalTable)throw std::runtime_error("Instance shadow-vtable hook not installed");
    auto** shadowTable=*reinterpret_cast<void***>(swap.Get());
    for(unsigned slot=0;slot<40;++slot)if(slot!=8&&slot!=13&&slot!=22&&shadowTable[slot]!=originalTable[slot])throw std::runtime_error("Unrelated COM slot was modified");
    OverlaySetFpsLimit(1); // Legacy callers must not make the overlay disappear.
    OverlayValidationMarker(true);
    std::array<float,2> lastPoseX={-2.f,3.f};
    for(unsigned frame=0;frame<60;++frame){
     // Exercise publication/movement, not only a static pose cached forever.
     for(auto& actor:synthetic.characters){
      const float x=(actor.id==1?-2.f:3.f)+static_cast<float>(frame)*.01f;
      actor.position.x=x;
      // Keep pose timestamps/joints unchanged for 15 frames while the fast
      // position advances. Pixels must still follow the current actor anchor.
      if(frame%15==0){const auto index=static_cast<std::size_t>(actor.id-1);const float dx=x-lastPoseX[index];lastPoseX[index]=x;actor.skeleton.capturedMs=frame/15+1;for(auto& bone:actor.bones)bone.position.x+=dx;}
     }
     game::SetValidationSnapshot(&synthetic);
     const auto& probe=synthetic.characters[1].position;OverlayValidationProbe(probe.x,probe.y,probe.z+.9f);
     if(frame==20)Check(swap->ResizeBuffers(0,1024,640,DXGI_FORMAT_UNKNOWN,0));
     if(frame==30||frame==40)PostMessageW(window,WM_KEYUP,VK_INSERT,0);
     submit();
     const auto priorDraws=OverlayValidationDrawCount();
     Check(swap->Present(0,DXGI_PRESENT_TEST));
     if(OverlayValidationDrawCount()!=priorDraws)throw std::runtime_error("Test-only Present rendered an overlay");
     if(frame%2){RECT dirty{100,100,110,110};DXGI_PRESENT_PARAMETERS partial{1,&dirty,nullptr,nullptr};Check(swap->Present1(0,0,&partial));}
     else Check(swap->Present(0,0));
     if(*OverlayLastError())throw std::runtime_error(OverlayLastError());
     if(!OverlayValidationPixelGood())throw std::runtime_error("Overlay marker readback failed");Sleep(1);
     if(!OverlayValidationProbeGood())throw std::runtime_error("Skeleton pixels lagged the fast character position while pose timestamp was unchanged");
     if(OverlayValidationSegmentCount()!=44||OverlayValidationBoxCount()!=2)throw std::runtime_error("Synthetic skeletons or anchored boxes were not drawn completely");
    }
    if(OverlayValidationPresentCount()-before!=60)throw std::runtime_error("Present hook callback count mismatch");
    if(OverlayValidationDrawCount()-drawsBefore!=60)throw std::runtime_error("Not every Present submitted an overlay frame");
    OverlayValidationMarker(false);game::SetValidationSnapshot(nullptr);OverlayShutdown();initialized=false;
    if(*reinterpret_cast<void***>(swap.Get())!=originalTable)throw std::runtime_error("Original COM vtable was not restored");
   }catch(...){if(initialized){OverlayShutdown();initialized=false;}renderFailure=std::current_exception();}
   done=true;
  });
  while(!done){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(1);}
  renderThread.join();if(renderFailure)std::rethrow_exception(renderFailure);
  if(info){for(UINT64 index=0;index<info->GetNumStoredMessages();++index){SIZE_T size=0;info->GetMessage(index,nullptr,&size);std::vector<unsigned char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(info->GetMessage(index,message,&size))&&(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION))throw std::runtime_error(message->pDescription);}}
  DestroyWindow(window);window=nullptr;UnregisterClassW(wc.lpszClassName,instance);dx12_hooks::AllowHiddenValidation(false);validationMessage=automatic?"Automatic Present1 bootstrap with two queues; 60 alternating Present/Present1 frames passed GPU marker readback, 44 moving skeleton segments, two anchored boxes, resize, input and shutdown with no DX12 debug errors.":"Explicit shadow hooks: 60 alternating Present/Present1 frames passed GPU marker readback, 44 moving skeleton segments, two anchored boxes, resize, input and shutdown with no DX12 debug errors.";return true;
 }catch(const std::exception& e){game::SetValidationSnapshot(nullptr);if(initialized)OverlayShutdown();if(window)DestroyWindow(window);UnregisterClassW(L"OfflineOverlayValidation",GetModuleHandleW(nullptr));validationMessage=e.what();return false;}
}
extern "C" __declspec(dllexport) bool OverlayRunValidation(){return RunGraphicsValidation(false);}
extern "C" __declspec(dllexport) bool OverlayRunAutomaticValidation(){return RunGraphicsValidation(true);}

// Test export consumes copies supplied by Python. It never reads another process.
extern "C" __declspec(dllexport) bool OverlayValidateWorldSnapshots(
 unsigned long long leaAddress,const unsigned char* lea,unsigned leaSize,
 unsigned long long loadAddress,const unsigned char* load,unsigned loadSize,
 unsigned long long world,unsigned long long state,unsigned long long table){
 try{
  if(!lea||!load||leaSize!=7||loadSize!=7)return false;
  auto result=emulation::Run(leaAddress,std::span(lea,leaSize),{},{});
  if(!result.ok||result.registers[emulation::RCX]!=table)return false;
  std::array<std::uint64_t,16> regs{};regs[emulation::RBX]=world;
  emulation::Region region{world+0x198,std::vector<std::uint8_t>(8),false};std::memcpy(region.bytes.data(),&state,8);
  result=emulation::Run(loadAddress,std::span(load,loadSize),regs,std::span(&region,1));
  return result.ok&&result.registers[emulation::R14]==state;
 }catch(...){return false;}
}

// The validation DLL runs in Python's process. Its only memory source is this
// read-only callback; the DLL neither opens nor injects into the game process.
using SnapshotReadCallback=bool(__cdecl*)(unsigned long long,void*,std::size_t);
extern "C" __declspec(dllexport) const char* OverlayValidationLayout(){
 static std::string text=[](){std::ostringstream s;s<<"{\"snapshot_world\":"<<offsetof(game::Snapshot,world)<<",\"snapshot_characters\":"<<offsetof(game::Snapshot,characters)<<",\"character_stride_r3\":"<<sizeof(game::Character)-8<<",\"character_bones\":"<<offsetof(game::Character,bones)<<",\"character_skeleton\":"<<offsetof(game::Character,skeleton)<<",\"skeleton_captured\":"<<offsetof(game::LocalSkeleton,capturedMs)<<"}";return s.str();}();return text.c_str();
}
extern "C" __declspec(dllexport) bool OverlayBenchmarkFrozenPose(
 unsigned long long base,unsigned long long character,unsigned long long teb,
 unsigned long long codeStart,unsigned long long codeSize,SnapshotReadCallback callback,char* output,unsigned outputSize){
 try{
  if(!callback||!output||outputSize<64)return false;
  std::map<std::pair<std::uint64_t,std::size_t>,std::vector<std::uint8_t>> copies;
  emulation::SnapshotRead source=[&](std::uint64_t at,void* out,std::size_t size){auto key=std::pair(at,size);auto found=copies.find(key);if(found==copies.end()){std::vector<std::uint8_t> bytes(size);if(!callback(at,bytes.data(),size))return false;found=copies.emplace(key,std::move(bytes)).first;}std::memcpy(out,found->second.data(),size);return true;};
  const emulation::CodeRange range{codeStart,codeSize};game::rig::Reader reader(base,base+game::rig::profile::MatrixAccessor,source,std::span(&range,1));
  if(!reader.Capture(character,teb,true,{},true).worldValid)return false;
  const auto started=std::chrono::steady_clock::now();bool valid=true;
  for(unsigned i=0;i<12;++i)valid&=reader.Capture(character,teb,true,{},true).worldValid;
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()/12;
  std::snprintf(output,outputSize,"{\"mean_ms\":%.5f,\"iterations\":12,\"valid\":%s,\"copied_ranges\":%zu}",ms,valid?"true":"false",copies.size());return valid;
 }catch(...){return false;}
}
extern "C" __declspec(dllexport) bool OverlayValidateConcurrentFrozenPose(
 unsigned long long base,unsigned long long character,unsigned long long teb,
 unsigned long long codeStart,unsigned long long codeSize,SnapshotReadCallback callback,char* output,unsigned outputSize){
 try{
  if(!callback||!output||outputSize<128)return false;
  std::mutex sourceMutex;std::map<std::pair<std::uint64_t,std::size_t>,std::vector<std::uint8_t>> copies;
  emulation::SnapshotRead source=[&](std::uint64_t at,void* out,std::size_t size){std::lock_guard lock(sourceMutex);auto key=std::pair(at,size);auto found=copies.find(key);if(found==copies.end()){std::vector<std::uint8_t> bytes(size);if(!callback(at,bytes.data(),size))return false;found=copies.emplace(key,std::move(bytes)).first;}std::memcpy(out,found->second.data(),size);return true;};
  const emulation::CodeRange range{codeStart,codeSize};game::rig::Reader reader(base,base+game::rig::profile::MatrixAccessor,source,std::span(&range,1));
  game::LocalSkeleton reference;
  for(unsigned attempt=0;attempt<4;++attempt){copies.clear();reference=reader.Capture(character,teb,true,{},true);if(reference.worldValid)break;}
  if(!reference.worldValid){std::snprintf(output,outputSize,"{\"warm_valid\":false}");return false;}
  const auto rangesBefore=copies.size();std::array<bool,2> valid={true,true};
  const auto started=std::chrono::steady_clock::now();
  std::array<std::jthread,2> workers;
  for(std::size_t i=0;i<workers.size();++i)workers[i]=std::jthread([&,i]{try{for(unsigned n=0;n<8;++n){const auto value=reader.Capture(character,teb,true,{},true);if(!value.worldValid||value.worldBones.size()!=reference.worldBones.size()){valid[i]=false;continue;}for(std::size_t k=0;k<value.worldBones.size();++k){const auto& a=value.worldBones[k];const auto& b=reference.worldBones[k];if(a.parent!=b.parent||std::abs(a.position.x-b.position.x)>1e-6f||std::abs(a.position.y-b.position.y)>1e-6f||std::abs(a.position.z-b.position.z)>1e-6f)valid[i]=false;}}}catch(...){valid[i]=false;}});
  for(auto& worker:workers)worker.join();
  const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
  const bool ok=valid[0]&&valid[1]&&copies.size()==rangesBefore;
  std::snprintf(output,outputSize,"{\"warm_valid\":true,\"workers\":2,\"captures\":16,\"identical_world_bones\":%s,\"no_new_source_ranges\":%s,\"elapsed_ms\":%.3f}",valid[0]&&valid[1]?"true":"false",copies.size()==rangesBefore?"true":"false",elapsed);return ok;
 }catch(...){return false;}
}
extern "C" __declspec(dllexport) bool OverlayValidateSkeletonSnapshot(
 unsigned long long base,unsigned long long character,unsigned long long teb,
 unsigned long long codeStart,unsigned long long codeSize,SnapshotReadCallback callback,
 bool localPose,char* output,unsigned outputSize){
 if(!callback||!output||outputSize<2)return false;
 try{
  const emulation::CodeRange range{codeStart,codeSize};
  game::rig::Reader reader(base,base+game::rig::profile::MatrixAccessor,
   [=](std::uint64_t address,void* out,std::size_t size){return callback(address,out,size);},std::span(&range,1));
  const auto result=reader.Capture(character,teb,localPose,{},validationBodyOnly);
  std::ostringstream text;text<<"{\"chain_valid\":"<<(result.chainValid?"true":"false")<<",\"pose_valid\":"<<(result.poseValid?"true":"false")<<",\"component\":"<<result.component<<",\"asset\":"<<result.asset<<",\"runtime\":"<<result.runtime<<",\"bone_count\":"<<result.boneCount<<",\"named\":[";
  for(std::size_t i=0;i<result.named.size();++i){if(i)text<<',';const auto& b=result.named[i];text<<"{\"hash\":"<<b.hash<<",\"index\":"<<b.index<<",\"parent\":"<<b.parentIndex<<",\"matrix\":[";for(unsigned k=0;k<16;++k){if(k)text<<',';text<<b.matrix[k];}text<<"]}";}
  text<<"],\"world_valid\":"<<(result.worldValid?"true":"false")<<",\"status\":"<<std::quoted(result.status)<<",\"native_root_position\":["<<result.nativeRootPosition.x<<','<<result.nativeRootPosition.y<<','<<result.nativeRootPosition.z<<"],\"anchor_correction_meters\":"<<result.anchorCorrectionMeters<<",\"root_matrix\":[";
  for(unsigned k=0;k<16;++k){if(k)text<<',';text<<result.rootMatrix[k];}text<<"],\"world_bones\":[";
  for(std::size_t i=0;i<result.worldBones.size();++i){if(i)text<<',';const auto& b=result.worldBones[i];text<<"["<<b.position.x<<','<<b.position.y<<','<<b.position.z<<','<<b.parent<<"]";}
  text<<"]}";const auto encoded=text.str();if(encoded.size()+1>outputSize)return false;
  std::memcpy(output,encoded.c_str(),encoded.size()+1);
  return result.chainValid&&(!localPose||result.poseValid);
 }catch(const std::exception&){std::snprintf(output,outputSize,"{\"chain_valid\":false,\"pose_valid\":false,\"initialization_failed\":true}");return false;}
}

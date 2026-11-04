#include "game_data.h"
#include "emulation.h"
#include "skeleton.h"
#include "projection.h"
#include "world_patterns.h"
#include "skeleton_patterns.h"
#include "skeleton_reader.h"
#include "skeleton_profile.h"
#include "pose_math.h"
#include "dx12_hooks.h"
#include "pose_schedule.h"
#include <Windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <condition_variable>
namespace game { namespace {
using Address=std::uintptr_t;
std::mutex dataMutex;
std::atomic<std::shared_ptr<const Snapshot>> published{std::make_shared<const Snapshot>()};
std::jthread worker;
std::atomic<bool> sampleNearCamera=false;
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
bool validationOverride=false;
Snapshot validationFrame;
#endif
struct Cancelled {};
void CheckStop(std::stop_token token){if(token.stop_requested())throw Cancelled{};}
bool ReadBytes(Address address,void* out,size_t size){
 if(address<0x10000||size>512ull*1024*1024||address+size<address)return false;
 SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),out,size,&got)&&got==size;
}
template<class T> bool Read(Address address,T& value){return ReadBytes(address,&value,sizeof(value));}
Address Pointer(Address address){Address result=0;if(!Read(address,result))throw std::runtime_error("A required pointer became unreadable");return result;}
bool HeapLike(Address value){return value>=0x100000000ull&&value<0x700000000000ull&&(value&7)==0;}
struct Buffer {
 unsigned char* data=nullptr;size_t size=0;
 explicit Buffer(size_t count):size(count){data=static_cast<unsigned char*>(VirtualAlloc(nullptr,count,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!data)throw std::runtime_error("Inspection scratch allocation failed");}
 ~Buffer(){if(data)VirtualFree(data,0,MEM_RELEASE);}
 Buffer(const Buffer&)=delete;Buffer& operator=(const Buffer&)=delete;
};
struct Section {Address address{};DWORD size{},flags{};std::string name;};
struct Image {
 Address base{};DWORD size{},timestamp{};std::vector<Section> sections;std::vector<RUNTIME_FUNCTION> functions;
 Image(){
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);const wchar_t* leaf=wcsrchr(path,L'\\');leaf=leaf?leaf+1:path;
  if(_wcsicmp(leaf,L"RainbowSix.exe")!=0)throw std::runtime_error("No matching offline game module in this process");
  base=reinterpret_cast<Address>(GetModuleHandleW(nullptr));IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
  if(!Read(base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000||!Read(base+static_cast<Address>(dos.e_lfanew),nt)||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)throw std::runtime_error("Unsupported module headers");
  size=nt.OptionalHeader.SizeOfImage;timestamp=nt.FileHeader.TimeDateStamp;
  if(size!=0x19528000||timestamp!=0x3B7DCD86)throw std::runtime_error("Build differs: character offsets require verification");
  auto table=base+static_cast<Address>(dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
  if(nt.FileHeader.NumberOfSections>96)throw std::runtime_error("Invalid section count");
  for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER s{};if(!Read(table+i*sizeof(s),s))throw std::runtime_error("Section table unreadable");char name[9]{};std::memcpy(name,s.Name,8);if(static_cast<std::uint64_t>(s.VirtualAddress)+s.Misc.VirtualSize>size)throw std::runtime_error("Section exceeds image");sections.push_back({base+s.VirtualAddress,s.Misc.VirtualSize,s.Characteristics,name});}
  const auto& ex=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if(ex.Size%sizeof(RUNTIME_FUNCTION)||static_cast<std::uint64_t>(ex.VirtualAddress)+ex.Size>size)throw std::runtime_error("Invalid function directory");
  functions.resize(ex.Size/sizeof(RUNTIME_FUNCTION));if(!ReadBytes(base+ex.VirtualAddress,functions.data(),ex.Size))throw std::runtime_error("Function directory unreadable");
 }
 const Section& Get(const char* name)const{for(const auto& s:sections)if(s.name==name)return s;throw std::runtime_error("Required image section is missing");}
 bool In(Address address,const char* name)const{const auto& s=Get(name);return address>=s.address&&address<s.address+s.size;}
 std::pair<DWORD,DWORD> Function(Address address)const{
  if(address<base||address>=base+size)throw std::runtime_error("Code target outside module");const auto rva=static_cast<DWORD>(address-base);
  auto it=std::upper_bound(functions.begin(),functions.end(),rva,[](DWORD value,const RUNTIME_FUNCTION& f){return value<f.BeginAddress;});
  if(it==functions.begin())throw std::runtime_error("Function boundary unavailable");--it;
  if(rva>=it->EndAddress)throw std::runtime_error("Function boundary unavailable");return {it->BeginAddress,it->EndAddress};
 }
};
using Pattern=std::vector<int>;
Pattern Parse(const char* text){std::istringstream input(text);std::string part;Pattern result;while(input>>part){if(part=="??"||part=="?")result.push_back(-1);else{if(part.size()!=2||!std::isxdigit(static_cast<unsigned char>(part[0]))||!std::isxdigit(static_cast<unsigned char>(part[1])))throw std::runtime_error("Invalid built-in pattern");result.push_back(std::stoi(part,nullptr,16));}}if(result.empty()||std::all_of(result.begin(),result.end(),[](int v){return v<0;}))throw std::runtime_error("Empty pattern");return result;}
bool Match(const unsigned char* source,const Pattern& pattern){for(size_t i=0;i<pattern.size();++i)if(pattern[i]>=0&&source[i]!=pattern[i])return false;return true;}
std::array<Address,8> FindAnchors(const Image& image,std::stop_token token){
 const std::array<Pattern,8> compiled={Parse(patterns::world_table_assignment),Parse(patterns::world_tasks_allocation),Parse(patterns::player_update_registration),Parse(patterns::bone_local_matrix_accessor),Parse(patterns::owner_skeleton_slot),Parse(patterns::runtime_pose_owner_decoder),Parse(patterns::rig_asset_decoder),Parse(patterns::character_root_accessor)};
 std::array<Address,8> found{};std::array<unsigned,8> counts{};
 for(const auto& section:image.sections){
  if(!(section.flags&IMAGE_SCN_MEM_EXECUTE)||!section.size)continue;CheckStop(token);Buffer copy(section.size);
  for(size_t offset=0;offset<section.size;offset+=1024*1024){CheckStop(token);auto n=std::min<size_t>(1024*1024,section.size-offset);if(!ReadBytes(section.address+offset,copy.data+offset,n))throw std::runtime_error("Executable section changed or became unreadable");}
  for(size_t kind=0;kind<compiled.size();++kind){const auto& pattern=compiled[kind];if(section.size<pattern.size())continue;
   for(size_t offset=0;offset<=section.size-pattern.size();++offset){if((offset&0xfffff)==0)CheckStop(token);if(copy.data[offset]!=pattern[0]||!Match(copy.data+offset,pattern))continue;if(++counts[kind]>1)throw std::runtime_error("Signature is ambiguous; drawing disabled");found[kind]=section.address+offset;}
  }
 }
 for(unsigned count:counts)if(count!=1)throw std::runtime_error("A required signature did not match");return found;
}
std::vector<ZydisDisassembledInstruction> Decode(Address address){
 std::array<unsigned char,192> code{};if(!ReadBytes(address,code.data(),code.size()))throw std::runtime_error("Instruction bytes unreadable");std::vector<ZydisDisassembledInstruction> result;
 for(size_t at=0;at<code.size();){ZydisDisassembledInstruction ins{};if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,address+at,code.data()+at,code.size()-at,&ins)))break;if(!ins.info.length)break;result.push_back(ins);at+=ins.info.length;}return result;
}
Address Absolute(const ZydisDisassembledInstruction& ins,const ZydisDecodedOperand& operand){ZyanU64 value=0;if(!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins.info,&operand,ins.runtime_address,&value)))throw std::runtime_error("Operand resolution failed");return static_cast<Address>(value);}
Address Rip(const ZydisDisassembledInstruction& ins){for(unsigned i=0;i<ins.info.operand_count_visible;++i)if(ins.operands[i].type==ZYDIS_OPERAND_TYPE_MEMORY&&ins.operands[i].mem.base==ZYDIS_REGISTER_RIP)return Absolute(ins,ins.operands[i]);throw std::runtime_error("Expected RIP-relative operand");}
struct Layout {Address table{},callback{},stateLoadAddress{},boneAccessor{};std::vector<std::uint8_t> stateLoadCode;size_t stateOffset{},tasksOffset{},callbackOffset{};};
Layout Resolve(const Image& image,std::stop_token token){
 const auto anchors=FindAnchors(image,token);auto table=Decode(anchors[0]),allocation=Decode(anchors[1]),registration=Decode(anchors[2]);if(table.empty()||allocation.empty()||registration.empty())throw std::runtime_error("Signature decoding failed");Layout layout{};layout.table=Rip(table[0]);
 if(!image.In(layout.table,".rdata")||layout.table-image.base!=0x10548CB0)throw std::runtime_error("World table differs from verified character build");
 const auto& first=allocation[0];if(first.info.mnemonic!=ZYDIS_MNEMONIC_MOV||first.operands[0].type!=ZYDIS_OPERAND_TYPE_REGISTER||first.operands[0].reg.value!=ZYDIS_REGISTER_R14||first.operands[1].type!=ZYDIS_OPERAND_TYPE_MEMORY||first.operands[1].mem.base!=ZYDIS_REGISTER_RBX)throw std::runtime_error("World-state load data flow changed");
 layout.stateOffset=static_cast<size_t>(first.operands[1].mem.disp.value);
 layout.stateLoadAddress=first.runtime_address;layout.stateLoadCode.resize(first.info.length);
 if(!ReadBytes(layout.stateLoadAddress,layout.stateLoadCode.data(),layout.stateLoadCode.size()))throw std::runtime_error("State-load snapshot failed");
 // Cross-check RIP-relative constructor address with independent Unicorn execution.
 std::vector<std::uint8_t> leaBytes(table[0].info.length);
 if(!ReadBytes(table[0].runtime_address,leaBytes.data(),leaBytes.size()))throw std::runtime_error("World LEA snapshot failed");
 const auto leaResult=emulation::Run(table[0].runtime_address,leaBytes,{},{});
 if(!leaResult.ok||leaResult.registers[emulation::RCX]!=layout.table)throw std::runtime_error("Unicorn/Zydis world table resolution disagrees");
 Address taskConstructor=0;
 for(size_t i=0;i+1<allocation.size();++i){const auto& ins=allocation[i];const auto& next=allocation[i+1];if(ins.info.mnemonic==ZYDIS_MNEMONIC_CALL&&ins.operands[0].type==ZYDIS_OPERAND_TYPE_IMMEDIATE&&next.info.mnemonic==ZYDIS_MNEMONIC_MOV&&next.operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY&&next.operands[0].mem.base==ZYDIS_REGISTER_R14&&next.operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER&&next.operands[1].reg.value==ZYDIS_REGISTER_R12){taskConstructor=Absolute(ins,ins.operands[0]);layout.tasksOffset=static_cast<size_t>(next.operands[0].mem.disp.value);break;}}
 layout.callback=Rip(registration[0]);bool labelFound=false,slotFound=false;
 for(size_t i=0;i<std::min<size_t>(14,registration.size());++i){const auto& ins=registration[i];
  if(ins.info.mnemonic==ZYDIS_MNEMONIC_MOV&&ins.operands[0].type==ZYDIS_OPERAND_TYPE_MEMORY&&ins.operands[0].mem.base==ZYDIS_REGISTER_R14&&ins.operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER&&ins.operands[1].reg.value==ZYDIS_REGISTER_R8){layout.callbackOffset=static_cast<size_t>(ins.operands[0].mem.disp.value);slotFound=true;}
  if(ins.info.mnemonic==ZYDIS_MNEMONIC_LEA&&ins.operands[0].type==ZYDIS_OPERAND_TYPE_REGISTER&&ins.operands[0].reg.value==ZYDIS_REGISTER_R9){std::array<char,96> label{};if(ReadBytes(Rip(ins),label.data(),label.size())&&std::memcmp(label.data(),"wld::WorldTasks::UpdatePlayerController",sizeof("wld::WorldTasks::UpdatePlayerController"))==0)labelFound=true;}
 }
 if(!taskConstructor||!slotFound||!labelFound||layout.stateOffset>=0x10000||layout.tasksOffset>=0x10000||layout.callbackOffset>=0x10000||image.Function(anchors[0])!=image.Function(anchors[1])||image.Function(anchors[2]).first!=taskConstructor-image.base)throw std::runtime_error("World constructor/task references disagree");
 layout.boneAccessor=anchors[3];
 if(layout.boneAccessor!=image.base+rig::profile::MatrixAccessor||anchors[4]!=image.base+0x09DEA65D||anchors[5]!=image.base+0x00456A24||anchors[6]!=image.base+0x09DF3782||anchors[7]!=image.base+rig::profile::RootAccessor)throw std::runtime_error("Skeleton signature moved beyond the validated layout profile");
 // Decode the native component-index load independently of signature matching.
 bool slotValidated=false;
 for(const auto& ins:Decode(anchors[4]))for(unsigned i=0;i<ins.info.operand_count_visible;++i){const auto& op=ins.operands[i];if(ins.info.mnemonic==ZYDIS_MNEMONIC_MOVZX&&op.type==ZYDIS_OPERAND_TYPE_MEMORY&&op.mem.base==ZYDIS_REGISTER_R10&&op.mem.disp.value==rig::profile::SkeletonSlot)slotValidated=true;}
 if(!slotValidated)throw std::runtime_error("Skeleton component-slot instruction changed");
 return layout;
}
bool ValidWorld(Address world,const Layout& layout){Address table=0,state=0,tasks=0,callback=0;return Read(world,table)&&table==layout.table&&Read(world+layout.stateOffset,state)&&Read(state+layout.tasksOffset,tasks)&&Read(tasks+layout.callbackOffset,callback)&&callback==layout.callback;}
bool EmulatedStateMatches(Address world,const Layout& layout){
 Address expected=0;if(!Read(world+layout.stateOffset,expected))return false;
 emulation::Region data{world+layout.stateOffset,std::vector<std::uint8_t>(8),false};std::memcpy(data.bytes.data(),&expected,8);
 std::array<std::uint64_t,16> registers{};registers[emulation::RBX]=world;
 auto result=emulation::Run(layout.stateLoadAddress,layout.stateLoadCode,registers,std::span(&data,1));
 return result.ok&&result.registers[emulation::R14]==expected;
}
Address DiscoverWorld(const Image& image,const Layout& layout,std::stop_token token){
 CheckStop(token);Address root=0,world=0,state=0,tasks=0;
 if(!image.In(image.base+rig::profile::RootSlot,".data")||!Read(image.base+rig::profile::RootSlot,root)||!HeapLike(root)||!Read(root+rig::profile::WorldFromRoot,world)||!ValidWorld(world,layout)||!EmulatedStateMatches(world,layout)||!Read(world+layout.stateOffset,state)||!Read(state+layout.tasksOffset,tasks)||tasks+0xD0!=root)throw std::runtime_error("Waiting for a validated World registry chain");
 return world;
}
struct Camera {Vec3 position;std::array<float,16> matrix;};
bool ReadCamera(Address address,Camera& camera){
 std::array<unsigned char,256> bytes{};if(!ReadBytes(address+0x190,bytes.data(),bytes.size()))return false;std::memcpy(&camera.position,bytes.data(),12);std::memcpy(camera.matrix.data(),bytes.data()+0xc0,64);
 if(!std::isfinite(camera.position.x)||!std::isfinite(camera.position.y)||!std::isfinite(camera.position.z))return false;
 for(float value:camera.matrix)if(!std::isfinite(value)||std::abs(value)>1e7f)return false;
 const auto& m=camera.matrix;Vec3 a{m[0],m[4],m[8]},b{m[1],m[5],m[9]},f{m[3],m[7],m[11]};
 auto dot=[](const Vec3& x,const Vec3& y){return x.x*y.x+x.y*y.y+x.z*y.z;};const float na=std::sqrt(dot(a,a)),nb=std::sqrt(dot(b,b)),nf=std::sqrt(dot(f,f));
 if(!(na>.05f&&na<10&&nb>.05f&&nb<10&&std::abs(nf-1)<.01f))return false;
 if(std::max({std::abs(dot(a,b)/(na*nb)),std::abs(dot(a,f)/(na*nf)),std::abs(dot(b,f)/(nb*nf))})>.01f)return false;
 const auto& p=camera.position;const float cx=p.x*m[0]+p.y*m[4]+p.z*m[8]+m[12],cy=p.x*m[1]+p.y*m[5]+p.z*m[9]+m[13],cw=p.x*m[3]+p.y*m[7]+p.z*m[11]+m[15];return std::max({std::abs(cx),std::abs(cy),std::abs(cw)})<.15f;
}
static constexpr unsigned char CameraAnchor[]={0xA4,0x70,0x7D,0xBF,0,0,0,0,0,0,0,0,0,0,0xA0,0x40,0,0,0xA0,0xC0,0,0,0,0,0,0,0,0,0xCD,0xCC,0x4C,0x3F,0,0,0,0x3F,0,0,0x80,0x3E};
class CameraSearch {
 struct Range{Address start;size_t size;};
 Buffer scratch{2*1024*1024+sizeof(CameraAnchor)};std::vector<Range> ranges;size_t range=0,offset=0;std::uint64_t total=0,done=0;std::set<Address> candidates;
public:
 CameraSearch(){
  ULONG_PTR low=0,high=0;GetCurrentThreadStackLimits(&low,&high);Address at=0;
  while(at<0x7fffffff0000ull){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)))break;auto begin=reinterpret_cast<Address>(info.BaseAddress);if(!info.RegionSize||begin+info.RegionSize<=at)break;at=begin+info.RegionSize;
   const auto protection=info.Protect&0xff;const bool writable=protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
   const bool scratchRange=begin<reinterpret_cast<Address>(scratch.data)+scratch.size&&at>reinterpret_cast<Address>(scratch.data);
   const bool workerStack=begin<high&&at>low;
   if(info.State==MEM_COMMIT&&info.Type==MEM_PRIVATE&&!(info.Protect&PAGE_GUARD)&&writable&&!scratchRange&&!workerStack){ranges.push_back({begin,info.RegionSize});total+=info.RegionSize;}
  }
  std::sort(ranges.begin(),ranges.end(),[](const Range& a,const Range& b){return a.size>b.size;});
 }
 bool Step(std::stop_token token){
  if(range>=ranges.size())return true;CheckStop(token);const auto& r=ranges[range];const auto core=std::min<size_t>(2*1024*1024,r.size-offset);const auto bytes=std::min(core+sizeof(CameraAnchor)-1,r.size-offset);const auto start=r.start+offset;
  if(ReadBytes(start,scratch.data,bytes)){
   const auto* cursor=scratch.data;const auto* end=scratch.data+bytes;
   while(cursor<end){const auto* hit=std::search(cursor,end,std::begin(CameraAnchor),std::end(CameraAnchor));if(hit==end||static_cast<size_t>(hit-scratch.data)>=core)break;auto candidate=start+static_cast<size_t>(hit-scratch.data)-0x2a4;Camera camera{};if(ReadCamera(candidate,camera))candidates.insert(candidate);cursor=hit+1;}
  }
  done+=core;offset+=core;if(offset==r.size){++range;offset=0;}return range>=ranges.size();
 }
 float Progress()const{return total?static_cast<float>(static_cast<double>(done)/static_cast<double>(total)):1;}
 Address Result()const{std::set<Address> valid;for(auto candidate:candidates){Camera value{};if(ReadCamera(candidate,value))valid.insert(candidate);}if(valid.size()!=1)throw std::runtime_error(valid.empty()?"Camera scan found no validated view-projection block":"Multiple camera blocks; drawing disabled");return *valid.begin();}
};
class PoseSampler {
 const rig::Reader& reader;Address base;emulation::SnapshotRead source;
 std::mutex mutex;std::condition_variable_any wake;
 std::vector<Address> jobs;std::uint64_t generation=0,jobVersion=0;
 std::unordered_set<Address> live;
 std::unordered_map<Address,std::uint64_t> nextDue;
 std::unordered_map<Address,std::shared_ptr<const LocalSkeleton>> results;
 std::unordered_map<Address,std::uint64_t> inFlight;
 std::array<std::jthread,2> threads;
 unsigned workerCount=1;
public:
 PoseSampler(const rig::Reader& r,Address b,emulation::SnapshotRead read):reader(r),base(b),source(std::move(read)){
  const unsigned count=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)>=8?2:1;
  workerCount=count;
  for(unsigned workerIndex=0;workerIndex<count;++workerIndex)threads[workerIndex]=std::jthread([this](std::stop_token stop){
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  Address teb=0;std::uint64_t nextTls=0;std::size_t cursor=0;
  while(!stop.stop_requested()){
   Address actor=0;std::uint64_t epoch=0;
   {std::unique_lock lock(mutex);
    while(!stop.stop_requested()){
     if(!wake.wait(lock,stop,[&]{return !jobs.empty();}))break;
     std::vector<Address> available;available.reserve(jobs.size());for(auto candidate:jobs)if(!inFlight.contains(candidate))available.push_back(candidate);
     const auto now=GetTickCount64();const auto decision=pose::PickJob(available,nextDue,cursor,now);actor=decision.actor;epoch=generation;
     if(actor){inFlight[actor]=epoch;break;}
     const auto version=jobVersion;wake.wait_for(lock,stop,std::chrono::milliseconds(std::max<std::uint64_t>(1,decision.nextWake-now)),[&]{return version!=jobVersion;});
    }
   }
   if(!actor||stop.stop_requested())break;
   if(GetTickCount64()>=nextTls){teb=rig::FindInitializedGameTeb(base,source);nextTls=GetTickCount64()+1000;}
   const auto started=GetTickCount64();auto sampled=reader.Capture(actor,teb,true,stop,true);sampled.capturedMs=started;sampled.captureDurationMs=GetTickCount64()-started;if(sampled.poseValid)sampled.poseMs=sampled.capturedMs;
   {std::lock_guard lock(mutex);if(epoch==generation&&live.contains(actor)){
    auto old=results.find(actor);
    const bool identityChanged=old!=results.end()&&sampled.chainValid&&(old->second->asset!=sampled.asset||old->second->component!=sampled.component||old->second->owner!=sampled.owner);
    if(sampled.worldValid||old==results.end()||identityChanged||!old->second->worldValid||AgeMs(sampled.capturedMs,old->second->capturedMs)>250)results[actor]=std::make_shared<const LocalSkeleton>(std::move(sampled));
   }pose::ReleaseCapture(inFlight,actor,epoch);++jobVersion;wake.notify_all();}
  }
  });
 }
 ~PoseSampler(){for(auto& thread:threads)thread.request_stop();wake.notify_all();for(auto& thread:threads)if(thread.joinable())thread.join();}
 void Reset(){std::lock_guard lock(mutex);jobs.clear();live.clear();results.clear();nextDue.clear();inFlight.clear();++generation;++jobVersion;wake.notify_all();}
 void Update(const std::vector<Address>& entries,const std::vector<Address>& visible){std::lock_guard lock(mutex);live.clear();live.insert(entries.begin(),entries.end());
  std::erase_if(results,[&](const auto& item){return !live.contains(item.first);});std::erase_if(nextDue,[&](const auto& item){return !live.contains(item.first);});
  if(jobs!=visible){jobs=visible;++jobVersion;wake.notify_all();}
 }
 std::shared_ptr<const LocalSkeleton> Get(Address address){std::lock_guard lock(mutex);auto found=results.find(address);return found==results.end()?nullptr:found->second;}
 unsigned WorkerCount()const{return workerCount;}
};
void Publish(Snapshot value){published.store(std::make_shared<const Snapshot>(std::move(value)),std::memory_order_release);}
void Invalidate(const std::string& message,float progress=0){Snapshot value;value.status=message;value.cameraScanProgress=progress;Publish(std::move(value));}
void Status(const std::string& message,float progress=0){auto old=published.load(std::memory_order_acquire);Snapshot value=*old;value.status=message;value.cameraScanProgress=progress;Publish(std::move(value));}
void Run(std::stop_token token){
 try{
  Image image;Status("Validating world and skeleton signatures");const auto layout=Resolve(image,token);Address world=0,camera=0;std::unique_ptr<CameraSearch> search;std::uint64_t nextWorld=0,nextCamera=0;
  std::vector<emulation::CodeRange> executable;
  for(const auto& section:image.sections)if(section.flags&IMAGE_SCN_MEM_EXECUTE)executable.push_back({section.address,section.size});
  const emulation::SnapshotRead source=[](std::uint64_t address,void* out,std::size_t size){return ReadBytes(address,out,size);};
  rig::Reader skeletonReader(image.base,layout.boneAccessor,source,executable);
  PoseSampler poses(skeletonReader,image.base,source);
  unsigned badCameraReads=0;
  // Last independently readable character root per instance. A single torn
  // read must not blink the actor out of the published frame.
  std::unordered_map<Address,std::pair<Vec3,std::uint64_t>> positions;
  while(!token.stop_requested()){
   dx12_hooks::FlushDiagnostics();
   const auto now=GetTickCount64();
   if(!world||!ValidWorld(world,layout)){
    world=0;camera=0;search.reset();poses.Reset();Invalidate("World changed; reacquiring");if(now<nextWorld){std::this_thread::sleep_for(std::chrono::milliseconds(30));continue;}
    Status("Resolving the active offline world");
    try{world=DiscoverWorld(image,layout,token);}catch(const std::runtime_error& e){Status(e.what());nextWorld=now+2000;continue;}
   }
   if(!camera){
    if(now<nextCamera){std::this_thread::sleep_for(std::chrono::milliseconds(30));continue;}
    if(!search){Status("Scanning for the active camera");search=std::make_unique<CameraSearch>();}
    bool complete=false;for(unsigned n=0;n<4&&!complete;++n)complete=search->Step(token);
    Status("Scanning for the active camera",search->Progress());
    if(complete){try{camera=search->Result();}catch(const std::runtime_error& e){Status(e.what());nextCamera=GetTickCount64()+5000;}search.reset();}
    std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;
   }
   Camera view{};if(!ReadCamera(camera,view)){
    if(++badCameraReads<30){std::this_thread::sleep_for(std::chrono::milliseconds(4));continue;}
    camera=0;badCameraReads=0;Invalidate("Camera changed; revalidating");continue;
   }badCameraReads=0;
   Address array=0;std::uint32_t metadata=0;if(!Read(world+rig::profile::Phase5Array,array)||!Read(world+rig::profile::Phase5Count,metadata)){world=0;continue;}const auto count=metadata&rig::profile::CountMask;
   if(count>256){world=0;Status("Character count no longer validates");continue;}
   std::vector<Address> entries(count);if(count&&!ReadBytes(array,entries.data(),entries.size()*8)){world=0;continue;}
   std::vector<std::pair<float,Address>> visible;
   auto samplingMatrix=view.matrix;for(unsigned row=0;row<4;++row){samplingMatrix[row*4]*=.8f;samplingMatrix[row*4+1]*=.8f;}
   Snapshot frame;frame.ready=true;frame.world=world;frame.camera=camera;frame.cameraPosition=view.position;frame.viewProjection=view.matrix;frame.cameraScanProgress=1;
   for(Address address:entries){Address table=0;Vec3 position{};
    const bool readOk=Read(address,table)&&table==image.base+rig::profile::CharacterTable&&Read(address+rig::profile::CharacterPosition,position)&&std::isfinite(position.x)&&std::isfinite(position.y)&&std::isfinite(position.z)&&std::max({std::abs(position.x),std::abs(position.y),std::abs(position.z)})<=1000000;
    if(readOk){positions[address]={position,now};}
    else{const auto cached=positions.find(address);if(cached==positions.end()||AgeMs(now,cached->second.second)>250){positions.erase(address);continue;}position=cached->second.first;}
    Character actor{address,position};if(auto sampled=poses.Get(address))actor.skeleton=*sampled;
    const auto distance=Distance(position,view.position);const float nearX=position.x-view.position.x,nearY=position.y-view.position.y;
    const bool nearCamera=nearX*nearX+nearY*nearY<.36f&&std::abs(position.z-view.position.z)<3.f;
    auto top=position;top.z+=2.2f;ScreenPoint screenA{},screenB{};
    if(distance<500.f&&(!nearCamera||sampleNearCamera.load(std::memory_order_relaxed))&&ProjectSegment(position,top,samplingMatrix,1,1,screenA,screenB))visible.push_back({distance,address});
    // Pose offsets update at the sampler cadence; the corroborated position is
    // refreshed by this fast worker. Rebase all joints together, never mix two
    // origins or keep a moving actor at the last expensive pose capture point.
    if(actor.skeleton.worldValid&&AgeMs(GetTickCount64(),actor.skeleton.capturedMs)<=250){
     actor.bones=actor.skeleton.worldBones;
     const Vec3 sampledAnchor{actor.skeleton.rootMatrix[12],actor.skeleton.rootMatrix[13],actor.skeleton.rootMatrix[14]};
     pose::RebaseWorld(actor.bones,sampledAnchor,position);
     actor.skeleton.rootMatrix[12]=position.x;actor.skeleton.rootMatrix[13]=position.y;actor.skeleton.rootMatrix[14]=position.z;
     actor.skeleton.worldBones=actor.bones;
    }
    else{actor.skeleton.worldBones.clear();}
    frame.characters.push_back(std::move(actor));
   }
   std::erase_if(positions,[&](const auto& item){return std::find(entries.begin(),entries.end(),item.first)==entries.end();});
   std::sort(visible.begin(),visible.end());std::vector<Address> requested;requested.reserve(visible.size());for(const auto& item:visible)requested.push_back(item.second);
   poses.Update(entries,requested);frame.scheduledPoses=static_cast<unsigned>(requested.size());frame.poseWorkers=poses.WorkerCount();
   std::uint32_t after=0;Address arrayAfter=0;if(!Read(world+rig::profile::Phase5Count,after)||after!=metadata||!Read(world+rig::profile::Phase5Array,arrayAfter)||arrayAfter!=array)frame.ready=false;
   if(!frame.ready){Status("Character layout changed; waiting for a consistent frame");std::this_thread::sleep_for(std::chrono::milliseconds(20));continue;}
   const auto chains=std::count_if(frame.characters.begin(),frame.characters.end(),[](const auto& actor){return actor.skeleton.chainValid;});
   const auto poseCount=std::count_if(frame.characters.begin(),frame.characters.end(),[](const auto& actor){return !actor.bones.empty();});
   frame.skeletonStatus=std::to_string(chains)+" validated rigs, "+std::to_string(poseCount)+" world skeletons; "+std::to_string(requested.size())+" visible pose jobs";
   frame.capturedMs=GetTickCount64();frame.status="Read-only phase-5 character cache ready";Publish(std::move(frame));std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
 }catch(const Cancelled&){}catch(const std::exception& e){Status(e.what());}
}
}
void Start(){Stop();worker=std::jthread(Run);}
void Stop(){if(worker.joinable()){worker.request_stop();worker.join();}Status("Data worker stopped");}
void RequestRescan(){Start();}
std::shared_ptr<const Snapshot> LatestShared(){
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
 {std::lock_guard lock(dataMutex);if(validationOverride){auto frame=validationFrame;frame.capturedMs=GetTickCount64();return std::make_shared<const Snapshot>(std::move(frame));}}
#endif
 return published.load(std::memory_order_acquire);
}
Snapshot Latest(){return *LatestShared();}
bool RefreshCamera(std::uint64_t address,Vec3& position,std::array<float,16>& matrix){Camera value{};if(!address||!ReadCamera(address,value))return false;position=value.position;matrix=value.matrix;return true;}
void SetNearCameraSampling(bool enabled){sampleNearCamera.store(enabled,std::memory_order_relaxed);}
bool SelfTest(){
 if(!pose::ScheduleSelfTest()||!emulation::SelfTest()||!emulation::CopiedMatrixSelfTest()||!rig::SelfTest()||!SkeletonSelfTest())return false;
 const unsigned char memberCode[]={0x4C,0x8B,0xB3,0x98,0x01,0x00,0x00};ZydisDisassembledInstruction member{};
 if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,0x1000,memberCode,sizeof(memberCode),&member))||member.info.mnemonic!=ZYDIS_MNEMONIC_MOV||member.operands[0].reg.value!=ZYDIS_REGISTER_R14||member.operands[1].mem.base!=ZYDIS_REGISTER_RBX||member.operands[1].mem.disp.value!=0x198)return false;
 const unsigned char ripCode[]={0x48,0x8D,0x0D,0x33,0x00,0x00,0x00};ZydisDisassembledInstruction rip{};
 if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,0x1000,ripCode,sizeof(ripCode),&rip))||Rip(rip)!=0x103A)return false;
 const auto p=Parse("AA ?? BB");const unsigned char good[]={0xAA,0x19,0xBB},bad[]={0xAB,0x19,0xBB};if(!Match(good,p)||Match(bad,p))return false;
 std::array<float,16> m={1,0,0,0,0,0,1,1,0,1,0,0,0,-1.6f,4.99f,5};ScreenPoint screen{};
 if(!Project({0,0,1.6f},m,1280,720,screen)||std::abs(screen.x-640)>.01f||std::abs(screen.y-360)>.01f)return false;
 if(Project({0,-6,1.6f},m,1280,720,screen)||Project({NAN,0,0},m,1280,720,screen)||Project({0,0,0},m,0,720,screen))return false;
 return true;
}
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
void SetValidationSnapshot(const Snapshot* value){std::lock_guard lock(dataMutex);validationOverride=value!=nullptr;if(value)validationFrame=*value;}
#endif
}

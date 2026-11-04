#include "copied_emulation.h"
#include <unicorn/unicorn.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <cstdio>
namespace emulation { namespace {
constexpr std::uint64_t Page=4096, Stack=0x500000000000ULL;
constexpr std::uint64_t StackSize=0x40000, Output=Stack+0x20000;
void Check(uc_err error){if(error!=UC_ERR_OK)throw std::runtime_error(uc_strerror(error));}
bool Inside(std::uint64_t address,std::size_t size,std::uint64_t start,std::uint64_t length){
 return address>=start&&size<=length&&address-start<=length-size;
}
struct Decoded {std::array<std::uint8_t,15> bytes{};std::uint32_t size{};bool allowed=false;};
thread_local std::unordered_map<std::uint64_t,Decoded> decodedCache;
struct State {
 const MatrixRequest& request; const SnapshotRead& read; std::span<const CodeRange> code;
 std::stop_token stop; std::set<std::uint64_t> pages;
 std::map<std::uint64_t,std::array<std::uint8_t,Page>> originalPages;
 std::set<std::uint64_t> dirty;
 std::set<std::uint64_t> dirtyStack;
 std::set<std::uint64_t> frozenStreams;
 std::unordered_map<std::uint64_t,Decoded>& decoded=decodedCache;
 ZydisDecoder decoder{};
 std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
 std::size_t steps=0,blocks=0; bool finished=false; std::string error;
 bool Executable(std::uint64_t address,std::size_t size)const{
  return std::any_of(code.begin(),code.end(),[&](const auto& r){return Inside(address,size,r.address,r.size);});
 }
};
void Fail(uc_engine* uc,State& s,const char* error){s.error=error;uc_emu_stop(uc);}
void FreezeStream(uc_engine* uc,State& s){
 std::uint64_t address=0,index=0;Check(uc_reg_read(uc,UC_X86_REG_R8,&address));Check(uc_reg_read(uc,UC_X86_REG_RDX,&index));
 if((index&0xffff)*32>=s.request.snapshotBytesFromR8)throw std::runtime_error("Decoded pose index outside rig stream");
 if(s.frozenStreams.contains(address))return;
 const auto first=address&~(Page-1),end=(address+s.request.snapshotBytesFromR8+Page-1)&~(Page-1);
 if(address<0x10000||end<address||end>=0x7FFFFFFFF000ULL||end-first>65536||s.pages.size()+(end-first)/Page>s.request.pageBudget)throw std::runtime_error("Invalid copied pose stream range");
 std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end-first));
 if(!s.read(first,bytes.data(),bytes.size()))throw std::runtime_error("Contiguous pose stream snapshot unreadable");
 for(auto at=first;at<end;at+=Page){
  if(!s.pages.contains(at)){
   Check(uc_mem_map(uc,at,Page,UC_PROT_ALL));s.pages.insert(at);
   std::array<std::uint8_t,Page> page{};std::memcpy(page.data(),bytes.data()+at-first,Page);
   Check(uc_mem_write(uc,at,page.data(),Page));s.originalPages.emplace(at,page);
  }
 }
 s.frozenStreams.insert(address);
}
bool MapPage(uc_engine* uc,State& s,std::uint64_t address){
 const auto at=address&~(Page-1);
 if(Inside(at,Page,Stack,StackSize)||s.pages.contains(at))return true;
 if(at<0x10000||at>=0x7FFFFFFFF000ULL||s.pages.size()>=s.request.pageBudget)return false;
 std::array<std::uint8_t,Page> bytes{};
 if(!s.read(at,bytes.data(),bytes.size()))return false;
 if(uc_mem_map(uc,at,Page,UC_PROT_ALL)!=UC_ERR_OK||uc_mem_write(uc,at,bytes.data(),bytes.size())!=UC_ERR_OK)return false;
 s.pages.insert(at);s.originalPages.emplace(at,bytes);return true;
}
void Write(uc_engine*,uc_mem_type,std::uint64_t address,int size,std::int64_t,void* user){
 auto& s=*static_cast<State*>(user);if(size<=0)return;
 for(auto at=address&~(Page-1);at<=((address+static_cast<unsigned>(size)-1)&~(Page-1));at+=Page){if(Inside(at,Page,Stack,StackSize))s.dirtyStack.insert(at);else s.dirty.insert(at);}
}
bool Missing(uc_engine* uc,uc_mem_type,std::uint64_t address,int size,std::int64_t,void* user){
 auto& s=*static_cast<State*>(user);
 try{
  if(size<=0||size>4096||address+static_cast<unsigned>(size)<address){s.error="Invalid copied-memory access";return false;}
  if(s.stop.stop_requested()){s.error="Pose capture cancelled";return false;}
  const auto last=(address+static_cast<unsigned>(size)-1)&~(Page-1);
  for(auto at=address&~(Page-1);at<=last;at+=Page)if(!MapPage(uc,s,at)){s.error="Source page unreadable or copy budget exceeded";return false;}
  return true;
 }catch(...){s.error="Source snapshot callback failed";return false;}
}
bool EnvironmentInstruction(ZydisMnemonic m){switch(m){
 case ZYDIS_MNEMONIC_SYSCALL:case ZYDIS_MNEMONIC_SYSENTER:case ZYDIS_MNEMONIC_SYSEXIT:case ZYDIS_MNEMONIC_SYSRET:
 case ZYDIS_MNEMONIC_INT:case ZYDIS_MNEMONIC_INT1:case ZYDIS_MNEMONIC_INT3:case ZYDIS_MNEMONIC_HLT:
 case ZYDIS_MNEMONIC_CPUID:case ZYDIS_MNEMONIC_RDTSC:case ZYDIS_MNEMONIC_RDTSCP:
 case ZYDIS_MNEMONIC_RDMSR:case ZYDIS_MNEMONIC_WRMSR:case ZYDIS_MNEMONIC_RDRAND:case ZYDIS_MNEMONIC_RDSEED:
 case ZYDIS_MNEMONIC_IN:case ZYDIS_MNEMONIC_OUT: return true;
 default:return false;
}}
void Code(uc_engine* uc,std::uint64_t address,std::uint32_t size,void* user){
 auto& s=*static_cast<State*>(user);
 try{
  if(address==s.request.returnAddress){s.finished=true;uc_emu_stop(uc);return;}
  if(s.stop.stop_requested()){Fail(uc,s,"Pose capture cancelled");return;}
  if((s.blocks++&31)==0&&std::chrono::steady_clock::now()-s.started>std::chrono::microseconds(s.request.timeoutMicroseconds)){Fail(uc,s,"Copied-accessor time budget reached");return;}
  if(!s.Executable(address,size)){char error[128];std::snprintf(error,sizeof(error),"Copied accessor left approved executable ranges at 0x%llX",static_cast<unsigned long long>(address));s.error=error;uc_emu_stop(uc);return;}
  if(s.request.snapshotCheckpoint==address&&s.request.snapshotBytesFromR8)FreezeStream(uc,s);
  std::array<std::uint8_t,15> bytes{};
  if(!size||size>bytes.size()||uc_mem_read(uc,address,bytes.data(),size)!=UC_ERR_OK){Fail(uc,s,"Copied instruction unreadable");return;}
  auto found=s.decoded.find(address);
  if(found==s.decoded.end()||found->second.size!=size||!std::equal(bytes.begin(),bytes.begin()+size,found->second.bytes.begin())){
   ZydisDecodedInstruction ins{};ZydisDecoderContext context{};
   if(!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&s.decoder,&context,bytes.data(),size,&ins))||ins.length!=size){Fail(uc,s,"Zydis could not decode copied instruction");return;}
   found=s.decoded.insert_or_assign(address,Decoded{bytes,size,!EnvironmentInstruction(ins.mnemonic)}).first;
  }
  if(!found->second.allowed){Fail(uc,s,"Environment-dependent instruction rejected");return;}
  ++s.steps;
 }catch(const std::exception& error){s.error=error.what();uc_emu_stop(uc);}catch(...){Fail(uc,s,"Copied instruction callback failed");}
}
}
MatrixResult RunCopiedMatrix(const MatrixRequest& request,const SnapshotRead& read,std::span<const CodeRange> code,std::stop_token stop){
 auto results=RunCopiedMatrices(std::span(&request,1),read,code,stop);return std::move(results.front());
}
std::vector<MatrixResult> RunCopiedMatrices(std::span<const MatrixRequest> requests,const SnapshotRead& read,std::span<const CodeRange> code,std::stop_token stop){
 std::vector<MatrixResult> results(requests.size());
 if(requests.empty())return results;
 try{
  if(!read||code.empty()||code.size()>96||requests.size()>1024)throw std::runtime_error("Invalid copied-accessor batch limits");
  MatrixRequest request=requests.front();
  if(decodedCache.size()>16384)decodedCache.clear();
  State state{request,read,code,stop};
  if(!ZYAN_SUCCESS(ZydisDecoderInit(&state.decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64)))throw std::runtime_error("Zydis decoder initialization failed");
  uc_engine* raw=nullptr;Check(uc_open(UC_ARCH_X86,UC_MODE_64,&raw));std::unique_ptr<uc_engine,decltype(&uc_close)> uc(raw,&uc_close);
  Check(uc_mem_map(uc.get(),Stack,StackSize,UC_PROT_ALL));
  uc_context* contextRaw=nullptr;Check(uc_context_alloc(uc.get(),&contextRaw));std::unique_ptr<uc_context,decltype(&uc_context_free)> initial(contextRaw,&uc_context_free);Check(uc_context_save(uc.get(),initial.get()));
  const std::array<std::uint8_t,Page> zeroPage{};
  uc_hook missing{},codeHook{},writeHook{};
  Check(uc_hook_add(uc.get(),&missing,UC_HOOK_MEM_UNMAPPED,reinterpret_cast<void*>(Missing),&state,1,0));
  Check(uc_hook_add(uc.get(),&codeHook,UC_HOOK_CODE,reinterpret_cast<void*>(Code),&state,1,0));
  Check(uc_hook_add(uc.get(),&writeHook,UC_HOOK_MEM_WRITE,reinterpret_cast<void*>(Write),&state,1,0));
  for(std::size_t index=0;index<requests.size();++index){auto& result=results[index];request=requests[index];
   try{
    if(stop.stop_requested())throw std::runtime_error("Pose batch cancelled");
    if(!request.instructionBudget||request.instructionBudget>30000||!request.pageBudget||request.pageBudget>4096||!request.timeoutMicroseconds||request.timeoutMicroseconds>1000000||!state.Executable(request.entry,1)||!state.Executable(request.returnAddress,1)||Inside(request.entry,1,Stack,StackSize)||request.entry==request.returnAddress)throw std::runtime_error("Invalid copied-accessor limits or entry");
    if(request.snapshotBytesFromR8&&(!request.snapshotCheckpoint||request.snapshotBytesFromR8>32768||!state.Executable(request.snapshotCheckpoint,1)))throw std::runtime_error("Invalid stream checkpoint");
    for(auto page:state.dirty){const auto found=state.originalPages.find(page);if(found!=state.originalPages.end())Check(uc_mem_write(uc.get(),page,found->second.data(),Page));}state.dirty.clear();
    Check(uc_context_restore(uc.get(),initial.get()));
    for(auto page:state.dirtyStack)Check(uc_mem_write(uc.get(),page,zeroPage.data(),Page));state.dirtyStack.clear();
    std::uint64_t rsp=Stack+0x18008;Check(uc_mem_write(uc.get(),rsp,&request.returnAddress,8));
    Check(uc_reg_write(uc.get(),UC_X86_REG_RSP,&rsp));Check(uc_reg_write(uc.get(),UC_X86_REG_RCX,&request.argument));
    const std::uint64_t output=Output,flags=2;Check(uc_reg_write(uc.get(),UC_X86_REG_RDX,&output));Check(uc_reg_write(uc.get(),UC_X86_REG_GS_BASE,&request.gsBase));Check(uc_reg_write(uc.get(),UC_X86_REG_RFLAGS,&flags));
    state.steps=0;state.blocks=0;state.error.clear();state.finished=false;state.started=std::chrono::steady_clock::now();
    if(!MapPage(uc.get(),state,request.entry))throw std::runtime_error("Accessor entry page unreadable");
    const auto error=uc_emu_start(uc.get(),request.entry,0,0,request.instructionBudget);
    result.instructions=state.steps;result.mappedPages=state.pages.size();
    if(!state.error.empty())throw std::runtime_error(state.error);Check(error);
    if(!state.finished)throw std::runtime_error("Copied-accessor instruction budget or timeout reached");
    Check(uc_mem_read(uc.get(),Output,result.matrix.data(),sizeof(result.matrix)));result.ok=true;
   }catch(const std::exception& error){result.error=error.what();}
  }
 }catch(const std::exception& error){for(auto& result:results)if(!result.ok)result.error=error.what();}
 return results;
}
bool CopiedMatrixSelfTest(){
 // Copied code calls a copied helper, writes a copied source page, reads GS TLS,
 // stores its result in output, and returns. Source bytes must remain unchanged.
 std::array<std::uint8_t,Page> code{},data{};
 const std::uint8_t body[]={0x48,0x83,0xEC,0x28,0xE8,0x17,0,0,0,0x48,0x83,0xC4,0x28,0xC3};
 const std::uint8_t helper[]={0x48,0xC7,0x01,0x44,0x33,0x22,0x11,0x65,0x48,0x8B,0x04,0x25,0x10,0,0,0,0x48,0x89,0x02,0xC3};
 std::memcpy(code.data(),body,sizeof(body));std::memcpy(code.data()+0x20,helper,sizeof(helper));
 const std::uint64_t tls=0xAABBCCDD11223344ULL;std::memcpy(data.data()+0x10,&tls,8);
 const auto original=data;SnapshotRead reader=[&](std::uint64_t at,void* out,std::size_t n){if(n!=Page)return false;if(at==0x100000)std::memcpy(out,code.data(),n);else if(at==0x200000)std::memcpy(out,data.data(),n);else return false;return true;};
 const CodeRange range{0x100000,Page};MatrixRequest request{0x100000,0x100100,0x200000,0x200000};
 auto result=RunCopiedMatrix(request,reader,std::span(&range,1));std::uint64_t actual=0;std::memcpy(&actual,result.matrix.data(),8);
 if(!result.ok||actual!=tls||data!=original)return false;
 const std::uint8_t increment[]={0x48,0xFF,0x01,0x48,0x8B,0x01,0x48,0x89,0x02,0xC3};
 std::memcpy(code.data()+0x20,increment,sizeof(increment));
 const std::array<MatrixRequest,2> repeated={request,request};const auto batch=RunCopiedMatrices(repeated,reader,std::span(&range,1));
 for(const auto& entry:batch){actual=0;std::memcpy(&actual,entry.matrix.data(),8);if(!entry.ok||actual!=1||data!=original)return false;}
 // A pose stream crossing a page boundary must be copied contiguously at the
 // checkpoint, rather than fetching its later page after more accessor calls.
 std::array<std::uint8_t,Page*2> stream{};
 const std::uint64_t expected=0x1122334455667788ULL;std::memcpy(stream.data()+Page+0x10,&expected,8);
 const auto untouched=stream;
 const std::uint8_t streamBody[]={0x49,0xB8,0xF0,0x0F,0x21,0,0,0,0,0,0x49,0x8B,0x40,0x20,0x48,0x89,0x02,0xC3};
 std::memcpy(code.data(),streamBody,sizeof(streamBody));unsigned contiguousReads=0;
 SnapshotRead streamRead=[&](std::uint64_t at,void* out,std::size_t n){
  if(at==0x100000&&n==Page){std::memcpy(out,code.data(),n);return true;}
  if(at==0x210000&&n==stream.size()){++contiguousReads;std::memcpy(out,stream.data(),n);return true;}
  return false;
 };
 auto streamRequest=request;streamRequest.snapshotCheckpoint=0x10000A;streamRequest.snapshotBytesFromR8=64;
 const std::array<MatrixRequest,2> streamRequests={streamRequest,streamRequest};const auto frozen=RunCopiedMatrices(streamRequests,streamRead,std::span(&range,1));
 for(const auto& entry:frozen){std::memcpy(&actual,entry.matrix.data(),8);if(!entry.ok||actual!=expected)return false;}
 if(contiguousReads!=1||stream!=untouched)return false;
 // Stack/output pages written by one invocation must be zero for the next,
 // even though untouched portions of the 256 KiB stack are no longer cleared.
 const std::uint8_t stackBody[]={0x48,0x85,0xC9,0x74,0x09,0x48,0xC7,0x44,0x24,0xF8,0x2A,0,0,0,0x48,0x8B,0x44,0x24,0xF8,0x48,0x89,0x02,0xC3};
 std::memcpy(code.data()+0x200,stackBody,sizeof(stackBody));
 auto firstStack=request;firstStack.entry=0x100200;firstStack.argument=1;auto secondStack=firstStack;secondStack.argument=0;
 const std::array<MatrixRequest,2> stackRequests={firstStack,secondStack};const auto stackResults=RunCopiedMatrices(stackRequests,reader,std::span(&range,1));
 for(std::size_t i=0;i<stackResults.size();++i){std::memcpy(&actual,stackResults[i].matrix.data(),8);if(!stackResults[i].ok||actual!=(i==0?42ULL:0ULL))return false;}
 code[0]=0x0F;code[1]=0x05;if(RunCopiedMatrix(request,reader,std::span(&range,1)).ok)return false;
 code[0]=0xEB;code[1]=0xFE;request.instructionBudget=8;if(RunCopiedMatrix(request,reader,std::span(&range,1)).ok)return false;
 request.entry=0x300000;return !RunCopiedMatrix(request,reader,std::span(&range,1)).ok;
}
}

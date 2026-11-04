#include "emulation.h"
#include <unicorn/unicorn.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
namespace emulation { namespace {
constexpr std::uint64_t page=4096;
const int ids[]={UC_X86_REG_RAX,UC_X86_REG_RCX,UC_X86_REG_RDX,UC_X86_REG_RBX,UC_X86_REG_RSP,UC_X86_REG_RBP,UC_X86_REG_RSI,UC_X86_REG_RDI,UC_X86_REG_R8,UC_X86_REG_R9,UC_X86_REG_R10,UC_X86_REG_R11,UC_X86_REG_R12,UC_X86_REG_R13,UC_X86_REG_R14,UC_X86_REG_R15};
void Check(uc_err error){if(error!=UC_ERR_OK)throw std::runtime_error(uc_strerror(error));}
bool Contains(std::uint64_t start,std::size_t count,std::uint64_t address,std::size_t size){return address>=start&&size<=count&&address-start<=count-size;}
struct State {std::uint64_t address;std::span<const std::uint8_t> code;std::span<const Region> memory;std::size_t steps=0;const char* error=nullptr;};
void Fail(uc_engine* uc,State& s,const char* error){s.error=error;uc_emu_stop(uc);}
bool Allowed(ZydisMnemonic m){switch(m){
case ZYDIS_MNEMONIC_MOV:case ZYDIS_MNEMONIC_MOVZX:case ZYDIS_MNEMONIC_MOVSX:case ZYDIS_MNEMONIC_MOVSXD:case ZYDIS_MNEMONIC_LEA:
case ZYDIS_MNEMONIC_ADD:case ZYDIS_MNEMONIC_SUB:case ZYDIS_MNEMONIC_XOR:case ZYDIS_MNEMONIC_AND:case ZYDIS_MNEMONIC_OR:
case ZYDIS_MNEMONIC_NOT:case ZYDIS_MNEMONIC_NEG:case ZYDIS_MNEMONIC_ROL:case ZYDIS_MNEMONIC_ROR:case ZYDIS_MNEMONIC_SHL:
case ZYDIS_MNEMONIC_SHR:case ZYDIS_MNEMONIC_SAR:case ZYDIS_MNEMONIC_BSWAP:case ZYDIS_MNEMONIC_IMUL:case ZYDIS_MNEMONIC_MUL:
case ZYDIS_MNEMONIC_INC:case ZYDIS_MNEMONIC_DEC:case ZYDIS_MNEMONIC_CMP:case ZYDIS_MNEMONIC_TEST:case ZYDIS_MNEMONIC_NOP:
case ZYDIS_MNEMONIC_JMP: return true;
default:return false;
}}
void CodeHook(uc_engine* uc,std::uint64_t address,std::uint32_t size,void* user){
 auto& s=*static_cast<State*>(user);
 if(!Contains(s.address,s.code.size(),address,size)){Fail(uc,s,"Execution outside supplied code");return;}
 ZydisDisassembledInstruction ins{};
 if(!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64,address,s.code.data()+(address-s.address),s.code.size()-static_cast<std::size_t>(address-s.address),&ins))||ins.info.length!=size||!Allowed(ins.info.mnemonic)){Fail(uc,s,"Instruction rejected by Zydis allowlist");return;}
 for(unsigned i=0;i<ins.info.operand_count;++i){const auto& op=ins.operands[i];if(op.type==ZYDIS_OPERAND_TYPE_MEMORY&&(op.mem.segment==ZYDIS_REGISTER_FS||op.mem.segment==ZYDIS_REGISTER_GS)){Fail(uc,s,"TLS segment base unavailable");return;}}
 ++s.steps;
}
void MemoryHook(uc_engine* uc,uc_mem_type type,std::uint64_t address,int size,std::int64_t,void* user){
 auto& s=*static_cast<State*>(user);bool write=type==UC_MEM_WRITE;
 if(size>0&&!write&&Contains(s.address,s.code.size(),address,static_cast<std::size_t>(size)))return;
 for(const auto& r:s.memory)if(size>0&&Contains(r.address,r.bytes.size(),address,static_cast<std::size_t>(size))&&(!write||r.writable))return;
 Fail(uc,s,write?"Write outside writable emulator snapshot":"Read outside supplied snapshot");
}
}
Result Run(std::uint64_t address,std::span<const std::uint8_t> code,const std::array<std::uint64_t,16>& registers,std::span<const Region> memory,std::size_t budget){
 Result result;result.registers=registers;
 try{
  if(code.empty()||code.size()>65536||!budget||budget>4096||memory.size()>64)throw std::runtime_error("Invalid emulation limits");
  uc_engine* raw=nullptr;Check(uc_open(UC_ARCH_X86,UC_MODE_64,&raw));std::unique_ptr<uc_engine,decltype(&uc_close)> uc(raw,&uc_close);
  std::set<std::uint64_t> pages;std::size_t total=0;
  auto map=[&](std::uint64_t start,std::span<const std::uint8_t> bytes,std::uint32_t permissions){
   if(bytes.empty()||start<page||start>0x00007fffffffffffULL||bytes.size()>0x0000800000000000ULL-start)throw std::runtime_error("Invalid snapshot address");
   const auto begin=start&~(page-1),end=(start+bytes.size()+page-1)&~(page-1);
   if(end-begin>1024*1024-total)throw std::runtime_error("Snapshot map exceeds 1 MiB");total+=static_cast<std::size_t>(end-begin);
   for(auto at=begin;at<end;at+=page)if(!pages.insert(at).second)throw std::runtime_error("Snapshot pages overlap");
   Check(uc_mem_map(uc.get(),begin,static_cast<std::size_t>(end-begin),permissions));Check(uc_mem_write(uc.get(),start,bytes.data(),bytes.size()));
  };
  map(address,code,UC_PROT_READ|UC_PROT_EXEC);
  for(const auto& r:memory)map(r.address,r.bytes,UC_PROT_READ|(r.writable?UC_PROT_WRITE:0));
  for(unsigned i=0;i<16;++i)Check(uc_reg_write(uc.get(),ids[i],&registers[i]));
  // Explicit reset flags. Conditional/flag-consuming instructions are not allowed.
  std::uint64_t flags=2;Check(uc_reg_write(uc.get(),UC_X86_REG_RFLAGS,&flags));
  State state{address,code,memory};uc_hook hookCode{},hookMemory{};
  Check(uc_hook_add(uc.get(),&hookCode,UC_HOOK_CODE,reinterpret_cast<void*>(CodeHook),&state,1,0));
  Check(uc_hook_add(uc.get(),&hookMemory,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,reinterpret_cast<void*>(MemoryHook),&state,1,0));
  const auto error=uc_emu_start(uc.get(),address,address+code.size(),100000,budget);
  result.instructions=state.steps;Check(uc_reg_read(uc.get(),UC_X86_REG_RIP,&result.rip));
  if(state.error)throw std::runtime_error(state.error);Check(error);
  if(result.rip!=address+code.size())throw std::runtime_error("Instruction budget or timeout reached before end");
  for(unsigned i=0;i<16;++i)Check(uc_reg_read(uc.get(),ids[i],&result.registers[i]));
  result.memory.assign(memory.begin(),memory.end());for(auto& r:result.memory)Check(uc_mem_read(uc.get(),r.address,r.bytes.data(),r.bytes.size()));result.ok=true;
 }catch(const std::exception& e){result.error=e.what();}
 return result;
}
bool SelfTest(){
 std::array<std::uint64_t,16> regs{};regs[RBX]=0x4000;
 const std::uint8_t load[]={0x4c,0x8b,0xb3,0x98,1,0,0};std::uint64_t pointer=0x123456789abcdef0ULL;
 Region region{0x4198,std::vector<std::uint8_t>(8),false};std::memcpy(region.bytes.data(),&pointer,8);
 auto r=Run(0x1000,load,regs,std::span(&region,1));if(!r.ok||r.registers[R14]!=pointer||r.instructions!=1)return false;
 const std::uint8_t lea[]={0x48,0x8d,0x0d,0x33,0,0,0};r=Run(0x1000,lea,regs,{});if(!r.ok||r.registers[RCX]!=0x103a)return false;
 // Rotate/xor pointer arithmetic, then 32-bit write zero-extension.
 const std::uint8_t rotate[]={0x48,0x89,0xd8,0x48,0xc1,0xc0,8,0x48,0x83,0xf0,0x55};
 regs[RBX]=0x123456789abcdef0ULL;r=Run(0x1000,rotate,regs,{});if(!r.ok||r.registers[RAX]!=0x3456789abcdef047ULL)return false;
 const std::uint8_t partial[]={0xb4,0x12,0x66,0xb8,0x78,0x56};regs[RAX]=0xfedcba9876543210ULL;
 r=Run(0x1000,partial,regs,{});if(!r.ok||r.registers[RAX]!=0xfedcba9876545678ULL)return false;
 const std::uint8_t arithmetic[]={0x48,0x89,0xd8,0x48,0xc1,0xc0,8,0x48,0x83,0xf0,0x55,0xb8,0x78,0x56,0x34,0x12};
 regs[RBX]=~0ULL;r=Run(0x1000,arithmetic,regs,{});if(!r.ok||r.registers[RAX]!=0x12345678)return false;
 const std::uint8_t store[]={0x48,0x89,0x03,0x48,0x8b,0x0b};regs[RBX]=region.address;regs[RAX]=pointer;
 if(Run(0x1000,store,regs,std::span(&region,1)).ok)return false;
 auto original=region.bytes;region.writable=true;r=Run(0x1000,store,regs,std::span(&region,1));if(!r.ok||r.registers[RCX]!=pointer||region.bytes!=original)return false;
 const std::uint8_t call[]={0xe8,0,0,0,0},syscall[]={0x0f,5},loop[]={0xeb,0xfe};
 if(Run(0x1000,call,regs,{}).ok||Run(0x1000,syscall,regs,{}).ok||Run(0x1000,loop,regs,{},8).ok)return false;
 const std::uint8_t paddingRead[]={0x48,0x8b,0x03};regs[RBX]=region.address+8;if(Run(0x1000,paddingRead,regs,std::span(&region,1)).ok)return false;
 const std::uint8_t tls[]={0x65,0x48,0x8b,0x04,0x25,0x30,0,0,0};if(Run(0x1000,tls,regs,{}).ok)return false;
 if(Run(0x1000,lea,regs,std::span(&region,1),0).ok)return false;
 Region overlap{0x1010,std::vector<std::uint8_t>(8),false};if(Run(0x1000,lea,regs,std::span(&overlap,1)).ok)return false;
 return true;
}
}

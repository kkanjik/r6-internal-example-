#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

// An instance-owned COM dispatch table. The caller retains the COM object and
// quiesces its calls before Restore/destruction; no shared table is modified.
class ShadowVtable {
 void*** object_{};
 void** original_{};
 void** shadow_{};
public:
 struct Patch {std::size_t index;void* replacement;};
 ShadowVtable()=default;
 ShadowVtable(const ShadowVtable&)=delete;
 ShadowVtable& operator=(const ShadowVtable&)=delete;
 ~ShadowVtable(){Restore();}
 bool ActiveFor(void* object)const{return object_==object&&shadow_&&*object_==shadow_;}
 void Install(void* object,std::size_t entries,std::span<const Patch> patches){
  if(object_||!object||!entries||entries>256)throw std::runtime_error("Invalid shadow-vtable installation");
  auto slot=reinterpret_cast<void***>(object);void** original=nullptr;SIZE_T got=0;
  if(!ReadProcessMemory(GetCurrentProcess(),slot,&original,sizeof(original),&got)||got!=sizeof(original))throw std::runtime_error("COM dispatch pointer unreadable");
  auto copy=static_cast<void**>(VirtualAlloc(nullptr,entries*sizeof(void*),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
  if(!copy)throw std::runtime_error("Shadow-vtable allocation failed");
  if(!ReadProcessMemory(GetCurrentProcess(),original,copy,entries*sizeof(void*),&got)||got!=entries*sizeof(void*)){VirtualFree(copy,0,MEM_RELEASE);throw std::runtime_error("COM dispatch table unreadable");}
  for(const auto& patch:patches){if(patch.index>=entries||!patch.replacement){VirtualFree(copy,0,MEM_RELEASE);throw std::runtime_error("Invalid shadow-vtable slot");}copy[patch.index]=patch.replacement;}
  DWORD previous=0;
  if(!VirtualProtect(copy,entries*sizeof(void*),PAGE_READONLY,&previous)){VirtualFree(copy,0,MEM_RELEASE);throw std::runtime_error("Cannot protect shadow-vtable storage");}
  // Exchange only an aligned, writable instance vfptr. Never change the
  // protection or contents of the shared DXGI dispatch table.
  MEMORY_BASIC_INFORMATION memory{};
  const bool queried=VirtualQuery(slot,&memory,sizeof(memory))!=0;
  const auto protection=memory.Protect&0xff;
  if(!queried||memory.State!=MEM_COMMIT||(memory.Protect&PAGE_GUARD)||
     (protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)||
     (reinterpret_cast<std::uintptr_t>(slot)%alignof(void*))!=0){VirtualFree(copy,0,MEM_RELEASE);throw std::runtime_error("COM dispatch pointer is not writable/aligned");}
  if(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(slot),copy,original)!=original){VirtualFree(copy,0,MEM_RELEASE);throw std::runtime_error("COM dispatch pointer changed during installation");}
  object_=slot;original_=original;shadow_=copy;
 }
 // If another owner replaced our vfptr, preserve its pointer and retain our
 // table: that owner may have chained through it. Unload then needs host repair.
 bool Restore(){
  if(!object_)return true;
  if(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(object_),original_,shadow_)!=shadow_)return false;
  VirtualFree(shadow_,0,MEM_RELEASE);object_=nullptr;original_=shadow_=nullptr;return true;
 }
};

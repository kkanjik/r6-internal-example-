#include "skeleton_reader.h"
#include "skeleton_profile.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <set>
namespace game::rig {
std::uint64_t FindInitializedGameTeb(std::uint64_t base,const emulation::SnapshotRead& read){
 // QUERY_INFORMATION only. No contexts, suspension, debugger or target calls.
 struct BasicInfo {LONG status;void* teb;HANDLE pid,tid;ULONG_PTR affinity;LONG priority,basePriority;};
 static_assert(sizeof(BasicInfo)==48);
 using Query=LONG(NTAPI*)(HANDLE,ULONG,void*,ULONG,ULONG*);
 auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationThread"));if(!query)return 0;
 std::uint32_t index=0;if(!read(base+profile::TlsIndex,&index,4)||index>=1088)return 0;
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;
 THREADENTRY32 entry{};entry.dwSize=sizeof(entry);std::set<std::uint64_t> keys;std::uint64_t selected=0;
 for(BOOL ok=Thread32First(snapshot,&entry);ok;ok=Thread32Next(snapshot,&entry)){
  if(entry.th32OwnerProcessID!=GetCurrentProcessId())continue;
  HANDLE thread=OpenThread(THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);if(!thread)continue;
  BasicInfo info{};ULONG returned=0;const auto status=query(thread,0,&info,sizeof(info),&returned);CloseHandle(thread);
  if(status<0||!info.teb||reinterpret_cast<ULONG_PTR>(info.pid)!=GetCurrentProcessId())continue;
  std::uint64_t array=0,tls=0,key=0;const auto teb=reinterpret_cast<std::uint64_t>(info.teb);
  if(!read(teb+profile::TebTls,&array,8)||!read(array+index*8ULL,&tls,8)||!read(tls+profile::AssetKey,&key,8)||!key)continue;
  keys.insert(key);if(!selected)selected=teb;
 }
 CloseHandle(snapshot);return keys.size()==1?selected:0;
}
}

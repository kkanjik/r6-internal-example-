#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
namespace game::pose {
struct JobDecision {std::uint64_t actor=0,nextWake=0;};
inline constexpr std::uint64_t PoseIntervalMs=16;
inline bool ReleaseCapture(std::unordered_map<std::uint64_t,std::uint64_t>& inFlight,std::uint64_t actor,std::uint64_t epoch){
 auto found=inFlight.find(actor);if(found==inFlight.end()||found->second!=epoch)return false;inFlight.erase(found);return true;
}
inline JobDecision PickJob(std::span<const std::uint64_t> jobs,std::unordered_map<std::uint64_t,std::uint64_t>& due,std::size_t& cursor,std::uint64_t now){
 JobDecision result{0,now+PoseIntervalMs};
 for(std::size_t step=0;step<jobs.size();++step){const auto i=(cursor+step)%jobs.size();const auto actor=jobs[i];const auto deadline=due[actor];
  if(deadline<=now){cursor=(i+1)%jobs.size();due[actor]=now+PoseIntervalMs;return {actor,now};}result.nextWake=std::min(result.nextWake,deadline);
 }
 return result;
}
inline bool ScheduleSelfTest(){
 std::unordered_map<std::uint64_t,std::uint64_t> due;std::size_t cursor=0;
 const std::uint64_t two[]={1,2};
 if(PickJob(two,due,cursor,100).actor!=1||PickJob(two,due,cursor,100).actor!=2)return false;
 const auto waiting=PickJob(two,due,cursor,115);if(waiting.actor||waiting.nextWake!=116)return false;
 const std::uint64_t one[]={2};if(PickJob(one,due,cursor,116).actor!=2)return false;
 const std::uint64_t changed[]={3,2};if(PickJob(changed,due,cursor,116).actor!=3)return false;
 std::unordered_map<std::uint64_t,std::uint64_t> running={{1,8}};
 if(ReleaseCapture(running,1,7)||running[1]!=8||!ReleaseCapture(running,1,8)||!running.empty())return false;
 return PickJob({},due,cursor,117).actor==0;
}
}

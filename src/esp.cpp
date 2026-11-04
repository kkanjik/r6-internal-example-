#include "esp.h"
#include "game_data.h"
#include "projection.h"
#include "skeleton.h"
#include "pose_math.h"
#include "imgui.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
extern "C" void OverlaySetFpsLimit(unsigned fps);
extern "C" unsigned OverlayFpsLimit();
namespace {
struct Settings {
 bool enabled=true,boxes=true,distance=true,markers=false,skeletons=true,hideNearCamera=true,smoothPoses=true;
 float maxDistance=250.0f,height=1.8f,rootAboveFeet=0.4f,widthRatio=0.42f;
 ImVec4 color={0.25f,0.9f,1.0f,1.0f};
} settings;
esp::DrawMetrics metrics;
// Two coherent poses are retained per character and blended over the observed
// capture cadence. Timestamps come from the capture clock (capturedMs); the
// render clock selects a short blend window for relative joint offsets. Live
// translation is reapplied independently after the animation blend.
struct PoseTrack {
 bool valid=false;
 std::uint64_t asset=0,stamp=0,arrival=0;
 float window=16.0f;
 game::Vec3 previousRoot{},latestRoot{};
 std::vector<game::Bone> previous,latest;
};
std::unordered_map<std::uint64_t,PoseTrack> tracks;std::uint64_t trackWorld=0;
bool Finite(const game::Vec3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
game::Vec3 Lerp(const game::Vec3& a,const game::Vec3& b,float t){
 // Missing joints use NaNs. Never let an older missing joint poison all future
 // interpolated samples, and never blend an invalid new joint into a valid one.
 if(!Finite(b))return b;if(!Finite(a))return b;
 return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};
}
float Progress(const PoseTrack& track,std::uint64_t now){return track.window>0.0001f?std::clamp(static_cast<float>(game::AgeMs(now,track.arrival))/track.window,0.f,1.f):1.f;}
std::vector<game::Bone> Displayed(const PoseTrack& track,std::uint64_t now,game::Vec3& root){
 if(!track.valid)return {};
 const float t=settings.smoothPoses?Progress(track,now):1.f;
 if(!settings.smoothPoses){root=track.latestRoot;return track.latest;}
 root=Lerp(track.previousRoot,track.latestRoot,t);
 if(track.previous.size()!=track.latest.size())return track.latest;
 auto result=track.latest;
 for(std::size_t i=0;i<result.size();++i)result[i].position=Lerp(track.previous[i].position,result[i].position,t);
 return result;
}
void Reset(PoseTrack& track,const game::Character& actor,std::uint64_t stamp,std::uint64_t now){
 track.valid=true;track.asset=actor.skeleton.asset;track.stamp=stamp;track.arrival=now;
 track.previousRoot=track.latestRoot=actor.position;track.previous=track.latest=actor.bones;track.window=16.0f;
}
void Advance(PoseTrack& track,const game::Character& actor,std::uint64_t stamp,std::uint64_t now){
 game::Vec3 root{};
 auto shown=Displayed(track,now,root);
 const float cadence=stamp>track.stamp?static_cast<float>(stamp-track.stamp):16.0f;
 track.previous=std::move(shown);track.previousRoot=root;
 track.latest=actor.bones;track.latestRoot=actor.position;track.stamp=stamp;track.arrival=now;
 // Blend joint animation briefly without delaying the fast position anchor.
 track.window=std::clamp(cadence*.5f,8.0f,25.0f);
}
// Bounding box taken from the character's own projected joints. Indices follow
// game::pose::Body (pelvis 0, neck 5, head 6, hips 15/19, knees 16/20,
// feet 17/21, toes 18/22). Arms are excluded from the width so an extended
// hand cannot balloon the box.
bool AnchorBox(const std::vector<game::Bone>& bones,const std::array<float,16>& matrix,float width,float height,float& left,float& top,float& right,float& bottom){
 if(bones.size()<23)return false;
 if(!std::all_of(bones.begin(),bones.end(),[](const auto& bone){return Finite(bone.position);}))return false;
 game::ScreenPoint headPoint{},neckPoint{};
 bool haveHead=game::Project(bones[6].position,matrix,width,height,headPoint);
 const bool haveNeck=game::Project(bones[5].position,matrix,width,height,neckPoint);
 if(!haveHead&&!haveNeck)return false;
 const float topY=haveHead?headPoint.y:neckPoint.y;
 float lowest=-1e30f;bool haveFeet=false;
 static const std::size_t feet[]={17,18,21,22};
 for(auto index:feet){game::ScreenPoint p{};if(!game::Project(bones[index].position,matrix,width,height,p))continue;lowest=std::max(lowest,p.y);haveFeet=true;}
 if(!haveFeet)return false;
 if(!(lowest>topY))return false;
 static const std::size_t torso[]={0,1,2,3,4,5,6,15,16,19,20};
 float minX=1e30f,maxX=-1e30f;
 for(auto index:torso){game::ScreenPoint p{};if(!game::Project(bones[index].position,matrix,width,height,p))continue;minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);}
 if(!(maxX>minX))return false;
 const float boxHeight=lowest-topY;
 if(boxHeight<2.0f||boxHeight>height*4.0f)return false;
 const float centre=(minX+maxX)*0.5f;
 const float half=std::max((maxX-minX)*0.5f,boxHeight*settings.widthRatio*0.5f);
 left=centre-half;right=centre+half;top=topY;bottom=lowest;
 return true;
}
}
void esp::Draw(bool menuVisible){
 metrics={};
 const auto shared=game::LatestShared();const auto& frame=*shared;const auto now=GetTickCount64();
 auto viewProjection=frame.viewProjection;auto cameraPosition=frame.cameraPosition;game::RefreshCamera(frame.camera,cameraPosition,viewProjection);
 if(trackWorld!=frame.world){tracks.clear();trackWorld=frame.world;}
 std::unordered_set<std::uint64_t> present;for(const auto& actor:frame.characters)present.insert(actor.id);std::erase_if(tracks,[&](const auto& item){return !present.contains(item.first);});
 if(menuVisible){
  ImGui::SetNextWindowSize(ImVec2(470,420),ImGuiCond_FirstUseEver);
  ImGui::Begin("Offline character overlay");
  ImGui::TextWrapped("%s",frame.status.c_str());
  if(!frame.ready&&frame.cameraScanProgress>0&&frame.cameraScanProgress<1)ImGui::ProgressBar(frame.cameraScanProgress,ImVec2(-1,0),"Camera scan");
  ImGui::Text("Cached characters: %u",static_cast<unsigned>(frame.characters.size()));
  const auto posed=std::count_if(frame.characters.begin(),frame.characters.end(),[](const auto& actor){return !actor.bones.empty();});
  ImGui::Text("Posed characters: %u",static_cast<unsigned>(posed));
  ImGui::TextWrapped("%s",frame.skeletonStatus.c_str());
  ImGui::Checkbox("Enable overlay",&settings.enabled);
  ImGui::Checkbox("Character boxes",&settings.boxes);
  ImGui::Checkbox("Distance labels",&settings.distance);
  ImGui::Checkbox("Skeleton ESP",&settings.skeletons);
  ImGui::Checkbox("Smooth joint animation",&settings.smoothPoses);
  ImGui::Checkbox("Head / feet markers",&settings.markers);
  if(ImGui::Checkbox("Hide near-camera character",&settings.hideNearCamera))game::SetNearCameraSampling(!settings.hideNearCamera);
  ImGui::TextUnformatted("Overlay draws on every presented frame");
  ImGui::SliderFloat("Maximum distance",&settings.maxDistance,10,500,"%.0f m");
  if(ImGui::CollapsingHeader("Box calibration")){
   ImGui::SliderFloat("Minimum width / height",&settings.widthRatio,0.2f,0.8f,"%.2f");
   ImGui::TextWrapped("Boxes and skeletons share a complete pose. Incomplete captures do not switch to a different origin or estimated standing height.");
  }
  ImGui::ColorEdit4("Overlay color",&settings.color.x,ImGuiColorEditFlags_NoInputs);
  if(ImGui::Button("Rescan world and camera"))game::RequestRescan();
  ImGui::Separator();
  ImGui::TextWrapped("Insert toggles this menu. Data checks disable drawing when the world, camera, or build does not validate.");
  if(ImGui::CollapsingHeader("Unresolved features")){
   ImGui::TextWrapped("Skeletons compose native joint transforms through their complete parent chains and character root. Chams and recoil changes remain unavailable.");
  }
  if(ImGui::CollapsingHeader("Diagnostics")){
   ImGui::TextUnformatted("Build: pose-r5 (independent pose workers + fast anchor)");
   ImGui::Text("World: 0x%llX",static_cast<unsigned long long>(frame.world));
   ImGui::Text("Camera block: 0x%llX",static_cast<unsigned long long>(frame.camera));
   ImGui::TextUnformatted("World/task checks use Zydis + Unicorn snapshots; GameManager class identity remains unresolved.");
   ImGui::TextWrapped("Characters come from a World phase cache rooted indirectly in .data; the array itself is on the heap. Coverage of a full EntityList is unverified.");
   if(ImGui::TreeNode("Skeleton reader")){
    for(const auto& actor:frame.characters){
     ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(actor.id)));
     if(ImGui::TreeNode("Character","Character 0x%llX",static_cast<unsigned long long>(actor.id))){
      const auto& s=actor.skeleton;ImGui::TextWrapped("%s",s.status.c_str());
      ImGui::Text("Rig bones: %u; named hashes: %u",s.boneCount,static_cast<unsigned>(s.named.size()));
      ImGui::Text("Component: 0x%llX, slot %u",static_cast<unsigned long long>(s.component),static_cast<unsigned>(s.componentSlot));
      ImGui::Text("Rig: 0x%llX",static_cast<unsigned long long>(s.asset));
      if(s.poseValid){
       ImGui::Text("Captured: %llu ms ago",static_cast<unsigned long long>(game::AgeMs(GetTickCount64(),s.capturedMs)));
       if(ImGui::TreeNode("Named local transforms")){
        for(const auto& b:s.named)if(b.matrixValid)ImGui::Text("%s [%u], parent %u: %.3f %.3f %.3f",b.name.c_str(),static_cast<unsigned>(b.index),static_cast<unsigned>(b.parentIndex),b.matrix[12],b.matrix[13],b.matrix[14]);
        ImGui::TreePop();
       }
      }
      ImGui::TreePop();
     }
     ImGui::PopID();
    }
    ImGui::TreePop();
   }
  }
  ImGui::End();
 }
 if(!settings.enabled||!frame.ready||game::AgeMs(now,frame.capturedMs)>300)return;
 const float frameAlpha=std::clamp((300.f-static_cast<float>(game::AgeMs(now,frame.capturedMs)))/150.f,0.f,1.f);
 auto* draw=ImGui::GetBackgroundDrawList();const auto display=ImGui::GetIO().DisplaySize;

 for(const auto& actor:frame.characters){
  auto& track=tracks[actor.id];
  game::Vec3 root=actor.position;std::vector<game::Bone> bones;
  if(actor.bones.size()==23&&std::all_of(actor.bones.begin(),actor.bones.end(),[](const auto& bone){return Finite(bone.position);})){ 
   if(!track.valid||track.asset!=actor.skeleton.asset||track.latest.size()!=actor.bones.size()||game::Distance(track.latestRoot,actor.position)>5.f)Reset(track,actor,actor.skeleton.capturedMs,now);
   else if(actor.skeleton.capturedMs!=track.stamp)Advance(track,actor,actor.skeleton.capturedMs,now);
   game::Vec3 shownRoot{};bones=Displayed(track,now,shownRoot);root=shownRoot;
  }else if(track.valid&&track.asset==actor.skeleton.asset&&game::AgeMs(now,track.stamp)<=250){
   game::Vec3 shownRoot{};bones=Displayed(track,now,shownRoot);root=shownRoot;
  }else continue;
  // Keep live translation independent of pose timestamps and interpolation.
  // Only relative joint offsets are blended; the anchor follows every snapshot.
  game::pose::RebaseWorld(bones,root,actor.position);root=actor.position;
  auto tint=settings.color;tint.w*=frameAlpha;const auto color=ImGui::ColorConvertFloat4ToU32(tint);
  const float range=game::Distance(root,cameraPosition);
  if(!std::isfinite(range)||range>settings.maxDistance)continue;
  const float dx=root.x-cameraPosition.x,dy=root.y-cameraPosition.y;
  if(settings.hideNearCamera&&dx*dx+dy*dy<0.6f*0.6f&&std::abs(root.z-cameraPosition.z)<3.0f)continue;
  if(settings.skeletons){for(const auto& line:game::ProjectSkeleton(bones,viewProjection,display.x,display.y)){draw->AddLine(ImVec2(line.a.x,line.a.y),ImVec2(line.b.x,line.b.y),color,1.5f);++metrics.segments;}}
  float left=0,right=0,top=0,bottom=0;
  bool anchored=AnchorBox(bones,viewProjection,display.x,display.y,left,top,right,bottom);
  if(!anchored)continue;
  const float boxHeight=bottom-top;
  if(boxHeight<2||boxHeight>display.y*4||!std::isfinite(left)||!std::isfinite(right))continue;
  if(right<0||left>display.x||bottom<0||top>display.y)continue;
  ++metrics.actors;
  if(settings.boxes){draw->AddRect(ImVec2(left,top),ImVec2(right,bottom),IM_COL32(0,0,0,220),0,0,3);draw->AddRect(ImVec2(left,top),ImVec2(right,bottom),color,0,0,1);++metrics.boxes;}
  if(settings.markers){const float cx=(left+right)*0.5f;draw->AddCircleFilled(ImVec2(cx,top),2,color);draw->AddCircleFilled(ImVec2(cx,bottom),2,color);}
  if(settings.distance){char text[32];std::snprintf(text,sizeof(text),"%.0f m",range);auto size=ImGui::CalcTextSize(text);ImVec2 pos((left+right)*0.5f-size.x*.5f,bottom+3);draw->AddText(ImVec2(pos.x+1,pos.y+1),IM_COL32(0,0,0,240),text);draw->AddText(pos,color,text);}
 }
}
esp::DrawMetrics esp::LastDrawMetrics(){return metrics;}

#pragma once
#include "projection.h"
#include <cmath>
#include <span>
namespace game {
struct BoneSegment { ScreenPoint a,b; };
inline bool FinitePosition(const Vec3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline std::vector<BoneSegment> ProjectSkeleton(std::span<const Bone> bones,const std::array<float,16>& matrix,float width,float height){
 std::vector<BoneSegment> result;
 if(bones.empty()||bones.size()>128)return result;
 result.reserve(bones.size());
 const auto count=static_cast<int>(bones.size());
 // Classify every joint once: 0 unvisited, 1 visiting, 2 valid chain, 3 broken/cyclic.
 std::vector<unsigned char> state(bones.size(),0);
 std::vector<int> path;
 auto classify=[&](int index){
  path.clear();int at=index;unsigned char mark=3;
  while(true){
   if(at==-1){mark=2;break;}
   if(at<0||at>=count){mark=3;break;}
   const auto current=state[static_cast<std::size_t>(at)];
   if(current==1){mark=3;break;}        // cycle reached
   if(current==2){mark=2;break;}
   if(current==3){mark=3;break;}
   state[static_cast<std::size_t>(at)]=1;path.push_back(at);
   at=bones[static_cast<std::size_t>(at)].parent;
  }
  for(auto node:path)state[static_cast<std::size_t>(node)]=mark;
 };
 for(int i=0;i<count;++i)if(state[static_cast<std::size_t>(i)]==0)classify(i);
 // Emit only the segments whose endpoints are individually usable. A single
 // bad joint no longer erases the rest of the skeleton.
 for(std::size_t i=0;i<bones.size();++i){
  const auto& bone=bones[i];
  if(bone.parent<0)continue;
  if(state[i]!=2)continue;
  if(bone.parent>=count||state[static_cast<std::size_t>(bone.parent)]!=2)continue;
  if(!FinitePosition(bone.position))continue;
  const auto& parent=bones[static_cast<std::size_t>(bone.parent)];
  if(!FinitePosition(parent.position))continue;
  if(Distance(bone.position,parent.position)>5.0f)continue; // implausible limb: drop one segment
  BoneSegment line{};
  if(ProjectSegment(bone.position,parent.position,matrix,width,height,line.a,line.b))result.push_back(line);
 }
 return result;
}
inline bool SkeletonSelfTest(){
 std::array<float,16> m={1,0,0,0,0,0,1,1,0,1,0,0,0,-1.6f,4.99f,5};
 std::vector<Bone> bones={{{0,0,0},-1},{{0,0,1},0},{{0,0,1.6f},1}};
 if(ProjectSkeleton(bones,m,1280,720).size()!=2)return false;
 bones[0].parent=2;if(!ProjectSkeleton(bones,m,1280,720).empty())return false;
 bones[0].parent=-1;bones[1].parent=99;if(!ProjectSkeleton(bones,m,1280,720).empty())return false;
 bones[1].parent=0;bones[1].position.x=NAN;if(!ProjectSkeleton(bones,m,1280,720).empty())return false;
 // A single implausible limb drops only that segment; the rest survives.
 bones={{{0,0,0},-1},{{0,0,1},0},{{0,0,1.6f},0},{{40,0,1.2f},1}};
 if(ProjectSkeleton(bones,m,1280,720).size()!=2)return false;
 // A limb crossing the near/viewport plane clips rather than disappearing.
 std::array<float,16> perspective={1,0,0,0,0,1,0,0,0,0,1,1,0,0,-.1f,0};ScreenPoint a{},b{};
 if(!ProjectSegment({0,0,-1},{0,0,2},perspective,1280,720,a,b))return false;
 if(ProjectSegment({0,0,-2},{0,0,-1},perspective,1280,720,a,b))return false;
 return a.x>=0&&a.x<=1280&&b.y>=0&&b.y<=720;
}
}

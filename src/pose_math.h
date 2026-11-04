#pragma once
#include "game_data.h"
#include <array>
#include <cmath>
#include <stdexcept>
#include <span>
namespace game::pose {
using Matrix=std::array<float,16>;
inline void RebaseWorld(std::span<Bone> bones,const Vec3& from,const Vec3& to){
 const Vec3 delta{to.x-from.x,to.y-from.y,to.z-from.z};
 for(auto& bone:bones){bone.position.x+=delta.x;bone.position.y+=delta.y;bone.position.z+=delta.z;}
}
inline Matrix Multiply(const Matrix& local,const Matrix& parent){
 Matrix result{};for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)for(unsigned k=0;k<4;++k)result[row*4+col]+=local[row*4+k]*parent[k*4+col];return result;
}
inline std::vector<Matrix> Compose(std::span<const Matrix> locals,std::span<const std::uint16_t> parents){
 if(locals.empty()||locals.size()!=parents.size()||locals.size()>1024)throw std::runtime_error("Invalid pose hierarchy size");
 std::vector<Matrix> model(locals.size());std::vector<unsigned char> state(locals.size());
 auto visit=[&](auto&& self,std::size_t index)->void{
  if(state[index]==2)return;if(state[index]==1)throw std::runtime_error("Cyclic pose hierarchy");state[index]=1;
  if(parents[index]==0xFFFF)model[index]=locals[index];
  else{if(parents[index]>=locals.size())throw std::runtime_error("Pose parent out of bounds");self(self,parents[index]);model[index]=Multiply(locals[index],model[parents[index]]);}
  state[index]=2;
 };
 for(std::size_t i=0;i<locals.size();++i)visit(visit,i);return model;
}
// Visible body links use supplied names; model positions still compose through
// every native parent, including unnamed intermediate joints.
struct Joint {std::uint32_t hash;int displayParent;};
inline constexpr std::array<Joint,23> Body={{
 {0xDED10611,-1},{0x530EC1CB,0},{0x8F39FA4E,1},{0x1630ABF4,2},
 {0x8023796D,3},{0x07C159A2,4},{0xA9CEFD4A,5},
 {0xF60647E5,3},{0x6BB3F727,7},{0x7257A1AA,8},{0x75F94D30,9},
 {0x2D4660A8,3},{0xEB830ADA,11},{0x89B93A80,12},{0xB675F36C,13},
 {0x757F1291,0},{0x863D09FC,15},{0x9B14362C,16},{0x42BE0FCB,17},
 {0x176183F0,0},{0x060DF401,19},{0x58988870,20},{0xB95094E1,21}
}};
inline bool SelfTest(){
 std::array<Bone,2> moving={Bone{{5,6,8},-1},Bone{{5,6,9},0}};
 RebaseWorld(moving,{5,6,7},{10,20,30});
 if(moving[0].position.x!=10||moving[0].position.y!=20||moving[0].position.z!=31||moving[1].position.z!=32)return false;
 const Matrix identity={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 auto root=identity;root[12]=10;root[13]=20;root[14]=30;
 auto child=identity;child[12]=2;auto grandchild=identity;grandchild[14]=3;
 const std::array<Matrix,3> locals={identity,child,grandchild};const std::array<std::uint16_t,3> parents={0xFFFF,0,1};
 const auto model=Compose(locals,parents);const auto world=Multiply(model[2],root);
 if(std::abs(world[12]-12)>0.0001f||std::abs(world[13]-20)>0.0001f||std::abs(world[14]-33)>0.0001f)return false;
 auto rotated=identity;rotated[0]=0;rotated[1]=1;rotated[4]=-1;rotated[5]=0;
 const auto turned=Multiply(child,rotated);if(std::abs(turned[12])>0.0001f||std::abs(turned[13]-2)>0.0001f)return false;
 auto cyclic=parents;cyclic[0]=2;try{Compose(locals,cyclic);return false;}catch(const std::runtime_error&){}
 return true;
}
}

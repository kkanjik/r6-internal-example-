#pragma once
#include "game_data.h"
#include <algorithm>
#include <cmath>
namespace game {
struct ScreenPoint { float x{},y{},depth{}; };
inline bool Project(const Vec3& point,const std::array<float,16>& m,float width,float height,ScreenPoint& out) {
 if(!(width>0&&height>0))return false;
 const float x=point.x*m[0]+point.y*m[4]+point.z*m[8]+m[12];
 const float y=point.x*m[1]+point.y*m[5]+point.z*m[9]+m[13];
 const float z=point.x*m[2]+point.y*m[6]+point.z*m[10]+m[14];
 const float w=point.x*m[3]+point.y*m[7]+point.z*m[11]+m[15];
 if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(w)||w<=0.05f)return false;
 out={(0.5f+0.5f*x/w)*width,(0.5f-0.5f*y/w)*height,z/w};
 return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.depth)&&out.depth>=-0.01f&&out.depth<=1.01f;
}
inline float Distance(const Vec3& a,const Vec3& b){const auto x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return std::sqrt(x*x+y*y+z*z);}
inline bool ProjectSegment(const Vec3& a,const Vec3& b,const std::array<float,16>& m,float width,float height,ScreenPoint& outA,ScreenPoint& outB){
 if(!(width>0&&height>0))return false;
 auto clip=[&](const Vec3& p){return std::array<float,4>{p.x*m[0]+p.y*m[4]+p.z*m[8]+m[12],p.x*m[1]+p.y*m[5]+p.z*m[9]+m[13],p.x*m[2]+p.y*m[6]+p.z*m[10]+m[14],p.x*m[3]+p.y*m[7]+p.z*m[11]+m[15]};};
 auto x=clip(a),y=clip(b);for(float v:x)if(!std::isfinite(v))return false;for(float v:y)if(!std::isfinite(v))return false;
 auto plane=[](const std::array<float,4>& p,unsigned i){switch(i){case 0:return p[3]-0.05f;case 1:return p[0]+p[3];case 2:return p[3]-p[0];case 3:return p[1]+p[3];case 4:return p[3]-p[1];case 5:return p[2];default:return p[3]-p[2];}};
 for(unsigned i=0;i<7;++i){const float dx=plane(x,i),dy=plane(y,i);if(dx<0&&dy<0)return false;if((dx<0)!=(dy<0)){const float t=dx/(dx-dy);std::array<float,4> p{};for(unsigned k=0;k<4;++k)p[k]=x[k]+(y[k]-x[k])*t;if(dx<0)x=p;else y=p;}}
 if(x[3]<=0||y[3]<=0)return false;
 outA={(.5f+.5f*x[0]/x[3])*width,(.5f-.5f*x[1]/x[3])*height,x[2]/x[3]};outB={(.5f+.5f*y[0]/y[3])*width,(.5f-.5f*y[1]/y[3])*height,y[2]/y[3]};return true;
}
}

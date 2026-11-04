#include "skeleton_reader.h"
#include "skeleton_profile.h"
#include "named_bones.h"
#include "pose_math.h"
#include <Windows.h>
#include <limits>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <bit>
namespace game::rig { namespace {
using Address=std::uint64_t;
bool PointerLike(Address value){return value>=0x100000000ULL&&value<0x700000000000ULL&&(value&7)==0;}
template<class T>T Load(const emulation::SnapshotRead& read,Address address){
 T value{};if(!read(address,&value,sizeof(value)))throw std::runtime_error("Skeleton source became unreadable");return value;
}
template<class T>std::vector<T> Array(const emulation::SnapshotRead& read,Address address,std::size_t count){
 if(!PointerLike(address)||!count||count>profile::MaxBones)throw std::runtime_error("Invalid skeleton array");
 std::vector<T> result(count);if(!read(address,result.data(),count*sizeof(T)))throw std::runtime_error("Skeleton array became unreadable");return result;
}
void Type(const emulation::SnapshotRead& read,Address object,Address expected){
 if(!PointerLike(object)||Load<Address>(read,object)!=expected)throw std::runtime_error("Skeleton object type changed");
}
struct MapRecord {std::uint32_t hash;std::uint16_t index,padding;};
static_assert(sizeof(MapRecord)==8);
struct HierarchyRecord {std::array<std::uint8_t,28> bytes;};
template<class T>T Field(const HierarchyRecord& r,std::size_t offset){T result;std::memcpy(&result,r.bytes.data()+offset,sizeof(result));return result;}
struct Rig {
 std::vector<MapRecord> map;std::vector<std::uint32_t> indexed;std::vector<std::uint16_t> parents;
};
bool DirectLocalRoot(const emulation::SnapshotRead& read,Address base,Address character,Address teb,pose::Matrix& matrix){
 const auto reference=character+profile::RootReference;
 if(Load<Address>(read,reference+0x2E0))return false; // Native helper's direct branch only.
 const auto tlsIndex=Load<std::uint32_t>(read,base+profile::TlsIndex);
 if(tlsIndex>=1088||!teb)return false;
 const auto tls=Load<Address>(read,Load<Address>(read,teb+profile::TebTls)+tlsIndex*8ULL);
 constexpr auto lookupOffset=0ULL-(0x3DE80B9ABF238F75ULL^0x6E759ED003CFA3E8ULL);
 const auto lookup=Load<Address>(read,base+0x10D6C128)+lookupOffset;
 const auto rotation=static_cast<int>(Load<Address>(read,lookup)&63);
 const auto key=Load<Address>(read,tls+0x9408);
 constexpr std::array<Address,8> before={0x5769AC682E27BBBCULL,0x1493455B715A0E8EULL,0xD1BCDE4EB48C6160ULL,0x8EE67741F7BEB432ULL,0x4C1010353AF10704ULL,0x0939A9287E2359D6ULL,0xC663421BC155ACA8ULL,0x838CDB0F0487FF7AULL};
 constexpr std::array<Address,8> after={0,0xAA8A1C9CB5F2D4B3ULL,0x551439396BE5A966ULL,0xFF9E55D621D87E19ULL,0xAA287272D7CB52CCULL,0x54B28F0F8DBE277FULL,0xFF3CABAC43B0FC32ULL,0xA9C6C848F9A3D0E5ULL};
 auto words=Load<std::array<Address,8>>(read,reference+0x90);
 for(std::size_t i=0;i<words.size();++i)words[i]=std::rotr(words[i]+before[i],rotation)+key+after[i];
 std::memcpy(matrix.data(),words.data(),sizeof(matrix));
 return !Load<Address>(read,reference+0x2E0)&&RigidLocalMatrix(matrix);
}
Rig ReadRig(const emulation::SnapshotRead& read,Address base,Address asset){
 Type(read,asset,base+profile::RigTable);
 const auto count=Load<std::uint32_t>(read,asset+profile::MapCount)&profile::CountMask;
 if(!count||count>profile::MaxBones||(Load<std::uint32_t>(read,asset+profile::HierarchyCount)&profile::CountMask)!=count||(Load<std::uint32_t>(read,asset+profile::IndexedCount)&profile::CountMask)!=count)throw std::runtime_error("Rig counts disagree");
 Rig rig;rig.map=Array<MapRecord>(read,Load<Address>(read,asset+profile::Map),count);
 rig.indexed=Array<std::uint32_t>(read,Load<Address>(read,asset+profile::IndexedHashes),count);
 const auto hierarchy=Array<HierarchyRecord>(read,Load<Address>(read,asset+profile::Hierarchy),count);
 for(std::size_t i=0;i<count;++i){const auto& record=rig.map[i];
  if(record.index>=count||rig.indexed[record.index]!=record.hash||(i&&rig.map[i-1].hash>=record.hash))throw std::runtime_error("Rig hash map does not validate");
  const auto parent=Field<std::uint16_t>(hierarchy[i],0);const auto parentHash=Field<std::uint32_t>(hierarchy[i],24);
  if(Field<std::uint32_t>(hierarchy[i],20)!=rig.indexed[i]||!((parent==0xFFFF&&parentHash==0xFFFFFFFF)||(parent<count&&rig.indexed[parent]==parentHash)))throw std::runtime_error("Rig hierarchy does not validate");
  rig.parents.push_back(parent);
 }
 for(std::size_t i=0;i<count;++i){auto at=static_cast<std::uint16_t>(i);std::size_t hops=0;
  while(at!=0xFFFF){if(at>=count||++hops>count)throw std::runtime_error("Rig hierarchy is cyclic");at=rig.parents[at];}
 }
 return rig;
}
}
bool RigidLocalMatrix(const std::array<float,16>& m){
 if(!std::all_of(m.begin(),m.end(),[](float v){return std::isfinite(v);}))return false;
 if(std::max({std::abs(m[3]),std::abs(m[7]),std::abs(m[11]),std::abs(m[15]-1)})>0.001f)return false;
 for(unsigned i=0;i<3;++i)for(unsigned j=i;j<3;++j){float dot=0;for(unsigned k=0;k<3;++k)dot+=m[i*4+k]*m[j*4+k];if(std::abs(dot-(i==j?1.0f:0.0f))>=0.03f)return false;}
 return true;
}
Reader::Reader(Address base,Address accessor,emulation::SnapshotRead read,std::span<const emulation::CodeRange> code):base_(base),accessor_(accessor),read_(std::move(read)),code_(code.begin(),code.end()){
 if(accessor_!=base_+profile::MatrixAccessor||!read_)throw std::runtime_error("Skeleton accessor differs from validated build");
 const auto nt=Load<std::uint32_t>(read_,base_+0x3C);
 if(Load<std::uint16_t>(read_,base_)!=0x5A4D||nt>0x100000||Load<std::uint32_t>(read_,base_+nt)!=0x4550||Load<std::uint32_t>(read_,base_+nt+8)!=profile::Timestamp||Load<std::uint32_t>(read_,base_+nt+80)!=profile::ImageSize)throw std::runtime_error("Skeleton layout profile does not match PE build");
 const auto sectionCount=Load<std::uint16_t>(read_,base_+nt+6),optionalSize=Load<std::uint16_t>(read_,base_+nt+20);
 if(sectionCount>96)throw std::runtime_error("Invalid executable section count");
 for(unsigned i=0;i<sectionCount;++i){
  const auto section=Load<IMAGE_SECTION_HEADER>(read_,base_+nt+24+optionalSize+i*sizeof(IMAGE_SECTION_HEADER));
  if(static_cast<std::uint64_t>(section.VirtualAddress)+section.Misc.VirtualSize>profile::ImageSize)throw std::runtime_error("Executable section outside image");
  if(section.Characteristics&IMAGE_SCN_MEM_EXECUTE)code_.push_back({base_+section.VirtualAddress,section.Misc.VirtualSize});
 }
 std::array<unsigned char,5> call{};if(!read_(base_+profile::MatrixCall,call.data(),call.size())||call[0]!=0xE8)throw std::runtime_error("Bone accessor return context changed");
 std::int32_t displacement=0;std::memcpy(&displacement,call.data()+1,4);
 if(base_+profile::MatrixReturn+displacement!=accessor_)throw std::runtime_error("Bone accessor caller disagrees with signature");
 if(!read_(base_+profile::RootCall,call.data(),call.size())||call[0]!=0xE8)throw std::runtime_error("Character-root caller changed");
 std::memcpy(&displacement,call.data()+1,4);if(base_+profile::RootReturn+displacement!=base_+profile::RootAccessor)throw std::runtime_error("Character-root accessor disagrees with caller");
 const std::array<std::uint8_t,12> streamCode={0x44,0x0f,0xb7,0xda,0x49,0xc1,0xe3,0x05,0x47,0x0f,0x28,0x3c};
 std::array<std::uint8_t,12> liveStreamCode{};
 if(!read_(base_+0x00456E86,liveStreamCode.data(),liveStreamCode.size())||liveStreamCode!=streamCode)throw std::runtime_error("Pose stream snapshot checkpoint changed");
 std::array<std::uint8_t,0x1C5> directCode{};
 if(read_(base_+0x0442F941,directCode.data(),directCode.size())){std::uint64_t hash=0xCBF29CE484222325ULL;for(auto byte:directCode)hash=(hash^byte)*0x100000001B3ULL;directRootVerified_=hash==0x2602867CA38C5E9EULL;}
 // The native root accessor uses a copied ntdll SRW-lock helper.
 const auto ntbase=reinterpret_cast<Address>(GetModuleHandleW(L"ntdll.dll"));
 if(ntbase){
  IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 headers{};
  if(read_(ntbase,&dos,sizeof(dos))&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<0x100000&&read_(ntbase+dos.e_lfanew,&headers,sizeof(headers))&&headers.Signature==IMAGE_NT_SIGNATURE&&headers.FileHeader.NumberOfSections<96){
   const auto sections=ntbase+dos.e_lfanew+24+headers.FileHeader.SizeOfOptionalHeader;
   for(unsigned i=0;i<headers.FileHeader.NumberOfSections;++i){IMAGE_SECTION_HEADER section{};if(read_(sections+i*sizeof(section),&section,sizeof(section))&&(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))code_.push_back({ntbase+section.VirtualAddress,section.Misc.VirtualSize});}
  }
 }

}
LocalSkeleton Reader::Capture(Address character,Address teb,bool localPose,std::stop_token stop,bool bodyOnly)const{
 LocalSkeleton result;result.status.clear();
 try{
  if(stop.stop_requested())throw std::runtime_error("Skeleton capture cancelled");
  Type(read_,character,base_+profile::CharacterTable);
  const auto anchorBefore=Load<Vec3>(read_,character+profile::CharacterPosition);
  const auto encodedOwner=Load<Address>(read_,character+profile::CharacterOwner);
  const auto owner=(encodedOwner^profile::OwnerXor)+profile::OwnerAdd;
  Type(read_,owner,base_+profile::OwnerTable);
  const auto slot=Load<std::uint8_t>(read_,owner+profile::SkeletonSlot);
  const auto componentCount=Load<std::uint32_t>(read_,owner+profile::ComponentCount)&profile::CountMask;
  if(slot>=componentCount||componentCount>profile::MaxComponents)throw std::runtime_error("Skeleton component index exceeds collection");
  const auto components=Load<Address>(read_,owner+profile::Components);
  if(!PointerLike(components))throw std::runtime_error("Invalid component collection");
  const auto component=Load<Address>(read_,components+slot*8ULL);Type(read_,component,base_+profile::SkeletonTable);
  const auto tlsIndex=Load<std::uint32_t>(read_,base_+profile::TlsIndex);
  if(tlsIndex>=1088||!teb)throw std::runtime_error("Initialized game TLS unavailable");
  const auto tls=Load<Address>(read_,Load<Address>(read_,teb+profile::TebTls)+tlsIndex*8ULL);
  const auto key=Load<Address>(read_,tls+profile::AssetKey);if(!key)throw std::runtime_error("Game TLS decoder unavailable");
  const auto encodedAsset=Load<Address>(read_,component+profile::AssetField);
  const auto asset=(encodedAsset+profile::AssetAdd)^key;
  const auto rig=ReadRig(read_,base_,asset);
  const auto references=Load<Address>(read_,component+profile::References);
  const auto referenceCount=Load<std::uint32_t>(read_,component+profile::ReferenceCount)&profile::CountMask;
  if(referenceCount!=rig.indexed.size()||!PointerLike(references))throw std::runtime_error("Bone references disagree with rig");
  const auto encodedRuntime=Load<Address>(read_,component+profile::RuntimeField);
  const auto runtime=(encodedRuntime^profile::RuntimeXor)+profile::RuntimeAdd;
  if(!PointerLike(runtime))throw std::runtime_error("Runtime pose owner does not validate");
  result.owner=owner;result.component=component;result.componentSlot=slot;result.asset=asset;result.runtime=runtime;result.references=references;result.boneCount=referenceCount;result.parents=rig.parents;
  bool valid=true;std::set<std::uint16_t> needed;std::string firstError;
  for(const auto& name:supplied_bones::named){
   if(stop.stop_requested())throw std::runtime_error("Skeleton capture cancelled");
   const auto found=std::lower_bound(rig.map.begin(),rig.map.end(),name.hash,[](const auto& record,std::uint32_t hash){return record.hash<hash;});
   if(found==rig.map.end()||found->hash!=name.hash)throw std::runtime_error("Required named hash absent from rig");
   LocalBone bone;bone.hash=name.hash;bone.name=name.name;bone.index=found->index;bone.parentIndex=rig.parents[found->index];
   if(localPose&&(!bodyOnly||std::any_of(pose::Body.begin(),pose::Body.end(),[&](const auto& joint){return joint.hash==name.hash;})))for(auto at=found->index;at!=0xFFFF;at=rig.parents[at])needed.insert(at);
   result.named.push_back(std::move(bone));
  }
  if(localPose){
   std::vector<emulation::MatrixRequest> requests;
   for(auto index:needed){
    emulation::MatrixRequest request{accessor_,base_+profile::MatrixReturn,references+index*profile::ReferenceStride,teb};
    request.snapshotCheckpoint=base_+0x00456E86;request.snapshotBytesFromR8=referenceCount*profile::PoseStride;
    requests.push_back(request);
   }
   const auto matrices=emulation::RunCopiedMatrices(requests,read_,code_,stop);
   const emulation::MatrixRequest rootRequest{base_+profile::RootAccessor,base_+profile::RootReturn,character+profile::RootReference,teb};
   emulation::MatrixResult nativeRoot;
   if(directRootVerified_)nativeRoot.ok=DirectLocalRoot(read_,base_,character,teb,nativeRoot.matrix);
   if(!nativeRoot.ok)nativeRoot=emulation::RunCopiedMatrix(rootRequest,read_,code_,stop);
   // A copied contended lock cannot make progress. Retry a bounded fresh copy;
   // never spin on that frozen lock or modify the live lock to force success.
   for(unsigned retry=0;retry<2&&!nativeRoot.ok&&!stop.stop_requested();++retry)nativeRoot=emulation::RunCopiedMatrix(rootRequest,read_,code_,stop);
   const pose::Matrix identity={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
   std::vector<pose::Matrix> locals(referenceCount,identity);
   std::vector<unsigned char> jointValid(referenceCount,1);
   std::size_t at=0;
   for(auto index:needed){
    const auto& matrix=matrices[at++];
    if(!matrix.ok||!RigidLocalMatrix(matrix.matrix)){jointValid[index]=0;valid=false;if(firstError.empty())firstError=matrix.error.empty()?"Non-rigid bone matrix":matrix.error;continue;}
    locals[index]=matrix.matrix;
   }
   for(auto& bone:result.named){bone.matrix=locals[bone.index];bone.matrixValid=jointValid[bone.index]!=0&&needed.contains(bone.index);}
   if(!nativeRoot.ok)result.status="Native character root: "+nativeRoot.error;
   else if(!RigidLocalMatrix(nativeRoot.matrix))result.status="Native character root is not rigid";
   const bool rootValid=nativeRoot.ok&&RigidLocalMatrix(nativeRoot.matrix);
   if(rootValid){
    result.rootMatrix=nativeRoot.matrix;
    result.nativeRootPosition={result.rootMatrix[12],result.rootMatrix[13],result.rootMatrix[14]};
    const auto anchor=Load<Vec3>(read_,character+profile::CharacterPosition);
    auto goodPosition=[](const Vec3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})<1000000;};
    const float movedX=anchor.x-anchorBefore.x,movedY=anchor.y-anchorBefore.y,movedZ=anchor.z-anchorBefore.z;
    if(!goodPosition(anchorBefore)||!goodPosition(anchor)||movedX*movedX+movedY*movedY+movedZ*movedZ>4)throw std::runtime_error("Character position changed discontinuously during pose capture");
    // The copied native getter can return a rigid matrix with a wrong/stale
    // translation. Movement capture corroborated this position against camera
    // motion; use it consistently instead of alternating native/cached origins.
    result.rootMatrix[12]=anchor.x;result.rootMatrix[13]=anchor.y;result.rootMatrix[14]=anchor.z;
    result.anchorCorrectionMeters=std::hypot(static_cast<double>(result.nativeRootPosition.x)-anchor.x,static_cast<double>(result.nativeRootPosition.y)-anchor.y,static_cast<double>(result.nativeRootPosition.z)-anchor.z);
    try{
     const auto models=pose::Compose(locals,rig.parents);
     auto ancestryValid=[&](std::uint16_t index){std::size_t hops=0;for(auto node=index;node!=0xFFFF;node=rig.parents[node]){if(!jointValid[node])return false;if(++hops>rig.parents.size())return false;}return true;};
     const Vec3 missing{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::quiet_NaN()};
     bool partial=false;
     for(const auto& joint:pose::Body){
      const auto found=std::lower_bound(rig.map.begin(),rig.map.end(),joint.hash,[](const auto& record,std::uint32_t hash){return record.hash<hash;});
      if(found==rig.map.end()||found->hash!=joint.hash)throw std::runtime_error("Display joint absent");
      if(!ancestryValid(found->index)){result.worldBones.push_back({missing,joint.displayParent});partial=true;continue;}
      const auto world=pose::Multiply(models[found->index],result.rootMatrix);
      const Vec3 point{world[12],world[13],world[14]};
      const float dx=point.x-result.rootMatrix[12],dy=point.y-result.rootMatrix[13],dz=point.z-result.rootMatrix[14];
      if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z)||dx*dx+dy*dy+dz*dz>25){result.worldBones.push_back({missing,joint.displayParent});partial=true;continue;}
      if(joint.displayParent>=0){
       const auto& parent=result.worldBones[joint.displayParent].position;
       if(std::isfinite(parent.x)&&std::isfinite(parent.y)&&std::isfinite(parent.z)){
        const auto x=point.x-parent.x,y=point.y-parent.y,z=point.z-parent.z;
        if(x*x+y*y+z*z>2.25f){result.worldBones.push_back({missing,joint.displayParent});partial=true;continue;}
       }
      }
      result.worldBones.push_back({point,joint.displayParent});
     }
     // A correctly sized vector can contain only withheld (NaN) joints.
     // Do not let that empty pose replace the sampler's last renderable sample.
     result.worldValid=!partial&&result.worldBones.size()==pose::Body.size();
     if(!result.worldValid)result.status="Incomplete pose withheld; awaiting a coherent body sample";
     if(partial&&result.status.empty())result.status="Partial pose: "+(firstError.empty()?std::string("some joints withheld"):firstError);
    }catch(const std::exception& error){result.worldValid=false;result.worldBones.clear();result.status=error.what();}
   }
  }
  if(Load<Address>(read_,character+profile::CharacterOwner)!=encodedOwner||Load<Address>(read_,component+profile::AssetField)!=encodedAsset||Load<Address>(read_,component+profile::RuntimeField)!=encodedRuntime||Load<Address>(read_,component+profile::References)!=references||Load<Address>(read_,owner+profile::Components)!=components||Load<Address>(read_,components+slot*8ULL)!=component)throw std::runtime_error("Skeleton chain changed during capture");
  result.chainValid=true;result.poseValid=localPose&&valid&&result.worldValid;
  if(!localPose)result.status="55 named hashes and hierarchy validated";
  else if(!valid&&result.status.empty())result.status="Local pose partially rejected: "+(firstError.empty()?std::string("non-rigid or torn snapshot"):firstError);
  else if(result.worldValid&&result.status.empty())result.status="World skeleton composed through native parents and character root";
 }catch(const std::exception& error){result.chainValid=false;result.poseValid=false;result.worldValid=false;result.worldBones.clear();result.named.clear();result.status=error.what();}
 return result;
}
bool SelfTest(){
 const std::array<float,16> identity={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 if(!RigidLocalMatrix(identity))return false;auto bad=identity;bad[0]=93;if(RigidLocalMatrix(bad))return false;bad=identity;bad[1]=0.5f;return !RigidLocalMatrix(bad)&&supplied_bones::named.size()==55&&pose::SelfTest();
}
}

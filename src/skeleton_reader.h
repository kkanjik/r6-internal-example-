#pragma once
#include "game_data.h"
#include "copied_emulation.h"
namespace game::rig {
// All source access is supplied by the caller; useful for copied-page validation.
class Reader {
 std::uint64_t base_,accessor_; emulation::SnapshotRead read_;
 std::vector<emulation::CodeRange> code_;
 bool directRootVerified_=false;
public:
 Reader(std::uint64_t base,std::uint64_t accessor,emulation::SnapshotRead,
        std::span<const emulation::CodeRange>);
 LocalSkeleton Capture(std::uint64_t character,std::uint64_t teb,bool localPose,
                       std::stop_token={},bool bodyOnly=false)const;
};
std::uint64_t FindInitializedGameTeb(std::uint64_t base,const emulation::SnapshotRead&);
bool RigidLocalMatrix(const std::array<float,16>&);
bool SelfTest();
}

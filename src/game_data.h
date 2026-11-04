#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
namespace game {
struct Vec3 { float x{},y{},z{}; };
// Monotonic age that tolerates a producer timestamp newer than the consumer's
// clock sample (cross-thread capture). Unsigned subtraction must never wrap.
inline std::uint64_t AgeMs(std::uint64_t now,std::uint64_t then){return now>=then?now-then:0;}
struct Bone { Vec3 position{}; int parent=-1; };
struct LocalBone {
 std::uint32_t hash{}; std::string name; std::uint16_t index{},parentIndex=0xFFFF;
 std::array<float,16> matrix{};
 bool matrixValid=false;
};
struct LocalSkeleton {
 bool chainValid=false,poseValid=false,worldValid=false; std::string status="Not sampled";
 std::uint64_t component{},owner{},asset{},runtime{},references{},capturedMs{},poseMs{};
 std::uint32_t boneCount{}; std::uint8_t componentSlot{};
 // Parent indices refer to this complete rig, not the 55-element named vector.
 std::vector<std::uint16_t> parents;
 std::vector<LocalBone> named;
 std::vector<Bone> worldBones;
 std::array<float,16> rootMatrix{};
 Vec3 nativeRootPosition{};double anchorCorrectionMeters=0;
 std::uint64_t captureDurationMs=0;
};
struct Character {
 std::uint64_t id{}; Vec3 position{};
 // Only independently validated world-space positions may enter the renderer.
 std::vector<Bone> bones;
 LocalSkeleton skeleton;
};
struct Snapshot {
 bool ready=false; std::string status="Waiting for render initialization";
 float cameraScanProgress=0; std::uint64_t capturedMs=0,world=0,camera=0;
 Vec3 cameraPosition{}; std::array<float,16> viewProjection{};
 std::vector<Character> characters;
 std::string skeletonStatus="Skeleton reader not initialized";
 unsigned scheduledPoses=0;
 unsigned poseWorkers=1;
};
void Start();
void Stop();
void RequestRescan();
Snapshot Latest();
std::shared_ptr<const Snapshot> LatestShared();
bool RefreshCamera(std::uint64_t camera,Vec3&,std::array<float,16>&);
void SetNearCameraSampling(bool enabled);
bool SelfTest();
#ifdef OVERLAY_ENABLE_TEST_EXPORTS
void SetValidationSnapshot(const Snapshot* snapshot);
#endif
}

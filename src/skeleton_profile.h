#pragma once
#include <cstdint>
namespace game::rig::profile {
inline constexpr std::uint32_t Timestamp=0x3B7DCD86, ImageSize=0x19528000;
inline constexpr std::uint64_t RootSlot=0x10F8BC10, WorldFromRoot=0xD0;
inline constexpr std::uint64_t Phase5Array=0x280, Phase5Count=0x288;
inline constexpr std::uint64_t CharacterTable=0x10301300, OwnerTable=0x102F2680;
inline constexpr std::uint64_t SkeletonTable=0x105F18E0, RigTable=0x1059B230;
inline constexpr std::uint64_t CharacterOwner=0x18, Components=0xD8, ComponentCount=0xE0, SkeletonSlot=0x1FE;
inline constexpr std::uint64_t CharacterPosition=0xA70;
inline constexpr std::uint64_t OwnerXor=0x56F89F2CFFCC9559ULL, OwnerAdd=0x47C23D0FEDD0C1A7ULL;
inline constexpr std::uint64_t AssetField=0xE8, AssetAdd=0x88649178A8C640EDULL;
inline constexpr std::uint64_t TlsIndex=0x10ED0EDC, TebTls=0x58, AssetKey=0x135A0;
inline constexpr std::uint64_t Map=0x50, MapCount=0x58, Hierarchy=0x70, HierarchyCount=0x78, IndexedHashes=0xA0, IndexedCount=0xA8;
inline constexpr std::uint64_t References=0xD0, ReferenceCount=0xD8, ReferenceStride=0x30;
inline constexpr std::uint64_t RuntimeField=0x118, RuntimeXor=0x17A38EFB4C5AB505ULL, RuntimeAdd=0x661FD77EA283BC18ULL;
inline constexpr std::uint64_t RuntimeStream=0x100, RuntimeFlags=0x110, PoseStride=0x20;
inline constexpr std::uint64_t MatrixAccessor=0x00456750, MatrixCall=0x01E104C2, MatrixReturn=0x01E104C7;
inline constexpr std::uint64_t RootAccessor=0x0442F820, RootCall=0x04045060, RootReturn=0x04045065, RootReference=0xB0;
inline constexpr std::uint32_t CountMask=0x3FFFFFFF;
inline constexpr unsigned MaxBones=1024, MaxComponents=128;
// RuntimeStream and PoseStride describe encrypted storage, not plain float arrays.
}

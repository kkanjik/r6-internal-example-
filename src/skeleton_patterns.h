#pragma once
// Current-build patterns: live bytes and .text uniqueness verified by Python/Capstone.
namespace game::patterns {
inline constexpr char bone_local_matrix_accessor[] = "41 57 41 54 56 55 41 56 57 53 41 55 48 81 EC D8 00 00 00 44 0F 29 B4 24 B0 00 00 00 44 0F 29 AC 24 A0 00 00 00 44 0F 29 9C 24 80 00 00 00 44 0F 29 44 24 60 44 0F 29 4C 24 70 44 0F 29 BC 24 C0 00 00 00 0F 29 74 24 40 0F 29 7C 24 50 44 0F 29 A4 24 90 00 00 00 49 89 CE 89 E1 49 89 E0 49 89 E3 49 89 E1 48 B8 BD 21 63 57 FF FF FF FF 49 BC 96 A3 20 83 3E 65 E6 7C";
inline constexpr char owner_skeleton_slot[] = "48 8D 15 ?? ?? ?? ?? 49 01 D0 4C 01 C9 49 31 CA 45 0F B6 9A FE 01 00 00 49 81 FB FF 00 00 00 0F 85 ?? ?? ?? ?? 49 89 DD 41 BE 6F 7D 80 80 48 89 E2 49 89 E1";
inline constexpr char runtime_pose_owner_decoder[] = "49 89 E7 49 BA 05 B5 5A 4C FB 8E A3 17 4D 33 90 18 01 00 00 4C 63 05 ?? ?? ?? ?? 4C 8D 25 ?? ?? ?? ?? 47 8B 44 20 50 48 21 E7";
inline constexpr char rig_asset_decoder[] = "4C 8B 82 E8 00 00 00 4D 01 C8 44 8B 1D ?? ?? ?? ?? 65 48 8B 0C 25 58 00 00 00 4A 8B 0C D9 48 8B 89 A0 35 01 00 4C 31 C1 44 8B 51 58 41 C1 E2 03 48 8B 49 50 49 89 C8 45 85 D2 48 8B 44 24 30 49 BF 90 82 7F 7F FF FF FF FF 4C 89 ED 0F 85 ?? ?? ?? ?? 66 BA FF FF 89 94 24 80 00 00 00 4C 01 D1 49 39 C8 0F 85 ?? ?? ?? ?? 49 89 C5 E9 ?? ?? ?? ?? 44 8B 1D ?? ?? ?? ??";
inline constexpr char character_root_accessor[] = "41 54 41 56 41 55 41 57 56 53 55 57 48 83 EC 78 44 0F 29 6C 24 50 44 0F 29 54 24 40 44 0F 29 74 24 60 0F 29 74 24 30 49 89 D5 49 89 CC E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 B8 ?? ?? ?? ?? ?? ?? ?? ?? 48 BB ?? ?? ?? ?? ?? ?? ?? ?? 49 BE ?? ?? ?? ?? ?? ?? ?? ??";
}

hi, this is bary. i'm dropping this dx12 imgui starter for u guys to use as a base.

it's got standard emulation and decoding with the offsets that are provided. the offsets rn are hardcoded to my build, so u need basic re skills to make this work on ur build or other qbs. this is just a base with standard skeleton emulation and esp boxes.

features:
- dx12 hooks + imgui overlay
- skeleton emulation + esp boxes
- pose math / projection
- game data + entity collection

deps (all vendored in third_party):
- imgui (win32 + dx12 backends)
- minhook
- zydis
- unicorn (x86)

build:
cmake -S . -B build -A x64
cmake --build build --config Release

windows x64 only, msvc, c++20. u can turn on OVERLAY_ENABLE_TEST_EXPORTS if u want the validation exports.

i hope u guys like it

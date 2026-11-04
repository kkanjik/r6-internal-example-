#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace emulation {
enum Register : unsigned { RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8,R9,R10,R11,R12,R13,R14,R15 };
struct Region { std::uint64_t address{}; std::vector<std::uint8_t> bytes; bool writable=false; };
struct Result {
 bool ok=false; std::string error; std::array<std::uint64_t,16> registers{};
 std::vector<Region> memory; std::uint64_t rip{}; std::size_t instructions{};
};
// All mappings are emulator-owned copies. No live-memory callbacks or native calls.
// Code and data must occupy distinct pages; total mapped bytes <= 1 MiB.
Result Run(std::uint64_t address,std::span<const std::uint8_t> code,
 const std::array<std::uint64_t,16>& registers,std::span<const Region> memory,
 std::size_t instructionBudget=256);
bool SelfTest();
}

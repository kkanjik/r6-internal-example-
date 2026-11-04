#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <stop_token>
#include <vector>
namespace emulation {
using SnapshotRead = std::function<bool(std::uint64_t, void*, std::size_t)>;
struct CodeRange { std::uint64_t address{}, size{}; };
struct MatrixRequest {
 std::uint64_t entry{}, returnAddress{}, argument{}, gsBase{};
 std::size_t instructionBudget=10000, pageBudget=256;
 std::uint64_t timeoutMicroseconds=25000;
 // At this validated instruction, R8 holds a contiguous dynamic stream.
 // Freeze all of it in one read before any element is loaded lazily.
 std::uint64_t snapshotCheckpoint=0,snapshotBytesFromR8=0;
};
struct MatrixResult {
 bool ok=false; std::string error; std::array<float,16> matrix{};
 std::size_t instructions{}, mappedPages{};
};
// Reads source pages into Unicorn-owned memory. Every emulated store stays there.
// No source-write callback or native target-call facility exists.
MatrixResult RunCopiedMatrix(const MatrixRequest&, const SnapshotRead&,
 std::span<const CodeRange>, std::stop_token={});
// One copied address-space snapshot and engine for an entire pose. CPU state,
// stack and dirty source pages are restored before every accessor invocation.
std::vector<MatrixResult> RunCopiedMatrices(std::span<const MatrixRequest>,
 const SnapshotRead&,std::span<const CodeRange>,std::stop_token={});
bool CopiedMatrixSelfTest();
}

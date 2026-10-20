// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 1 execution: a translation cache that runs guest programs through
// generated AArch64 code instead of the interpreter.
//
// Scope (Phase 4): this is a *dispatcher*, not yet an optimising JIT. It
// decodes a guest program, translates one basic block at a time into a fresh
// ExecMem region, caches the result by guest program counter, and runs blocks
// in a loop until the program halts. There is no block chaining, no register
// allocation and no eviction policy yet; those are Phase 5.
//
// Correctness rules that this class exists to enforce:
//   - An instruction the translator does not support is never executed.
//     The dispatcher stops with RunStatus::kUnsupported and the PC sitting on
//     the offending instruction, so the result is never silently wrong.
//   - The authoritative next PC is always GuestState::rip, which the generated
//     block writes. The cache is only a lookup accelerator.
//   - Guest memory is read through the same Memory interface the interpreter
//     uses, so a program cannot make the translator read out of bounds.
#ifndef HYPERION_TRANSLATION_CACHE_HPP
#define HYPERION_TRANSLATION_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "hyperion/exec_mem.hpp"
#include "hyperion/interpreter.hpp" // Memory, RunResult
#include "hyperion/run.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translator.hpp"

namespace hyperion {

// A guest program counter range mapped to one generated block.
struct CachedBlock {
    std::unique_ptr<ExecMem> memory; // generated AArch64 code
    BlockExit exit = BlockExit::kFallThrough;
    std::uint64_t start = 0;                 // first guest PC covered
    std::uint64_t target = 0;                // predicted successor (see BlockTranslation)
    std::size_t guest_instruction_count = 0; // instructions retired per execution
    std::size_t code_bytes = 0;              // size of the generated AArch64 code
};

// The translation cache and dispatcher.
class TranslationCache {
public:
    TranslationCache() = default;
    TranslationCache(const TranslationCache&) = delete;
    TranslationCache& operator=(const TranslationCache&) = delete;

    // Run the guest program whose first instruction is at `state.rip`, reading
    // code and data through `memory`, until it halts, faults, hits an
    // unsupported instruction, or exceeds `max_steps` retired instructions.
    //
    // `max_steps` counts guest instructions, matching interpreter::run_program,
    // so the two tiers can be compared directly.
    //
    // `syscalls` handles guest `syscall` instructions. When it is null a syscall
    // stops the run with RunStatus::kUnsupported rather than being skipped.
    RunResult run(Memory& memory, GuestState state, std::uint64_t max_steps = 1'000'000,
                  SyscallHandler* syscalls = nullptr);

    // Number of blocks currently cached (diagnostics/tests).
    std::size_t block_count() const { return blocks_.size(); }

    // Dispatch fast-path counters (diagnostics/tests/benchmarks). A "hit" is a
    // block found in the direct-mapped lookup cache without touching the hash
    // map; a "miss" is a lookup that fell back to the map (and translated the
    // block if it was not there yet). The hit rate is a deterministic proxy for
    // dispatch overhead, unlike timing.
    std::uint64_t dispatch_hits() const { return dispatch_hits_; }
    std::uint64_t dispatch_misses() const { return dispatch_misses_; }

    // Number of times a guest PC was translated (diagnostics/tests). Equal to
    // the number of cached blocks; exposed separately to make the intent clear.
    std::size_t translation_count() const { return blocks_.size(); }

    // Total bytes of generated AArch64 code across all cached blocks. A
    // deterministic proxy for translation quality: it changes only when the
    // code generator changes, unlike timing measurements, so it makes a stable
    // regression guard (see BENCHMARKS.md).
    std::size_t total_code_bytes() const;

private:
    // Translate the block starting at `pc`, or return nullptr if the
    // instruction there cannot be decoded. Never executes anything.
    const CachedBlock* translate_at(Memory& memory, std::uint64_t pc);

    // Look up `pc` through the direct-mapped fast path, then the map. Updates
    // the hit/miss counters. Returns nullptr only if the block is absent.
    const CachedBlock* lookup(std::uint64_t pc);

    // Install `block` (whose guest PC is `block->start`) in the map and the
    // direct-mapped cache.
    void install(const CachedBlock* block);

    std::unordered_map<std::uint64_t, std::unique_ptr<CachedBlock>> blocks_;

    // Direct-mapped dispatch cache: a small power-of-two table indexed by the
    // low bits of the guest PC, consulted before the hash map. It is a *pure
    // accelerator* - the map remains authoritative, so a collision (two PCs
    // sharing a slot) can only cause a fall-through to the map, never a wrong
    // block. This keeps the fast path free of correctness risk.
    static constexpr std::size_t kDirectBits = 12; // 4096 slots
    static constexpr std::size_t kDirectSize = 1u << kDirectBits;
    static constexpr std::size_t kDirectMask = kDirectSize - 1;
    std::vector<const CachedBlock*> direct_{kDirectSize, nullptr};
    std::uint64_t dispatch_hits_ = 0;
    std::uint64_t dispatch_misses_ = 0;

    // Scratch buffer reused for fetching guest code, to avoid an allocation per
    // block. Sized generously; a block is cut at the first unsupported
    // instruction well before this limit in practice.
    std::vector<std::uint8_t> fetch_;
};

} // namespace hyperion

#endif // HYPERION_TRANSLATION_CACHE_HPP

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 1 dispatcher: see translation_cache.hpp for the design and the
// correctness rules this file implements.
#include "hyperion/translation_cache.hpp"

#include <algorithm>
#include <string>

namespace hyperion {

namespace {

using CachedBlockFn = RawBlockReturn (*)(GuestState*, std::uint64_t);

// Guest code is fetched in bounded windows. A block longer than the window is
// simply cut at the window boundary (a fall-through), which is correct: the
// dispatcher re-enters at the next PC and translates the continuation.
constexpr std::size_t kMaxWindow = 4096;

// Try to read a window of guest code starting at `pc`. Returns the number of
// bytes read, shrinking the request until the read succeeds: a Memory
// implementation may fault if the window crosses an unmapped page, and a small
// program image may be shorter than the window. Returns 0 only if not even a
// single byte can be read.
std::size_t read_window(Memory& memory, std::uint64_t pc, std::uint8_t* out, std::size_t capacity) {
    for (std::size_t want = capacity; want >= 1; want /= 2) {
        if (memory.fetch(pc, out, want)) {
            return want;
        }
        if (want == 1) {
            break;
        }
    }
    return 0;
}

} // namespace

const CachedBlock* TranslationCache::lookup(std::uint64_t pc) {
    const CachedBlock* fast = direct_[pc & kDirectMask];
    if (fast != nullptr && fast->start == pc) {
        ++dispatch_hits_;
        return fast;
    }
    ++dispatch_misses_;
    const auto it = blocks_.find(pc);
    if (it == blocks_.end()) {
        return nullptr;
    }
    direct_[pc & kDirectMask] = it->second.get();
    return it->second.get();
}

void TranslationCache::install(const CachedBlock* block) {
    direct_[block->start & kDirectMask] = block;
}

void TranslationCache::invalidate_all() {
    blocks_.clear();
    std::fill(direct_.begin(), direct_.end(), nullptr);
    arena_.reset();
}

const CachedBlock* TranslationCache::translate_at(Memory& memory, std::uint64_t pc,
                                                  TranslateFailure& failure) {
    failure = TranslateFailure::kNone;
    const CachedBlock* cached = lookup(pc);
    if (cached != nullptr) {
        return cached;
    }

    if (fetch_.size() < kMaxWindow) {
        fetch_.resize(kMaxWindow);
    }
    const std::size_t available = read_window(memory, pc, fetch_.data(), fetch_.size());
    if (available == 0) {
        failure = TranslateFailure::kFault;
        return nullptr; // unmapped or non-executable fetch at pc
    }

    const DecodeResult decoded = decode(fetch_.data(), available, pc);
    if (!decoded.ok()) {
        failure = TranslateFailure::kUnsupported;
        return nullptr; // not a decodable instruction: report, do not guess
    }

    const BlockTranslation translated =
        translate_block(fetch_.data(), available, decoded.instruction, /*enable_self_loop=*/true);
    if (!translated.ok() || translated.code.empty()) {
        failure = TranslateFailure::kUnsupported;
        return nullptr;
    }

    auto block = std::make_unique<CachedBlock>();
    block->entry = arena_.allocate(translated.code.data(), translated.code.size());
    block->exit = translated.exit;
    block->start = pc;
    block->target = translated.target;
    block->guest_instruction_count = translated.guest_instruction_count;
    block->code_bytes = translated.code.size();

    const CachedBlock* raw = block.get();
    blocks_.emplace(pc, std::move(block));
    install(raw);
    return raw;
}

std::size_t TranslationCache::total_code_bytes() const {
    std::size_t total = 0;
    for (const auto& entry : blocks_) {
        total += entry.second->code_bytes;
    }
    return total;
}

RunResult TranslationCache::run(Memory& memory, GuestState state, std::uint64_t max_steps,
                                SyscallHandler* syscalls) {
    RunResult out;
    out.state = state;

    if (!ExecMem::executable_supported()) {
        out.status = RunStatus::kUnsupported;
        out.message = "host cannot execute generated AArch64 code";
        return out;
    }

    const std::uint64_t entry_epoch = memory.code_epoch();
    if (cached_epoch_ != 0 && entry_epoch != cached_epoch_) {
        invalidate_all();
    }
    cached_epoch_ = entry_epoch;

    std::uint64_t steps = 0;
    const CachedBlock* prev_block = nullptr;
    while (true) {
        const std::uint64_t pc = out.state.rip;
        const CachedBlock* block = nullptr;

        // Direct 2-way block chaining fast path across multi-block control flow.
        if (prev_block != nullptr) {
            if (prev_block->chain_block0 != nullptr && prev_block->chain_pc0 == pc) {
                block = prev_block->chain_block0;
                ++dispatch_hits_;
            } else if (prev_block->chain_block1 != nullptr && prev_block->chain_pc1 == pc) {
                block = prev_block->chain_block1;
                ++dispatch_hits_;
            }
        }

        if (block == nullptr) {
            TranslateFailure failure = TranslateFailure::kNone;
            block = translate_at(memory, pc, failure);
            if (block == nullptr) {
                if (failure == TranslateFailure::kFault) {
                    out.status = RunStatus::kFault;
                    out.message = "instruction fetch fault at rip";
                } else {
                    out.status = RunStatus::kUnsupported;
                    out.message = "unsupported instruction at pc=0x" + std::to_string(pc);
                }
                out.steps = steps;
                return out;
            }
            if (prev_block != nullptr) {
                if (prev_block->chain_block0 == nullptr) {
                    prev_block->chain_pc0 = pc;
                    prev_block->chain_block0 = block;
                } else if (prev_block->chain_block1 == nullptr) {
                    prev_block->chain_pc1 = pc;
                    prev_block->chain_block1 = block;
                }
            }
        }

        const std::uint64_t remaining = max_steps - steps;
        if (block->guest_instruction_count > remaining) {
            out.status = RunStatus::kStepLimit;
            out.message = "step limit reached without halting";
            out.steps = steps;
            return out;
        }

        const std::uint64_t max_runs = remaining / block->guest_instruction_count;
        const auto fn = reinterpret_cast<CachedBlockFn>(block->entry);
        const RawBlockReturn ret = fn(&out.state, max_runs);
        const auto status = static_cast<BlockResult>(ret.status);
        const std::uint64_t runs = ret.runs;
        steps += runs * block->guest_instruction_count;
        if (runs > 1) {
            dispatch_hits_ += (runs - 1);
        }

        if (status == BlockResult::kHalted) {
            out.status = RunStatus::kOk;
            out.steps = steps;
            return out;
        }
        if (status == BlockResult::kSyscall) {
            prev_block = nullptr;
            if (syscalls == nullptr) {
                out.status = RunStatus::kUnsupported;
                out.message = "syscall encountered with no handler";
                out.steps = steps;
                return out;
            }
            const SyscallResult sr = syscalls->handle(out.state, memory);
            const std::uint64_t post_epoch = memory.code_epoch();
            if (post_epoch != cached_epoch_) {
                invalidate_all();
                cached_epoch_ = post_epoch;
            }
            if (sr.status == SyscallStatus::kExit) {
                out.status = RunStatus::kOk;
                out.exited_via_syscall = true;
                out.exit_code = sr.exit_code;
                out.message = "guest exited via syscall";
                out.steps = steps;
                return out;
            }
            if (sr.status == SyscallStatus::kUnimplemented) {
                ++out.unimplemented_syscalls;
            }
            continue;
        }
        prev_block = block;
    }
}

} // namespace hyperion

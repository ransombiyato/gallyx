// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Reference interpreter for the Phase 2 instruction subset.
//
// This is the project's correctness oracle. It is written for clarity and
// exactness, not speed: Phase 3's AArch64 code generator is validated by
// comparing its results against this interpreter, so a subtle bug here would
// be replicated into the generated code. Where x86-64 semantics are
// surprising (32-bit writes zero-extending to 64 bits, INC/DEC preserving CF,
// AND/OR/XOR clearing CF and OF), the code states the rule explicitly.
#ifndef HYPERION_INTERPRETER_HPP
#define HYPERION_INTERPRETER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"

namespace hyperion {

// Abstract guest memory. The interpreter never touches host memory directly,
// so tests can supply a bounded flat region, and later a real address space
// can be layered underneath without changing the interpreter.
class Memory {
public:
    virtual ~Memory() = default;
    // Return false on an unmapped/invalid access. Implementations must not
    // read or write out of bounds.
    virtual bool read(std::uint64_t address, std::uint8_t* out, std::size_t count) = 0;
    virtual bool write(std::uint64_t address, const std::uint8_t* in, std::size_t count) = 0;
};

// A flat, byte-addressable region [base, base + size). Used by tests and by
// the differential harness. Accesses outside the region fault rather than
// silently reading zeros.
class FlatMemory final : public Memory {
public:
    FlatMemory(std::uint64_t base, std::size_t size);

    bool read(std::uint64_t address, std::uint8_t* out, std::size_t count) override;
    bool write(std::uint64_t address, const std::uint8_t* in, std::size_t count) override;

    std::uint64_t base() const { return base_; }
    std::size_t size() const { return bytes_.size(); }

    // Direct access for test setup and inspection. Callers must stay in range.
    std::uint8_t* data() { return bytes_.data(); }
    const std::uint8_t* data() const { return bytes_.data(); }

private:
    bool contains(std::uint64_t address, std::size_t count) const;

    std::uint64_t base_;
    std::vector<std::uint8_t> bytes_;
};

enum class StepStatus {
    kOk,          // one instruction executed; state updated
    kHalted,      // HLT executed
    kFault,       // memory fault or invalid state
    kUnsupported, // decode failed: the instruction is not in the subset
};

struct StepResult {
    StepStatus status = StepStatus::kOk;
    DecodeError decode_error = DecodeError::kNone;
    Instruction instruction{};
    std::string message;

    bool ok() const { return status == StepStatus::kOk; }
};

// Execute exactly one instruction at state.rip. On kOk the state is fully
// updated, including rip. On kUnsupported the state is unchanged, so a caller
// can stop cleanly at the first unknown instruction rather than run past it.
StepResult step(GuestState& state, Memory& memory);

// ALU helpers, exposed for the interpreter's tests and for the Phase 3 code
// generator's differential checks. `width` is 32 or 64 bits.
namespace alu {

// Add/subtract with full flag computation, operating at `width` bits. The
// returned value is the truncated result (not yet zero-extended).
std::uint64_t add(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t sub(std::uint64_t a, std::uint64_t b, int width, Flags& flags);

// Logical ops: CF and OF are cleared; AF is cleared (undefined on real
// hardware, and this project models it as 0 for determinism); SF/ZF/PF from
// the result.
std::uint64_t logical_and(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t logical_or(std::uint64_t a, std::uint64_t b, int width, Flags& flags);
std::uint64_t logical_xor(std::uint64_t a, std::uint64_t b, int width, Flags& flags);

// INC/DEC behave like ADD/SUB of 1 except that CF is preserved.
std::uint64_t inc(std::uint64_t a, int width, Flags& flags);
std::uint64_t dec(std::uint64_t a, int width, Flags& flags);

// Evaluate a condition code against the flags. Exposed so the interpreter and
// the code generator can share one definition of "jle".
bool evaluate(Condition condition, const Flags& flags);

// Set SF/ZF/PF from a result of the given width.
void set_szp(std::uint64_t result, int width, Flags& flags);

} // namespace alu

// Result of running a program image to completion.
enum class RunStatus {
    kOk,          // program halted (HLT/RET) normally
    kStepLimit,   // max_steps reached without halting
    kFault,       // memory fault during execution
    kUnsupported, // hit an instruction outside the supported subset
};

struct RunResult {
    RunStatus status = RunStatus::kOk;
    std::uint64_t steps = 0; // instructions retired
    std::string message;     // human-readable reason on non-kOk
    // The final architectural state, valid when status == kOk or kStepLimit.
    GuestState state;
};

// Run `code` placed at `base` until the program halts (HLT/RET), an error
// occurs, or `max_steps` is exceeded. This is a convenience layer over step();
// it adds no new semantics, but it makes multi-instruction programs (with
// branches and loops) testable.
//
// `max_steps` is mandatory-by-default (not unbounded): a guest program that
// never halts must be reported as such, not hang the test suite. Callers that
// genuinely want an unbounded run can pass a very large value, but the default
// protects the tests.
RunResult run_program(Memory& memory, GuestState state, std::uint64_t max_steps = 1'000'000);

} // namespace hyperion

#endif // HYPERION_INTERPRETER_HPP

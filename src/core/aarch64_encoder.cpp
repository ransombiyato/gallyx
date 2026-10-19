// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 encoder. Every encoding here is checked against GNU as/objdump in
// tests/unit/test_aarch64_encoder.cpp; see the reference dump recorded there.
// Base opcodes carry a short comment naming the encoding class so a reader can
// cross-check against the ARM ARM without reverse-engineering the constants.
#include "hyperion/aarch64_encoder.hpp"

#include <cassert>
#include <stdexcept>

namespace hyperion::a64 {

namespace {

constexpr std::size_t kUnbound = static_cast<std::size_t>(-1);

// sf bit (bit 31) and a few shared bases for data-processing instructions.
constexpr std::uint32_t sf_of(Width w) {
    return w == Width::k64 ? 0x80000000u : 0u;
}

// Add/subtract (shifted register): sf op S 01011 shift(2) 0 Rm imm6 Rn Rd.
// LSL by 0 means shift=0, imm6=0.
constexpr std::uint32_t kAddSubRegBase = 0x0B000000u;
// Add/subtract (immediate): sf op S 10001 sh(1) imm12 Rn Rd.
constexpr std::uint32_t kAddSubImmBase = 0x11000000u;
// Logical (shifted register): sf opc(2) 01010 shift(2) N(1) Rm imm6 Rn Rd.
constexpr std::uint32_t kLogicalRegBase = 0x0A000000u;

void validate_reg(Reg r) {
    if (r > 31) {
        throw std::invalid_argument("a64::Encoder: register number out of range");
    }
}

void validate_imm12(std::uint32_t v) {
    if (v > 0xFFFu) {
        throw std::invalid_argument("a64::Encoder: immediate does not fit in 12 bits");
    }
}

} // namespace

const char* to_string(Cond c) {
    switch (c) {
    case Cond::kEq:
        return "eq";
    case Cond::kNe:
        return "ne";
    case Cond::kCs:
        return "cs";
    case Cond::kCc:
        return "cc";
    case Cond::kMi:
        return "mi";
    case Cond::kPl:
        return "pl";
    case Cond::kVs:
        return "vs";
    case Cond::kVc:
        return "vc";
    case Cond::kHi:
        return "hi";
    case Cond::kLs:
        return "ls";
    case Cond::kGe:
        return "ge";
    case Cond::kLt:
        return "lt";
    case Cond::kGt:
        return "gt";
    case Cond::kLe:
        return "le";
    case Cond::kAl:
        return "al";
    case Cond::kNv:
        return "nv";
    }
    return "?";
}

void Encoder::emit(std::uint32_t word) {
    code_.push_back(word);
}

void Encoder::add(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (rm << 16) | (rn << 5) | rd);
}

void Encoder::sub(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::and_(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (rm << 16) | (rn << 5) | rd);
}

void Encoder::orr(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::eor(Width w, Reg rd, Reg rn, Reg rm) {
    validate_reg(rd);
    validate_reg(rn);
    validate_reg(rm);
    emit(sf_of(w) | kLogicalRegBase | (2u << 29) | (rm << 16) | (rn << 5) | rd);
}

void Encoder::add_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (shift12 ? (1u << 22) : 0u) | (imm12 << 10) | (rn << 5) | rd);
}

void Encoder::sub_imm(Width w, Reg rd, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rd);
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (shift12 ? (1u << 22) : 0u) | (imm12 << 10) |
         (rn << 5) | rd);
}

void Encoder::cmp(Width w, Reg rn, Reg rm) {
    validate_reg(rn);
    validate_reg(rm);
    // SUBS with Rd = XZR.
    emit(sf_of(w) | kAddSubRegBase | (1u << 30) | (1u << 29) | (rm << 16) | (rn << 5) | kXzr);
}

void Encoder::cmp_imm(Width w, Reg rn, std::uint32_t imm12, bool shift12) {
    validate_reg(rn);
    validate_imm12(imm12);
    emit(sf_of(w) | kAddSubImmBase | (1u << 30) | (1u << 29) | (shift12 ? (1u << 22) : 0u) |
         (imm12 << 10) | (rn << 5) | kXzr);
}

void Encoder::mov_reg(Width w, Reg rd, Reg rm) {
    validate_reg(rd);
    validate_reg(rm);
    // ORR Rd, XZR, Rm  -- the canonical MOV alias. A 32-bit form zero-extends.
    emit(sf_of(w) | kLogicalRegBase | (1u << 29) | (rm << 16) | (kXzr << 5) | rd);
}

void Encoder::movz(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movz shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    // MOVZ: sf 10 100101 hw(2) imm16 Rd.
    const std::uint32_t base = (w == Width::k64) ? 0xD2800000u : 0x52800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::movk(Width w, Reg rd, std::uint16_t imm16, unsigned shift) {
    validate_reg(rd);
    if (shift > 48 || (shift % 16) != 0) {
        throw std::invalid_argument("a64::Encoder: movk shift must be 0/16/32/48");
    }
    const std::uint32_t hw = shift / 16;
    // MOVK: sf 11 100101 hw(2) imm16 Rd.
    const std::uint32_t base = (w == Width::k64) ? 0xF2800000u : 0x72800000u;
    emit(base | (hw << 21) | (static_cast<std::uint32_t>(imm16) << 5) | rd);
}

void Encoder::mov_imm64(Reg rd, std::uint64_t value) {
    // First instruction is always MOVZ so the register is fully defined; higher
    // halfwords are added only when non-zero.
    bool first = true;
    for (unsigned shift = 0; shift <= 48; shift += 16) {
        const auto half = static_cast<std::uint16_t>((value >> shift) & 0xFFFFu);
        if (first) {
            movz(Width::k64, rd, half, shift);
            first = false;
        } else if (half != 0) {
            movk(Width::k64, rd, half, shift);
        }
    }
}

void Encoder::b(Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x14000000u); // B, imm26 placeholder
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], false);
    } else {
        fixups_.push_back({at, target, false});
    }
}

void Encoder::b_cond(Cond c, Label target) {
    check_label(target);
    const std::size_t at = code_.size();
    emit(0x54000000u | static_cast<std::uint32_t>(c)); // B.cond, imm19 placeholder
    if (label_pos_[target] != kUnbound) {
        resolve_patch(at, label_pos_[target], true);
    } else {
        fixups_.push_back({at, target, true});
    }
}

void Encoder::ret(Reg rn) {
    validate_reg(rn);
    emit(0xD65F0000u | (rn << 5)); // RET Xn
}

void Encoder::nop() {
    emit(0xD503201Fu);
}

Label Encoder::new_label() {
    label_pos_.push_back(kUnbound);
    return label_pos_.size() - 1;
}

void Encoder::bind(Label label) {
    check_label(label);
    if (label_pos_[label] != kUnbound) {
        throw std::logic_error("a64::Encoder: label bound twice");
    }
    label_pos_[label] = code_.size();
    // Patch any forward branches that were waiting on this label.
    for (auto& f : fixups_) {
        if (f.target == label && f.at != kUnbound) {
            resolve_patch(f.at, code_.size(), f.conditional);
            f.at = kUnbound; // mark done
        }
    }
}

void Encoder::resolve_patch(std::size_t at, std::size_t target, bool conditional) {
    const auto delta_bytes =
        static_cast<std::int64_t>(target) * 4 - static_cast<std::int64_t>(at) * 4;
    if (conditional) {
        const std::int64_t imm19 = delta_bytes / 4;
        if (imm19 < -(1 << 18) || imm19 > (1 << 18) - 1) {
            throw std::out_of_range("a64::Encoder: B.cond target out of range");
        }
        code_[at] |= (static_cast<std::uint32_t>(imm19) & 0x7FFFFu) << 5;
    } else {
        const std::int64_t imm26 = delta_bytes / 4;
        if (imm26 < -(1 << 25) || imm26 > (1 << 25) - 1) {
            throw std::out_of_range("a64::Encoder: B target out of range");
        }
        code_[at] |= static_cast<std::uint32_t>(imm26) & 0x03FFFFFFu;
    }
}

void Encoder::check_label(Label label) const {
    if (label >= label_pos_.size()) {
        throw std::out_of_range("a64::Encoder: unknown label");
    }
}

bool Encoder::has_unresolved_labels() const {
    for (const auto& f : fixups_) {
        if (f.at != kUnbound) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> Encoder::bytes() const {
    if (has_unresolved_labels()) {
        throw std::logic_error("a64::Encoder: branch to a label that was never bound");
    }
    std::vector<std::uint8_t> out;
    out.reserve(code_.size() * 4);
    for (const std::uint32_t word : code_) {
        out.push_back(static_cast<std::uint8_t>(word & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFFu));
    }
    return out;
}

} // namespace hyperion::a64

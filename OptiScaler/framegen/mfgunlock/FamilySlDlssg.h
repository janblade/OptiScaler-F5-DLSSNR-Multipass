#pragma once

#include "FamilyRtx40.h"

// RTX 40 side of the Streamline DLSS-G plugin (sl.dlss_g), two independent derivations that must both succeed:
//  - the frame-count clamp: `mov edx, N / cmp ecx, edx / cmovb edx, ecx` becomes `cmovb edx, edx` (one ModRM byte), so
//    the count NGX reports is not lowered back to the plugin's compiled maximum;
//  - software pacing: the plugin has a fallback for a missing flip-metering DLL that logs "FG1 DLL has been detected"
//    and writes one byte of its context. That (offset, value) pair is read from the fallback itself and every other
//    byte store to the offset is pinned to the same value, so hardware flip metering cannot freeze more than 2x. The
//    member initialisation (a cleared register stored once) is left as it is, although it writes 0 (hardware flip
//    metering): that rests on the construction pattern, not on a proven order with the pinned stores. Any other store
//    that cannot be pinned in place stands the family aside.
// Adapted from KleberMotta/OptiScaler-DLSS5-MFG-RTX40 (MIT, from the RenoDX MFG Unlock addon). See
// Licenses/MFGUnlock_LICENSE.txt.
namespace MfgUnlock::FamilyRtx40::Streamline
{
inline constexpr std::string_view kFlipMarker = "FG1 DLL has been detected";
inline constexpr uint32_t kFlipWindow = 0x200;
inline constexpr uint32_t kMinField = 0x100;
inline constexpr uint32_t kMaxField = 0x20000;
inline constexpr size_t kStoreLength = 7;
inline constexpr size_t kMaxSites = 16;

struct Report
{
    std::vector<LocatedPatch> patches;
    uint8_t compiledCeiling = 0;
    uint32_t flipField = 0;
    uint8_t flipValue = 0;
    // Zero-initialising register stores to the state, left unpinned: they write 0 (hardware flip metering) and are
    // accepted by the construction pattern only, not a proven order with the pinned stores (see LocateFlip).
    unsigned initialisers = 0;
    std::string failure;

    bool Complete() const
    {
        return failure.empty() && !patches.empty() &&
               std::all_of(patches.begin(), patches.end(),
                           [](const LocatedPatch& patch) { return patch.state == LocateState::Found; });
    }
};

namespace detail
{
inline constexpr std::string_view kCeilingBytes("\xBA\x00\x00\x00\x00\x3B\xCA\x0F\x42\xD1", 10);
inline constexpr std::string_view kCeilingMask = "x???xxxxxx";
inline constexpr MaskedPattern kCeiling { kCeilingBytes, kCeilingMask };

inline bool CeilingAnchor(const ImageView& image, uint32_t rva, void*)
{
    const auto* bytes = image.DataAtRva(rva, kCeilingBytes.size());
    if (bytes == nullptr)
        return false;
    const auto count = std::to_integer<uint8_t>(bytes[1]);
    const bool zeroUpper = bytes[2] == std::byte { 0 } && bytes[3] == std::byte { 0 } && bytes[4] == std::byte { 0 };
    FunctionRange function {};
    return zeroUpper && count >= 1 && count <= 8 && image.FindFunctionRange(rva, function) && rva >= function.begin &&
           rva + kCeilingBytes.size() <= function.end;
}

// mov byte ptr [reg+disp32], x with mod = 10 and no SIB.
inline bool IsDisp32NoSib(uint8_t modrm)
{
    return modrm >= 0x80 && modrm <= 0xBF && (modrm & 7) != 4;
}

inline uint32_t ReadU32(const std::byte* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

enum class Walk
{
    Start,    // the linear walk lands on rva
    NotStart, // the walk decoded cleanly past rva: rva is inside another instruction
    Unknown   // no unwind function, or the walk could not decode its way to rva (VEX, data, desync)
};

// Decodes linearly from the start of the unwind function holding rva.
inline Walk WalkToInstruction(const ImageView& image, uint32_t rva)
{
    FunctionRange function {};
    if (!image.FindFunctionRange(rva, function) || rva < function.begin || rva >= function.end)
        return Walk::Unknown;
    for (uint32_t cursor = function.begin; cursor < function.end;)
    {
        if (cursor == rva)
            return Walk::Start;
        if (cursor > rva)
            return Walk::NotStart;
        const uint32_t room = function.end - cursor;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(cursor, take);
        if (bytes == nullptr)
            return Walk::Unknown;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take)
            return Walk::Unknown;
        cursor += decoded.len;
    }
    return Walk::Unknown;
}

// True only when rva is provably an instruction start (fail closed for the fallback's own store).
inline bool IsInstructionStart(const ImageView& image, uint32_t rva)
{
    return WalkToInstruction(image, rva) == Walk::Start;
}

inline std::string Hex(uint32_t value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (int shift = 28; shift >= 0; shift -= 4)
        if (!text.empty() || (value >> shift) != 0 || shift == 0)
            text += digits[(value >> shift) & 15];
    return text;
}

inline std::string HexBytes(const ImageView& image, uint32_t rva, size_t length)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    const auto* bytes = image.DataAtRva(rva, length);
    for (size_t index = 0; bytes != nullptr && index < length; ++index)
    {
        const auto value = std::to_integer<uint8_t>(bytes[index]);
        if (index != 0)
            text += ' ';
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

struct ByteStore
{
    uint32_t rva = 0;
    hde64s decoded {};
    bool knownInstruction = true; // false: outside every unwind function, so not provably an instruction
};

// Instructions that write one byte to their memory operand.
inline bool WritesByte(const hde64s& decoded)
{
    const uint8_t op = decoded.opcode;
    const uint8_t reg = decoded.modrm_reg;
    if (op == 0x0F)
        return (decoded.opcode2 >= 0x90 && decoded.opcode2 <= 0x9F) || decoded.opcode2 == 0xB0 ||
               decoded.opcode2 == 0xC0; // setcc, cmpxchg r/m8, xadd r/m8
    return op == 0xC6 || op == 0x88 || op == 0x86 || (op == 0x80 && reg != 7) ||
           (op <= 0x30 && (op & 7) == 0) || (op == 0xFE && reg <= 1) || (op == 0xF6 && (reg == 2 || reg == 3));
}

// The instruction whose disp32 sits at dispRva, when it writes one byte to [base + field] (mod 10, with or without
// SIB). Reads and wider writes of that offset are other data and are not reported.
inline bool FindByteStore(const ImageView& image, uint32_t dispRva, uint32_t field, ByteStore& store)
{
    for (uint32_t back = 2; back <= 5 && back <= dispRva; ++back)
    {
        const uint32_t start = dispRva - back;
        const size_t room = image.ImageSize() - start;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(start, take);
        if (bytes == nullptr)
            continue;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take ||
            (decoded.flags & F_DISP32) == 0 || decoded.modrm_mod != 2 || decoded.disp.disp32 != field)
            continue;
        const uint32_t immediate = (decoded.flags & F_IMM8) != 0    ? 1
                                   : (decoded.flags & F_IMM16) != 0 ? 2
                                   : (decoded.flags & F_IMM32) != 0 ? 4
                                   : (decoded.flags & F_IMM64) != 0 ? 8
                                                                    : 0;
        if (start + decoded.len - immediate - sizeof(uint32_t) != dispRva || !WritesByte(decoded))
            continue;

        // Only a clean walk past `start` rules a candidate out. When the walk cannot reach it the store may be real,
        // so it is reported as not provably an instruction, which stands the family aside.
        const Walk walk = WalkToInstruction(image, start);
        if (walk == Walk::NotStart)
            continue;
        store.rva = start;
        store.decoded = decoded;
        store.knownInstruction = walk == Walk::Start;
        return true;
    }
    return false;
}

// A `mov [base+field], r8` without REX (six bytes) has no in-place imm8 form. The known providers have exactly one:
// the object's member initialisation, storing a register cleared by `xor r32, r32`. It is accepted only when that is
// provable in a linear walk of its function: the xor is the last write of the register before the store, everything in
// between is a memory store, a load into another register or a nop, and no relative branch lands in between.
inline bool IsZeroInitialiser(const ImageView& image, const ByteStore& store)
{
    const auto& target = store.decoded;
    const bool rex = (target.flags & F_PREFIX_REX) != 0;
    const uint8_t source = static_cast<uint8_t>(target.modrm_reg | (target.rex_r << 3));
    FunctionRange function {};
    if (target.opcode != 0x88 || !store.knownInstruction || (!rex && source >= 4) || // ah..bh
        !image.FindFunctionRange(store.rva, function))
        return false;

    uint32_t clearedAt = 0;
    bool cleared = false;
    std::vector<uint32_t> branchTargets;
    for (uint32_t cursor = function.begin; cursor < function.end;)
    {
        const uint32_t room = function.end - cursor;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(cursor, take);
        if (bytes == nullptr)
            return false;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take)
            return false;

        uint32_t branch = 0;
        if ((decoded.flags & F_RELATIVE) != 0 && DecodeRelativeBranchTarget(image, cursor, branch))
            branchTargets.push_back(branch);

        if (cursor < store.rva)
        {
            const bool memory = (decoded.flags & F_MODRM) != 0 && decoded.modrm_mod != 3;
            const uint8_t reg = static_cast<uint8_t>(decoded.modrm_reg | (decoded.rex_r << 3));
            const uint8_t rm = static_cast<uint8_t>(decoded.modrm_rm | (decoded.rex_b << 3));
            if ((decoded.opcode == 0x31 || decoded.opcode == 0x33) && (decoded.flags & F_MODRM) != 0 && !memory &&
                reg == rm && reg == source && (decoded.flags & F_PREFIX_66) == 0)
            {
                cleared = true;
                clearedAt = cursor;
            }
            else if (cleared)
            {
                // General-purpose stores and the SSE stores (movups/movaps/movdqa/movq/movntps) of initialisers.
                const bool sseStore = decoded.opcode == 0x0F &&
                                      (decoded.opcode2 == 0x11 || decoded.opcode2 == 0x13 || decoded.opcode2 == 0x17 ||
                                       decoded.opcode2 == 0x29 || decoded.opcode2 == 0x2B || decoded.opcode2 == 0x7F ||
                                       decoded.opcode2 == 0xD6 || decoded.opcode2 == 0xE7);
                const bool memoryStore = memory && (decoded.opcode == 0x88 || decoded.opcode == 0x89 ||
                                                    decoded.opcode == 0xC6 || decoded.opcode == 0xC7 || sseStore);
                const bool otherLoad =
                    (decoded.opcode == 0x8A || decoded.opcode == 0x8B || decoded.opcode == 0x8D) && reg != source;
                const bool nop = decoded.opcode == 0x90 || (decoded.opcode == 0x0F && decoded.opcode2 == 0x1F);
                if (!memoryStore && !otherLoad && !nop)
                    cleared = false;
            }
        }
        cursor += decoded.len;
    }
    if (!cleared)
        return false;
    for (const uint32_t branch : branchTargets)
        if (branch > clearedAt && branch <= store.rva)
            return false;
    return true;
}

// The bytes that make the store write `want`; empty when it already does. False when it cannot be rewritten in place:
// a store outside every unwind function, any byte write other than `mov`, an immediate that is not 0/1, or a register
// store whose immediate form is longer.
inline bool PinByteStore(const ImageView& image, const ByteStore& store, uint8_t want, std::vector<std::byte>& replacement)
{
    replacement.clear();
    const auto& decoded = store.decoded;
    const auto* bytes = image.DataAtRva(store.rva, decoded.len);
    if (!store.knownInstruction || bytes == nullptr)
        return false;

    if (decoded.opcode == 0xC6 && decoded.modrm_reg == 0)
    {
        const uint8_t value = decoded.imm.imm8;
        if (value == want)
            return true;
        if (value != 1 - want)
            return false;
        replacement.assign(bytes, bytes + decoded.len);
        replacement.back() = static_cast<std::byte>(want);
        return true;
    }

    // mov [base(+index)+disp32], r8 with a REX prefix that only names the source register (or nothing): dropping the
    // REX for the imm8 keeps the length, C6 modrm [sib] disp32 imm8.
    const bool sib = (decoded.flags & F_SIB) != 0;
    const size_t expected = 1 + 1 + 1 + (sib ? 1 : 0) + sizeof(uint32_t);
    if (decoded.opcode != 0x88 || (decoded.flags & F_PREFIX_ANY) != F_PREFIX_REX || decoded.rex_b != 0 ||
        decoded.rex_x != 0 || decoded.len != expected)
        return false;
    replacement.reserve(decoded.len);
    replacement.push_back(std::byte { 0xC6 });
    replacement.push_back(static_cast<std::byte>(0x80 | decoded.modrm_rm));
    replacement.insert(replacement.end(), bytes + 3, bytes + decoded.len); // [sib] disp32
    replacement.push_back(static_cast<std::byte>(want));
    return replacement.size() == decoded.len;
}

inline void Fail(Report& report, std::string message)
{
    if (report.failure.empty())
        report.failure = std::move(message);
}

inline LocatedPatch MakePatch(std::string name, const ImageView& image, uint32_t rva,
                              std::vector<std::byte> replacement)
{
    LocatedPatch patch;
    patch.name = std::move(name);
    patch.rva = rva;
    patch.replacement = std::move(replacement);
    const auto* bytes = image.DataAtRva(rva, patch.replacement.size());
    if (bytes == nullptr)
    {
        patch.state = LocateState::InvalidImage;
        return patch;
    }
    patch.expected.assign(bytes, bytes + patch.replacement.size());
    patch.state = LocateState::Found;
    return patch;
}

inline void LocateFlip(const ImageView& image, Report& report)
{
    const auto reference = FindUniqueStringReference(image, kFlipMarker, true);
    if (reference.state != LocateState::Found)
    {
        report.patches.push_back({ "F1 flip metering fallback", reference.state, 0, {}, {} });
        Fail(report, "F1 flip metering fallback: " + std::string(LocateStateName(reference.state)));
        return;
    }

    // The fallback's own store defines the wanted state; every store in the window must agree.
    const uint32_t room = reference.function.end - reference.instructionRva;
    const uint32_t window = std::min(room, kFlipWindow);
    bool haveWanted = false;
    bool conflict = false;
    uint32_t wantField = 0;
    uint8_t wantValue = 0;
    for (uint32_t offset = 0; offset + kStoreLength <= window; ++offset)
    {
        const auto* p = image.DataAtRva(reference.instructionRva + offset, kStoreLength);
        if (p == nullptr)
            break;
        if (p[0] != std::byte { 0xC6 } || !IsDisp32NoSib(std::to_integer<uint8_t>(p[1])) ||
            (std::to_integer<uint8_t>(p[1]) & 0x38) != 0)
            continue;
        const uint32_t field = ReadU32(p + 2);
        const auto value = std::to_integer<uint8_t>(p[6]);
        if (field <= kMinField || field >= kMaxField || value > 1 ||
            !IsInstructionStart(image, reference.instructionRva + offset))
            continue;
        if (!haveWanted)
        {
            haveWanted = true;
            wantField = field;
            wantValue = value;
        }
        else if (field != wantField || value != wantValue)
        {
            conflict = true;
        }
    }
    if (!haveWanted || conflict)
    {
        report.patches.push_back({ "F1 flip metering fallback", LocateState::AnchorFailed, 0, {}, {} });
        Fail(report, std::string("F1 flip metering fallback: ") +
                         (conflict ? "the fallback writes more than one state" : "state not readable"));
        return;
    }
    report.flipField = wantField;
    report.flipValue = wantValue;

    // Every other byte store to the field in executable code is pinned to the same value. Stores are found by their
    // displacement and decoded, so every encoding is seen; one that cannot be rewritten in place stands the family
    // aside rather than staying un-pinned behind a complete report.
    std::vector<LocatedPatch> sites;
    std::vector<uint32_t> seen;
    std::string unpinnable;
    for (WORD index = 0; index < image.SectionCount() && unpinnable.empty(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_READ) == 0 || section.Misc.VirtualSize < sizeof(uint32_t))
            continue;
        const auto* data = image.DataAtRva(section.VirtualAddress, section.Misc.VirtualSize);
        if (data == nullptr)
            continue;
        for (uint32_t offset = 0; offset + sizeof(uint32_t) <= section.Misc.VirtualSize; ++offset)
        {
            if (ReadU32(data + offset) != wantField)
                continue;
            const uint32_t dispRva = section.VirtualAddress + offset;
            ByteStore store;
            if (!FindByteStore(image, dispRva, wantField, store) ||
                std::find(seen.begin(), seen.end(), store.rva) != seen.end())
                continue;
            seen.push_back(store.rva);

            std::vector<std::byte> replacement;
            if (!PinByteStore(image, store, wantValue, replacement) && IsZeroInitialiser(image, store))
            {
                // Accepted residual: the member initialisation writes 0, which is hardware flip metering (the
                // opposite of the pinned state when that is 1). It is kept on the basis of the object-construction
                // pattern only; nothing here proves that a pinned store runs after construction and before the
                // state is read. Reported as "initialisers" in the status.
                ++report.initialisers;
                continue;
            }
            if (!PinByteStore(image, store, wantValue, replacement))
            {
                unpinnable = "the store at RVA 0x" + Hex(store.rva) + " (" + HexBytes(image, store.rva, store.decoded.len) +
                             (store.knownInstruction ? ")" : ", outside every unwind function)") +
                             " cannot be pinned in place";
                break;
            }
            if (replacement.empty())
                continue; // already writes the wanted state
            if (sites.size() >= kMaxSites)
            {
                report.patches.push_back({ "F2 flip metering stores", LocateState::Ambiguous, 0, {}, {} });
                Fail(report, "F2 flip metering stores: too many stores to the state");
                return;
            }
            sites.push_back(MakePatch("F2 flip metering store", image, store.rva, std::move(replacement)));
        }
    }
    if (!unpinnable.empty())
    {
        report.patches.push_back({ "F2 flip metering stores", LocateState::AnchorFailed, 0, {}, {} });
        Fail(report, "F2 flip metering stores: " + unpinnable);
        return;
    }
    if (sites.empty())
    {
        report.patches.push_back({ "F2 flip metering stores", LocateState::Missing, 0, {}, {} });
        Fail(report, "F2 flip metering stores: no other store to the state can be patched");
        return;
    }
    for (size_t i = 0; i < sites.size(); ++i)
    {
        for (size_t j = i + 1; j < sites.size(); ++j)
        {
            if (sites[i].rva < sites[j].rva + sites[j].replacement.size() &&
                sites[j].rva < sites[i].rva + sites[i].replacement.size())
            {
                report.patches.push_back({ "F2 flip metering stores", LocateState::Ambiguous, 0, {}, {} });
                Fail(report, "F2 flip metering stores: stores overlap");
                return;
            }
        }
    }
    for (auto& site : sites)
        report.patches.push_back(std::move(site));
}
} // namespace detail

inline Report Locate(const ImageView& image)
{
    Report report;
    if (!image.IsValid())
    {
        report.failure = "sl.dlss_g must be a valid mapped AMD64 image";
        return report;
    }

    const auto ceiling = FindUniqueInImage(image, detail::kCeiling, detail::CeilingAnchor);
    if (ceiling.state == LocateState::Found)
    {
        const auto* bytes = image.DataAtRva(ceiling.rva, detail::kCeilingBytes.size());
        report.compiledCeiling = bytes != nullptr ? std::to_integer<uint8_t>(bytes[1]) : 0;
        report.patches.push_back(detail::MakePatch("C1 frame count clamp", image, ceiling.rva + 9,
                                                   FamilyRtx40::detail::Bytes({ 0xD2 })));
        if (report.patches.back().expected.empty() || report.patches.back().expected[0] != std::byte { 0xD1 })
            report.patches.back().state = LocateState::AnchorFailed;
    }
    else
    {
        report.patches.push_back({ "C1 frame count clamp", ceiling.state, 0, {}, {} });
    }
    if (report.patches.back().state != LocateState::Found)
        detail::Fail(report, "C1 frame count clamp: " + std::string(LocateStateName(report.patches.back().state)));

    detail::LocateFlip(image, report);
    return report;
}

inline std::vector<PatchOperation> BuildOperations(const Report& report, const ImageView& image)
{
    return report.Complete() ? BuildOperationsFromPatches(report.patches, image) : std::vector<PatchOperation> {};
}
} // namespace MfgUnlock::FamilyRtx40::Streamline

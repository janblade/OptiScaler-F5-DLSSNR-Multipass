#pragma once

#include "FamilyRtx40.h"

// RTX 40 side of the Streamline DLSS-G plugin (sl.dlss_g), two independent derivations that must both succeed:
//  - the frame-count clamp: `mov edx, N / cmp ecx, edx / cmovb edx, ecx` becomes `cmovb edx, edx` (one ModRM byte), so
//    the count NGX reports is not lowered back to the plugin's compiled maximum;
//  - software pacing: the plugin has a fallback for a missing flip-metering DLL that logs "FG1 DLL has been detected"
//    and writes one byte of its context. That (offset, value) pair is read from the fallback itself and every other
//    store to the offset is pinned to the same value, so hardware flip metering cannot freeze more than 2x.
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

// Decodes linearly from the start of the unwind function holding rva; true only when rva is an instruction start.
// Data embedded in the function can desynchronise the walk, which rejects the candidate (fail closed).
inline bool IsInstructionStart(const ImageView& image, uint32_t rva)
{
    FunctionRange function {};
    if (!image.FindFunctionRange(rva, function) || rva < function.begin || rva >= function.end)
        return false;
    for (uint32_t cursor = function.begin; cursor < function.end;)
    {
        if (cursor == rva)
            return true;
        if (cursor > rva)
            return false;
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
        cursor += decoded.len;
    }
    return false;
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

    // Every other store to the field in executable code is pinned to the same value.
    const uint8_t opposite = static_cast<uint8_t>(1 - wantValue);
    std::vector<LocatedPatch> sites;
    for (WORD index = 0; index < image.SectionCount(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            section.Misc.VirtualSize < kStoreLength)
            continue;
        for (uint32_t offset = 0; offset + kStoreLength <= section.Misc.VirtualSize; ++offset)
        {
            const uint32_t rva = section.VirtualAddress + offset;
            const auto* p = image.DataAtRva(rva, kStoreLength);
            if (p == nullptr)
                break;

            std::vector<std::byte> replacement(p, p + kStoreLength);
            if (p[0] == std::byte { 0xC6 })
            {
                const auto modrm = std::to_integer<uint8_t>(p[1]);
                if (!IsDisp32NoSib(modrm) || (modrm & 0x38) != 0 || ReadU32(p + 2) != wantField ||
                    std::to_integer<uint8_t>(p[6]) != opposite)
                    continue;
                replacement[6] = static_cast<std::byte>(wantValue);
            }
            else if (p[0] == std::byte { 0x40 } && p[1] == std::byte { 0x88 })
            {
                // REX exactly 0x40 keeps the store seven bytes, the length of the C6 form with the same base.
                const auto modrm = std::to_integer<uint8_t>(p[2]);
                if (!IsDisp32NoSib(modrm) || ReadU32(p + 3) != wantField)
                    continue;
                replacement[0] = std::byte { 0xC6 };
                replacement[1] = static_cast<std::byte>(0x80 | (modrm & 7));
                std::memcpy(replacement.data() + 2, &wantField, sizeof(wantField));
                replacement[6] = static_cast<std::byte>(wantValue);
            }
            else
            {
                continue;
            }

            if (!IsInstructionStart(image, rva))
                continue;
            if (sites.size() >= kMaxSites)
            {
                report.patches.push_back({ "F2 flip metering stores", LocateState::Ambiguous, 0, {}, {} });
                Fail(report, "F2 flip metering stores: too many stores to the state");
                return;
            }
            sites.push_back(MakePatch("F2 flip metering store", image, rva, std::move(replacement)));
        }
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
            if (sites[i].rva < sites[j].rva + kStoreLength && sites[j].rva < sites[i].rva + kStoreLength)
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

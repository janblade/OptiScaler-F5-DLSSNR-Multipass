// Build: cl /std:c++20 /EHsc /W4 /wd4701 tests\mfgunlock_core_smoke.cpp
// OptiScaler\framegen\mfgunlock\hde\hde64.c
#include "../OptiScaler/framegen/mfgunlock/Locator.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyXeFG.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyRtx40.h"
#include "../OptiScaler/framegen/mfgunlock/FamilySlDlssg.h"
#include "../OptiScaler/framegen/mfgunlock/FamilyFatbin.h"
#include "../OptiScaler/framegen/mfgunlock/PtxTuring.h"
#include "../OptiScaler/framegen/mfgunlock/Patch.h"
#include "../OptiScaler/framegen/mfgunlock/Status.h"
#include "../OptiScaler/framegen/mfgunlock/Mode.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace
{
int failures = 0;

void Check(bool passed, const char* message)
{
    std::printf("  %s: %s\n", passed ? "ok  " : "FAIL", message);
    if (!passed)
        ++failures;
}

bool AcceptAnchor(const MfgUnlock::ImageView&, uint32_t, void*) { return true; }

bool RejectAnchor(const MfgUnlock::ImageView&, uint32_t, void*) { return false; }

struct SyntheticImage
{
    static constexpr size_t kSize = 0x6000;
    std::byte* bytes = static_cast<std::byte*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

    SyntheticImage()
    {
        if (bytes == nullptr)
            return;

        IMAGE_DOS_HEADER dos {};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(bytes, &dos, sizeof(dos));

        const DWORD signature = IMAGE_NT_SIGNATURE;
        std::memcpy(bytes + 0x80, &signature, sizeof(signature));

        IMAGE_FILE_HEADER file {};
        file.Machine = IMAGE_FILE_MACHINE_AMD64;
        file.NumberOfSections = 3;
        file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        std::memcpy(bytes + 0x84, &file, sizeof(file));

        IMAGE_OPTIONAL_HEADER64 optional {};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = static_cast<DWORD>(kSize);
        optional.SizeOfHeaders = 0x400;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = { 0x3000, 0x200 };
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = { 0x2000, sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) };
        std::memcpy(bytes + 0x98, &optional, sizeof(optional));

        const size_t sectionOffset = 0x98 + sizeof(optional);
        IMAGE_SECTION_HEADER sections[3] {};
        std::memcpy(sections[0].Name, ".text", 5);
        sections[0].Misc.VirtualSize = 0x1000;
        sections[0].VirtualAddress = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[1].Name, ".pdata", 6);
        sections[1].Misc.VirtualSize = 0x1000;
        sections[1].VirtualAddress = 0x2000;
        sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[2].Name, ".edata", 6);
        sections[2].Misc.VirtualSize = 0x1000;
        sections[2].VirtualAddress = 0x3000;
        sections[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(bytes + sectionOffset, sections, sizeof(sections));

        const IMAGE_RUNTIME_FUNCTION_ENTRY function { 0x1000, 0x1020, 0x2100 };
        std::memcpy(bytes + 0x2000, &function, sizeof(function));

        IMAGE_EXPORT_DIRECTORY exports {};
        exports.NumberOfFunctions = 1;
        exports.NumberOfNames = 1;
        exports.AddressOfFunctions = 0x3040;
        exports.AddressOfNames = 0x3044;
        exports.AddressOfNameOrdinals = 0x3048;
        std::memcpy(bytes + 0x3000, &exports, sizeof(exports));
        const DWORD functionRva = 0x1000;
        const DWORD nameRva = 0x3050;
        const WORD ordinal = 0;
        std::memcpy(bytes + 0x3040, &functionRva, sizeof(functionRva));
        std::memcpy(bytes + 0x3044, &nameRva, sizeof(nameRva));
        std::memcpy(bytes + 0x3048, &ordinal, sizeof(ordinal));
        constexpr char exportName[] = "xefgSwapChainGetProperties";
        std::memcpy(bytes + 0x3050, exportName, sizeof(exportName));
        constexpr char anchorString[] = "Network model does not support requested multi frame generation";
        std::memcpy(bytes + 0x3080, anchorString, sizeof(anchorString));

        SetPattern(0x1000);
        SetLea(0x1008, 0x3080);
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    ~SyntheticImage()
    {
        if (bytes != nullptr)
            VirtualFree(bytes, 0, MEM_RELEASE);
    }

    void SetPattern(size_t offset)
    {
        constexpr std::array<uint8_t, 3> pattern { 0xa1, 0xb2, 0xc3 };
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, pattern.data(), pattern.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void ClearPattern(size_t offset)
    {
        const std::array<uint8_t, 3> pattern { 0xcc, 0xcc, 0xcc };
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, pattern.data(), pattern.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void SetLea(size_t offset, uint32_t targetRva)
    {
        std::array<uint8_t, 7> instruction { 0x48, 0x8d, 0x05 };
        const int32_t displacement = static_cast<int32_t>(targetRva - (offset + instruction.size()));
        std::memcpy(instruction.data() + 3, &displacement, sizeof(displacement));
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, instruction.data(), instruction.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void SetJump(size_t offset, uint32_t targetRva)
    {
        std::array<uint8_t, 5> instruction { 0xe9 };
        const int32_t displacement = static_cast<int32_t>(targetRva - (offset + instruction.size()));
        std::memcpy(instruction.data() + 1, &displacement, sizeof(displacement));
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, instruction.data(), instruction.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }
};

struct SyntheticFamilyImage
{
    static constexpr size_t kSize = 0x8000;
    static constexpr uint32_t kXeFGU3Rva = 0x1200;
    std::byte* bytes =
        static_cast<std::byte*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    uint32_t defaultCeilingRva = kXeFGU3Rva;

    explicit SyntheticFamilyImage(bool xell, uint32_t u3Rva = kXeFGU3Rva) : defaultCeilingRva(u3Rva)
    {
        if (bytes == nullptr)
            return;

        IMAGE_DOS_HEADER dos {};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(bytes, &dos, sizeof(dos));

        const DWORD signature = IMAGE_NT_SIGNATURE;
        std::memcpy(bytes + 0x80, &signature, sizeof(signature));

        IMAGE_FILE_HEADER file {};
        file.Machine = IMAGE_FILE_MACHINE_AMD64;
        file.NumberOfSections = 3;
        file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        std::memcpy(bytes + 0x84, &file, sizeof(file));

        IMAGE_OPTIONAL_HEADER64 optional {};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = static_cast<DWORD>(kSize);
        optional.SizeOfHeaders = 0x400;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] =
            { 0x5000, static_cast<DWORD>((xell ? 1 : 2) * sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) };
        if (xell)
            optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = { 0x6000, 0x200 };
        std::memcpy(bytes + 0x98, &optional, sizeof(optional));

        const size_t sectionOffset = 0x98 + sizeof(optional);
        IMAGE_SECTION_HEADER sections[3] {};
        std::memcpy(sections[0].Name, ".text", 5);
        sections[0].Misc.VirtualSize = 0x3000;
        sections[0].VirtualAddress = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[1].Name, ".pdata", 6);
        sections[1].Misc.VirtualSize = 0x1000;
        sections[1].VirtualAddress = 0x5000;
        sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[2].Name, ".edata", 6);
        sections[2].Misc.VirtualSize = 0x1000;
        sections[2].VirtualAddress = 0x6000;
        sections[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(bytes + sectionOffset, sections, sizeof(sections));

        if (xell)
        {
            const IMAGE_RUNTIME_FUNCTION_ENTRY function { 0x1100, 0x1120, 0x5100 };
            std::memcpy(bytes + 0x5000, &function, sizeof(function));
            const std::array<uint8_t, 5> jump { 0xe9 };
            std::memcpy(bytes + 0x1000, jump.data(), jump.size());
            WriteRelative(0x1000, 1, 5, 0x1100);
            std::memcpy(bytes + 0x1100, MfgUnlock::FamilyXeFG::detail::kXellPatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kXellPatternBytes.size());

            IMAGE_EXPORT_DIRECTORY exports {};
            exports.NumberOfFunctions = 1;
            exports.NumberOfNames = 1;
            exports.AddressOfFunctions = 0x6040;
            exports.AddressOfNames = 0x6044;
            exports.AddressOfNameOrdinals = 0x6048;
            std::memcpy(bytes + 0x6000, &exports, sizeof(exports));
            const DWORD functionRva = 0x1000;
            const DWORD nameRva = 0x6050;
            const WORD ordinal = 0;
            std::memcpy(bytes + 0x6040, &functionRva, sizeof(functionRva));
            std::memcpy(bytes + 0x6044, &nameRva, sizeof(nameRva));
            std::memcpy(bytes + 0x6048, &ordinal, sizeof(ordinal));
            constexpr char exportName[] = "xellSetGeneratedFramesCount";
            std::memcpy(bytes + 0x6050, exportName, sizeof(exportName));
        }
        else
        {
            const std::array<IMAGE_RUNTIME_FUNCTION_ENTRY, 2> functions {
                IMAGE_RUNTIME_FUNCTION_ENTRY { 0x1000, 0x1050, 0x5100 },
                IMAGE_RUNTIME_FUNCTION_ENTRY { 0x1100, 0x1180, 0x5110 }
            };
            std::memcpy(bytes + 0x5000, functions.data(), sizeof(functions));

            auto* u1 = bytes + 0x1000;
            std::memcpy(u1, MfgUnlock::FamilyXeFG::detail::kU1PatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kU1PatternBytes.size());
            WriteRelative(0x1000, 1, 5, 0x1500);
            WriteRelative(0x1007, 2, 6, 0x1040);
            WriteRelative(0x1010, 2, 6, 0x1040);
            constexpr std::string_view xellVersion =
                "XeLL version {}.{}.{} is too old to support multi-frame generation, falling back to 2x";
            const uint32_t stringRva = 0x6100;
            std::memcpy(bytes + stringRva, xellVersion.data(), xellVersion.size());
            bytes[stringRva + xellVersion.size()] = std::byte { 0 };
            bytes[0x1020] = std::byte { 0x48 };
            bytes[0x1021] = std::byte { 0x8d };
            bytes[0x1022] = std::byte { 0x05 };
            WriteRelative(0x1020, 3, 7, stringRva);
            const std::array<uint8_t, 6> booleanPredicate { 0xb0, 0x01, 0xc3, 0x32, 0xc0, 0xc3 };
            std::memcpy(bytes + 0x1500, booleanPredicate.data(), booleanPredicate.size());

            auto* u5Function = bytes + 0x1100;
            std::memset(u5Function, 0x90, 0x80);
            u5Function[0] = std::byte { 0xe8 };
            WriteRelative(0x1100, 1, 5, 0x1600);
            std::memcpy(bytes + 0x1140, MfgUnlock::FamilyXeFG::detail::kU5PatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kU5PatternBytes.size());

            std::memcpy(bytes + u3Rva, MfgUnlock::FamilyXeFG::detail::kU3PatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kU3PatternBytes.size());
            bytes[0x1600] = std::byte { 0xe9 };
            WriteRelative(0x1600, 1, 5, u3Rva);

            std::memcpy(bytes + 0x1300, MfgUnlock::FamilyXeFG::detail::kU4OldPatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kU4OldPatternBytes.size());
            std::memcpy(bytes + 0x1400, MfgUnlock::FamilyXeFG::detail::kU2OldPatternBytes.data(),
                        MfgUnlock::FamilyXeFG::detail::kU2OldPatternBytes.size());
        }

        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x3000, PAGE_EXECUTE_READ, &previous);
    }

    ~SyntheticFamilyImage()
    {
        if (bytes != nullptr)
            VirtualFree(bytes, 0, MEM_RELEASE);
    }

    void RenameDefaultCeiling()
    {
        DWORD previous = 0;
        VirtualProtect(bytes + defaultCeilingRva, 1, PAGE_EXECUTE_READWRITE, &previous);
        bytes[defaultCeilingRva] = std::byte { 0x49 };
        VirtualProtect(bytes + defaultCeilingRva, 1, PAGE_EXECUTE_READ, &previous);
    }

    void WriteRelative(uint32_t instructionRva, size_t displacementOffset, size_t instructionSize,
                       uint32_t targetRva)
    {
        const int64_t displacement = static_cast<int64_t>(targetRva) -
                                     (static_cast<int64_t>(instructionRva) + instructionSize);
        const int32_t relative = static_cast<int32_t>(displacement);
        std::memcpy(bytes + instructionRva + displacementOffset, &relative, sizeof(relative));
    }
};

struct FailingWriter
{
    size_t writes = 0;

    static bool Write(void* address, std::span<const std::byte> expected, std::span<const std::byte> replacement)
    {
        auto& self = Current();
        if (++self.writes == 2)
            return false;
        if (std::memcmp(address, expected.data(), expected.size()) != 0)
            return false;
        std::memcpy(address, replacement.data(), replacement.size());
        return true;
    }

    static FailingWriter& Current()
    {
        static FailingWriter writer;
        return writer;
    }
};

struct PartialWriter
{
    size_t writes = 0;
    bool failApplyAndRollback = true;

    static bool Write(void* address, std::span<const std::byte> expected, std::span<const std::byte> replacement)
    {
        auto& self = Current();
        ++self.writes;
        if (self.failApplyAndRollback && (self.writes == 2 || self.writes == 3))
            return false;
        if (std::memcmp(address, expected.data(), expected.size()) != 0)
            return false;
        std::memcpy(address, replacement.data(), replacement.size());
        return true;
    }

    static PartialWriter& Current()
    {
        static PartialWriter writer;
        return writer;
    }
};
} // namespace

int main()
{
    SyntheticImage image;
    if (image.bytes == nullptr)
    {
        std::puts("FAIL: could not allocate synthetic PE image");
        return 1;
    }

    const MfgUnlock::ImageView view(image.bytes, SyntheticImage::kSize);
    Check(view.IsValid(), "accepts a bounded AMD64 PE image");

    uint32_t exportRva = 0;
    Check(view.FindExport("xefgSwapChainGetProperties", exportRva) && exportRva == 0x1000,
          "resolves an export by its name");

    MfgUnlock::FunctionRange function {};
    Check(view.FindFunctionRange(0x1003, function) && function.begin == 0x1000 && function.end == 0x1020,
          "resolves the owning unwind function");

    const MfgUnlock::MaskedPattern pattern { "\xa1\xb2\xc3", "xxx" };
    auto result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::Found && result.rva == 0x1000,
          "finds a unique masked pattern with a passing second anchor");
    result = MfgUnlock::FindUniqueFromExport(view, "xefgSwapChainGetProperties", pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::Found && result.rva == 0x1000,
          "anchors a unique locator at an export");
    result = MfgUnlock::FindUniqueFromStringXref(
        view, "Network model does not support requested multi frame generation", pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::Found && result.rva == 0x1000,
          "anchors a unique locator at the owning function of a string xref");

    image.SetLea(0x1010, 0x3080);
    const auto ambiguousReference =
        MfgUnlock::FindUniqueStringReference(view, "Network model does not support requested multi frame generation");
    Check(ambiguousReference.state == MfgUnlock::LocateState::Ambiguous,
          "rejects a string with multiple RIP-relative LEA references");
    image.ClearPattern(0x1010);

    image.SetPattern(0x1008);
    result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::Ambiguous, "rejects an ambiguous pattern");

    image.ClearPattern(0x1000);
    image.ClearPattern(0x1008);
    result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::Missing, "reports a missing pattern");

    image.SetPattern(0x1000);
    result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, RejectAnchor);
    Check(result.state == MfgUnlock::LocateState::AnchorFailed, "rejects a pattern when the second anchor fails");

    DWORD previous = 0;
    VirtualProtect(image.bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
    result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
    Check(result.state == MfgUnlock::LocateState::NotExecutable, "rejects a non-RX executable page");
    VirtualProtect(image.bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    image.SetJump(0x1010, 0x1000);
    uint32_t branchTarget = 0;
    Check(MfgUnlock::DecodeRelativeBranchTarget(view, 0x1010, branchTarget) && branchTarget == 0x1000,
          "decodes a relative branch target within the mapped image");
    Check(MfgUnlock::BranchTargetEquals(view, 0x1010, &branchTarget),
          "uses the decoded branch target as a second anchor");

    alignas(8) std::array<uint16_t, 3> values { 0x1111, 0x2222, 0x3333 };
    std::vector<MfgUnlock::PatchOperation> operations;
    for (size_t index = 0; index < values.size(); ++index)
    {
        MfgUnlock::PatchOperation operation;
        operation.address = &values[index];
        operation.expected.resize(sizeof(values[index]));
        operation.replacement.resize(sizeof(values[index]));
        std::memcpy(operation.expected.data(), &values[index], sizeof(values[index]));
        const uint16_t replacement = static_cast<uint16_t>(values[index] + 1);
        std::memcpy(operation.replacement.data(), &replacement, sizeof(replacement));
        operations.push_back(std::move(operation));
    }

    FailingWriter::Current().writes = 0;
    const auto transaction = MfgUnlock::ApplyTransaction(operations, &FailingWriter::Write);
    Check(!transaction.applied && transaction.rollbackComplete && transaction.failedIndex == 1 &&
              values == std::array<uint16_t, 3> { 0x1111, 0x2222, 0x3333 },
          "rolls back patch one when patch two of three fails");

    MfgUnlock::AppliedPatchSet partialPatchSet;
    PartialWriter::Current() = {};
    const auto partialApply = partialPatchSet.Apply(operations, &PartialWriter::Write);
    Check(!partialApply.applied && !partialApply.rollbackComplete && partialPatchSet.IsActive() &&
              values[0] == 0x1112,
          "retains recovery data when an atomic transaction cannot fully roll back");
    PartialWriter::Current().failApplyAndRollback = false;
    PartialWriter::Current().writes = 0;
    const auto partialRestore = partialPatchSet.Restore(&PartialWriter::Write);
    Check(partialRestore.applied && !partialPatchSet.IsActive() &&
              values == std::array<uint16_t, 3> { 0x1111, 0x2222, 0x3333 },
          "reconciles a partial transaction by restoring only bytes it changed");

    MfgUnlock::PatchOperation executablePatch;
    executablePatch.address = image.bytes + 0x1000;
    executablePatch.expected = { std::byte { 0xa1 }, std::byte { 0xb2 } };
    executablePatch.replacement = { std::byte { 0xd4 }, std::byte { 0xe5 } };
    MfgUnlock::AppliedPatchSet patchSet;
    const auto executableResult = patchSet.Apply(std::span<const MfgUnlock::PatchOperation>(&executablePatch, 1));
    Check(executableResult.applied && patchSet.IsActive() && image.bytes[0x1000] == std::byte { 0xd4 } &&
              image.bytes[0x1001] == std::byte { 0xe5 },
          "atomically patches an aligned two-byte sequence on an RX page");
    const auto restoreResult = patchSet.Restore();
    Check(restoreResult.applied && !patchSet.IsActive() && image.bytes[0x1000] == std::byte { 0xa1 } &&
              image.bytes[0x1001] == std::byte { 0xb2 },
          "restores the original bytes and RX page protection");

    MfgUnlock::FamilyStatus activeStatus;
    activeStatus.state = MfgUnlock::FamilyState::Active;
    activeStatus.patches = { { "gate", "found" }, { "wrapper", "found" } };
    Check(MfgUnlock::FormatStatus(activeStatus) == "active: 2 of 2 patches",
          "formats active family status with the located patch count");

    MfgUnlock::FamilyStatus asideStatus;
    asideStatus.state = MfgUnlock::FamilyState::StoodAside;
    asideStatus.failure = "gate not found in provider.dll 1.2";
    Check(MfgUnlock::FormatStatus(asideStatus) == "stood aside: gate not found in provider.dll 1.2",
          "formats stand-aside status with the exact failed patch");

    SyntheticFamilyImage familyXeFg(false, 0x1250);
    SyntheticFamilyImage familyXeLl(true);
    const MfgUnlock::ImageView familyXeFgView(familyXeFg.bytes, SyntheticFamilyImage::kSize);
    const MfgUnlock::ImageView familyXeLlView(familyXeLl.bytes, SyntheticFamilyImage::kSize);
    auto familyReport = MfgUnlock::FamilyXeFG::Locate(familyXeFgView, familyXeLlView, 5);
    if (!familyReport.Complete())
    {
        std::printf("  synthetic family failure: %s\n", familyReport.failure.c_str());
        for (const auto& patch : familyReport.patches)
            std::printf("    %s: %.*s at 0x%X\n", patch.name.c_str(),
                        static_cast<int>(MfgUnlock::FamilyXeFG::LocateStateName(patch.state).size()),
                        MfgUnlock::FamilyXeFG::LocateStateName(patch.state).data(), patch.rva);
    }
    Check(familyReport.Complete() && familyReport.patches.size() == 6 &&
              familyReport.patches[0].replacement == std::vector<std::byte> { std::byte { 0x89 } } &&
              familyReport.patches[2].rva == 0x1250 + 14,
          "locates the XeFG family structurally after moving the default-ceiling function");
    const auto familyOperations =
        MfgUnlock::FamilyXeFG::BuildOperations(familyReport, familyXeFgView, familyXeLlView);
    MfgUnlock::AppliedPatchSet familyPatchSet;
    const auto familyApply =
        familyPatchSet.Apply(std::span<const MfgUnlock::PatchOperation>(familyOperations.data(),
                                                                         familyOperations.size()));
    Check(familyApply.applied && familyPatchSet.IsActive() && familyXeFg.bytes[0x1250 + 14] == std::byte { 0x05 } &&
              familyXeLl.bytes[0x1102] == std::byte { 0x05 },
          "applies all six XeFG and XeLL changes as one transaction");
    const auto familyRestore = familyPatchSet.Restore();
    Check(familyRestore.applied && !familyPatchSet.IsActive() && familyXeFg.bytes[0x1250 + 14] == std::byte { 0x03 } &&
              familyXeLl.bytes[0x1102] == std::byte { 0x03 },
          "restores the complete XeFG family patch transaction");

    familyXeFg.RenameDefaultCeiling();
    const MfgUnlock::ImageView renamedXeFgView(familyXeFg.bytes, SyntheticFamilyImage::kSize);
    familyReport = MfgUnlock::FamilyXeFG::Locate(renamedXeFgView, familyXeLlView, 5);
    Check(!familyReport.Complete() && familyReport.failure.find("U3 default ceiling") != std::string::npos &&
              familyReport.patches[2].state == MfgUnlock::LocateState::Missing,
          "stands aside with the named patch when a structural function changes");

    const auto writeCode = [](SyntheticImage& target, size_t offset, std::initializer_list<uint8_t> code)
    {
        DWORD previous = 0;
        VirtualProtect(target.bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        size_t cursor = offset;
        for (const uint8_t value : code)
            target.bytes[cursor++] = static_cast<std::byte>(value);
        VirtualProtect(target.bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    };
    const auto clearCode = [&](SyntheticImage& target)
    {
        for (size_t offset = 0x1000; offset < 0x1020; ++offset)
            writeCode(target, offset, { 0xcc });
    };

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        // 310.9 layout: advertise jl at +6 targets inside the function, validate setae follows.
        writeCode(rtx40Image, 0x1000, { 0x81, 0xfd, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x8c, 0x10, 0x00, 0x00, 0x00, 0xbf,
                                        0x05, 0x00, 0x00, 0x00, 0x3d, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x93, 0xc0 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(report.Complete() && report.patches.size() == 2 && report.advertiseLayout == "310.9" &&
                  report.validateLayout == "310.9",
              "locates both RTX 40 gates in the 310.9 layout");

        const auto operations = MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View);
        Check(operations.size() == 6, "RTX 40 310.9 patches change six single bytes");
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        Check(applied.applied && rtx40Image.bytes[0x1007] == std::byte { 0x1f } &&
                  rtx40Image.bytes[0x100b] == std::byte { 0x90 } && rtx40Image.bytes[0x1016] == std::byte { 0xb0 } &&
                  rtx40Image.bytes[0x1017] == std::byte { 0x01 },
              "applies the RTX 40 310.9 patches");
        const auto restored = patchSet.Restore();
        Check(restored.applied && rtx40Image.bytes[0x1007] == std::byte { 0x8c } &&
                  rtx40Image.bytes[0x1016] == std::byte { 0x0f },
              "restores the RTX 40 310.9 patches");

        // Branch leaving the function fails the structural anchor.
        writeCode(rtx40Image, 0x1008, { 0x00, 0x10, 0x00, 0x00 });
        Check(!MfgUnlock::FamilyRtx40::Locate(rtx40View).Complete(),
              "RTX 40 stands aside when the advertise branch leaves the function");
    }

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        writeCode(rtx40Image, 0x1000, { 0xbb, 0x01, 0x00, 0x00, 0x00, 0x41, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x81, 0xff,
                                        0xb0, 0x01, 0x00, 0x00, 0x44, 0x0f, 0x4c, 0xc3, 0x3d, 0xb0, 0x01, 0x00, 0x00,
                                        0x7c, 0x05, 0x83, 0xfb, 0x03, 0x76 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(report.Complete() && report.patches.size() == 4 && report.advertiseLayout == "legacy" &&
                  report.validateLayout == "legacy",
              "locates both RTX 40 gates in the legacy layout");
        const auto operations = MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View);
        Check(!operations.empty() && operations.size() == 1 + 4 + 2 + 1,
              "RTX 40 legacy patches change eight single bytes");
    }

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        writeCode(rtx40Image, 0x1000, { 0x3d, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x93, 0xc0 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(!report.Complete() && report.failure.find("A1 advertise gate") != std::string::npos &&
                  MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View).empty(),
              "RTX 40 stands aside and writes nothing when the advertise gate is missing");
    }

    {
        SyntheticImage slImage;
        const MfgUnlock::ImageView slView(slImage.bytes, SyntheticImage::kSize);
        clearCode(slImage);
        constexpr char marker[] = "FG1 DLL has been detected: forcing flip-metering off.";
        std::memcpy(slImage.bytes + 0x3100, marker, sizeof(marker));
        writeCode(slImage, 0x1000, { 0xba, 0x05, 0x00, 0x00, 0x00, 0x3b, 0xca, 0x0f, 0x42, 0xd1 });
        slImage.SetLea(0x100a, 0x3100);
        // The fallback's own store (state 1) and one register store that has to be pinned to the same state.
        writeCode(slImage, 0x1011, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 });
        writeCode(slImage, 0x1018, { 0x40, 0x88, 0xbb, 0xf8, 0x44, 0x00, 0x00 });

        const auto report = MfgUnlock::FamilyRtx40::Streamline::Locate(slView);
        Check(report.Complete() && report.patches.size() == 2 && report.compiledCeiling == 5 &&
                  report.flipField == 0x44f8 && report.flipValue == 1,
              "locates the sl.dlss_g clamp and derives the flip metering state from the fallback");

        const auto operations = MfgUnlock::FamilyRtx40::Streamline::BuildOperations(report, slView);
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        const std::array<uint8_t, 7> pinned { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 };
        bool pinnedOk = true;
        for (size_t index = 0; index < pinned.size(); ++index)
            pinnedOk = pinnedOk && slImage.bytes[0x1018 + index] == static_cast<std::byte>(pinned[index]);
        Check(applied.applied && slImage.bytes[0x1009] == std::byte { 0xd2 } && pinnedOk,
              "applies the sl.dlss_g clamp and pins the flip metering store");
        const auto restored = patchSet.Restore();
        Check(restored.applied && slImage.bytes[0x1009] == std::byte { 0xd1 } &&
                  slImage.bytes[0x1018] == std::byte { 0x40 } && slImage.bytes[0x101e] == std::byte { 0x00 },
              "restores the sl.dlss_g patches");

        // A fallback state that disagrees fails closed.
        writeCode(slImage, 0x1017, { 0x00 });
        writeCode(slImage, 0x1011, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x00 });
        writeCode(slImage, 0x1018, { 0xc6, 0x83, 0xf0, 0x44, 0x00, 0x00, 0x01 });
        Check(!MfgUnlock::FamilyRtx40::Streamline::Locate(slView).Complete(),
              "sl.dlss_g stands aside when no other store can be pinned");
    }

    {
        SyntheticImage slImage;
        const MfgUnlock::ImageView slView(slImage.bytes, SyntheticImage::kSize);
        clearCode(slImage);
        writeCode(slImage, 0x1000, { 0xba, 0x05, 0x00, 0x00, 0x00, 0x3b, 0xca, 0x0f, 0x42, 0xd1 });
        const auto report = MfgUnlock::FamilyRtx40::Streamline::Locate(slView);
        Check(!report.Complete() && report.failure.find("F1 flip metering fallback") != std::string::npos &&
                  MfgUnlock::FamilyRtx40::Streamline::BuildOperations(report, slView).empty(),
              "sl.dlss_g stands aside and writes nothing when the fallback message is missing");
    }

    {
        // Midpoint fix: one container holding an Ada cubin, a Blackwell PTX and an Ada PTX.
        SyntheticImage image;
        const MfgUnlock::ImageView view(image.bytes, SyntheticImage::kSize);
        const auto put = [&](size_t offset, const void* data, size_t size) { std::memcpy(image.bytes + offset, data, size); };
        const auto writeImage = [&](size_t offset, uint16_t kind, uint64_t payload, uint32_t arch, const char* text) {
            const uint32_t header = 32;
            put(offset, &kind, sizeof(kind));
            put(offset + 4, &header, sizeof(header));
            put(offset + 8, &payload, sizeof(payload));
            put(offset + 28, &arch, sizeof(arch));
            if (text != nullptr)
                put(offset + header, text, std::strlen(text));
        };
        constexpr size_t base = 0x3200;
        const uint8_t magic[] = { 0x50, 0xed, 0x55, 0xba, 0x01, 0x00, 0x10, 0x00 };
        const uint64_t fatSize = 3 * 32 + 8 + 24 + 8;
        put(base, magic, sizeof(magic));
        put(base + 8, &fatSize, sizeof(fatSize));
        writeImage(base + 16, 2, 8, 89, nullptr);
        writeImage(base + 16 + 40, 1, 24, 120, ".target sm_120\n.version");
        writeImage(base + 16 + 40 + 56, 1, 8, 89, ".target ");

        const auto report = MfgUnlock::FamilyRtx40::Fatbin::Locate(view);
        Check(report.Complete() && report.containers == 1 && report.patches.size() == 4,
              "locates a fatbin container with a Blackwell PTX and Ada images");
        const auto operations = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(report, view);
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        uint32_t blackwellArch = 0;
        uint32_t adaArch = 0;
        std::memcpy(&blackwellArch, image.bytes + base + 16 + 40 + 28, sizeof(blackwellArch));
        std::memcpy(&adaArch, image.bytes + base + 16 + 28, sizeof(adaArch));
        Check(applied.applied && blackwellArch == 89 && adaArch == 122 &&
                  std::memcmp(image.bytes + base + 16 + 40 + 32, ".target sm_89 \n", 15) == 0,
              "relabels the Blackwell PTX as sm_89 and parks the Ada images");
        const auto restored = patchSet.Restore();
        std::memcpy(&blackwellArch, image.bytes + base + 16 + 40 + 28, sizeof(blackwellArch));
        Check(restored.applied && blackwellArch == 120 &&
                  std::memcmp(image.bytes + base + 16 + 40 + 32, ".target sm_120\n", 15) == 0,
              "restores the fatbin container");

        // ".target sm_120a" is a different target and must not be rewritten.
        put(base + 16 + 40 + 32 + 14, "a", 1);
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(view).Complete(),
              "leaves a container whose PTX targets sm_120a alone");
    }

    {
        const auto minmax = MfgUnlock::PtxTuring::RewriteText("{min.f16 %h1, %h2, %h3;}\n{max.f16x2 %r1, %r2, %r3;}\n");
        Check(minmax.ok && minmax.minMaxScalar == 1 && minmax.minMaxVec == 1 &&
                  minmax.ptx.find("setp.nan.f16") != std::string::npos,
              "min/max rewrite keeps PTX's NaN operand rule");

        std::array<uint8_t, 0x10 + 0x40 + 8> container{};
        const uint32_t magic = MfgUnlock::PtxTuring::kFatbinMagic;
        const uint16_t headerSize = 0x10;
        const uint64_t fatSize = 0x48;
        std::memcpy(container.data(), &magic, 4);
        std::memcpy(container.data() + 6, &headerSize, 2);
        std::memcpy(container.data() + 8, &fatSize, 8);
        const uint16_t kind = 1;
        const uint32_t entryHeader = 0x40;
        const uint64_t wrapped = 0xFFFFFFFFFFFFFFF0ull;
        const uint32_t arch = 89;
        std::memcpy(container.data() + 0x10, &kind, 2);
        std::memcpy(container.data() + 0x10 + 4, &entryHeader, 4);
        std::memcpy(container.data() + 0x10 + 8, &wrapped, 8);
        std::memcpy(container.data() + 0x10 + 0x1C, &arch, 4);
        Check(!MfgUnlock::PtxTuring::BuildFromContainer(container.data()).ok,
              "rejects a container entry whose payload size wraps");

        const uint64_t onePayload = 1;
        const uint32_t compressed = 1;
        const uint64_t uncompressed = 16;
        container[0x10 + 0x40] = 0x28;
        std::memcpy(container.data() + 0x10 + 8, &onePayload, 8);
        std::memcpy(container.data() + 0x10 + 16, &compressed, 4);
        std::memcpy(container.data() + 0x10 + 0x38, &uncompressed, 8);
        Check(!MfgUnlock::PtxTuring::BuildFromContainer(container.data()).ok,
              "rejects a one-byte compressed payload without reading past it");
    }

    {
        // RTX 30 retarget of an LZ4-compressed Ada PTX (0x40-byte image header with the payload sizes).
        const auto writeCompressedContainer = [](SyntheticImage& target, size_t base, const std::vector<uint8_t>& stream,
                                                 uint64_t uncompressed)
        {
            const auto put = [&](size_t offset, const void* data, size_t size)
            { std::memcpy(target.bytes + offset, data, size); };
            std::memset(target.bytes + base, 0, 0x200);
            const uint8_t magic[] = { 0x50, 0xed, 0x55, 0xba, 0x01, 0x00, 0x10, 0x00 };
            const uint64_t payload = (stream.size() + 7) & ~size_t { 7 };
            const uint64_t fatSize = 0x40 + payload;
            put(base, magic, sizeof(magic));
            put(base + 8, &fatSize, sizeof(fatSize));
            const uint16_t kind = 1;
            const uint32_t header = 0x40;
            const uint32_t compressed = static_cast<uint32_t>(stream.size());
            const uint32_t arch = 89;
            put(base + 16, &kind, sizeof(kind));
            put(base + 16 + 4, &header, sizeof(header));
            put(base + 16 + 8, &payload, sizeof(payload));
            put(base + 16 + 16, &compressed, sizeof(compressed));
            put(base + 16 + 28, &arch, sizeof(arch));
            put(base + 16 + 0x38, &uncompressed, sizeof(uncompressed));
            put(base + 16 + 0x40, stream.data(), stream.size());
        };
        constexpr size_t base = 0x3400;
        constexpr size_t body = base + 16 + 0x40;

        SyntheticImage literalImage;
        const MfgUnlock::ImageView literalView(literalImage.bytes, SyntheticImage::kSize);
        // One literal run of 17 bytes: token 0xF0, length extension 2.
        const std::string text = ".target sm_89\nabc";
        std::vector<uint8_t> literal = { 0xF0, 0x02 };
        literal.insert(literal.end(), text.begin(), text.end());
        writeCompressedContainer(literalImage, base, literal, text.size());
        const auto retarget =
            MfgUnlock::FamilyRtx40::Fatbin::Locate(literalView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
        MfgUnlock::AppliedPatchSet retargetSet;
        const bool applied = retarget.Complete() && retarget.containers == 1 &&
                             retargetSet.Apply(MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(retarget, literalView))
                                 .applied;
        std::vector<uint8_t> decoded;
        const bool decodes = MfgUnlock::PtxTuring::detail::Lz4Decompress(
            reinterpret_cast<const uint8_t*>(literalImage.bytes + body), literal.size(), decoded, text.size());
        Check(applied && decodes && std::string(decoded.begin(), decoded.end()) == ".target sm_86\nabc",
              "retargets a compressed PTX whose .target sits in one literal run");
        retargetSet.Restore();

        // The same bytes as literals, then a match copying them: the edit would change two directives.
        SyntheticImage copiedImage;
        const MfgUnlock::ImageView copiedView(copiedImage.bytes, SyntheticImage::kSize);
        const std::string directive = ".target sm_89\n";
        std::vector<uint8_t> copied = { 0xEA };
        copied.insert(copied.end(), directive.begin(), directive.end());
        copied.push_back(0x0E);
        copied.push_back(0x00);
        writeCompressedContainer(copiedImage, base, copied, directive.size() * 2);
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(copiedView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere)
                   .Complete(),
              "rejects a compressed PTX whose .target bytes are copied again by a later match");

        // A stream that does not decode is rejected too.
        std::vector<uint8_t> broken = literal;
        broken[1] = 0x09; // literal run longer than the stream
        SyntheticImage brokenImage;
        const MfgUnlock::ImageView brokenView(brokenImage.bytes, SyntheticImage::kSize);
        writeCompressedContainer(brokenImage, base, broken, text.size());
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(brokenView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere)
                   .Complete(),
              "rejects a compressed PTX that does not decode");
    }

    {
        bool valid = false;
        const bool autoIsDefault = !MfgUnlock::ParseMode("auto", valid).has_value() && valid &&
                                   !MfgUnlock::ParseMode("AUTO", valid).has_value() && valid &&
                                   !MfgUnlock::ParseMode("", valid).has_value() && valid;
        const bool detect = MfgUnlock::ParseMode("Detect", valid) == MfgUnlockMode::Detect && valid;
        const bool onOff = MfgUnlock::ParseMode("true", valid) == MfgUnlockMode::On &&
                           MfgUnlock::ParseMode("on", valid) == MfgUnlockMode::On &&
                           MfgUnlock::ParseMode("false", valid) == MfgUnlockMode::Off &&
                           MfgUnlock::ParseMode("OFF", valid) == MfgUnlockMode::Off && valid;
        const bool invalid = !MfgUnlock::ParseMode("yes", valid).has_value() && !valid;
        Check(autoIsDefault && detect && onOff && invalid,
              "config: auto is the default (off), detect is its own mode, junk is rejected");
        Check(std::string(MfgUnlock::ModeText(std::nullopt)) == "auto" &&
                  std::string(MfgUnlock::ModeText(MfgUnlockMode::Detect)) == "detect" &&
                  std::string(MfgUnlock::ModeText(MfgUnlockMode::On)) == "true",
              "config: modes save as auto, detect and true");
    }

    return failures == 0 ? 0 : 1;
}

#pragma once

// The parts of misc/REFrameworkCompat.h that need nothing else of OptiScaler (tests/reframework_compat_smoke.cpp
// checks them alone): whether the switch is on, and whether a DLL on disk exports a given function.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace REFrameworkCompatCore
{

// The function a REFramework build that follows XeFG exports (the onehoon fork, Release 13 and later). Called before
// XeFG's swapchain is torn down so REFramework lets go of it first. Arguments: XeFG's public swapchain (the one the game
// presents to), XeFG's swapchain context, the window.
inline constexpr char kPreRetireExport[] = "REFramework_XeFG_PreRetireSwapchainV1";
using PreRetireFn = uint32_t(WINAPI*)(IUnknown* publicProxy, void* xefgContext, HWND hwnd);

// What it answers. Any other value is treated as Blocked: an unknown answer from a later version is not trusted.
enum class PreRetireResult : uint32_t
{
    NotTracked = 0, // REFramework was not drawing on this swapchain
    Detached = 1,   // it was, and it let go
    Blocked = 2,    // it could not let go
};

// [Hotfix] REFrameworkCompat: true / false as set, auto (no value) = on for a game with the quirk where the REFramework
// fork was found.
inline bool Resolve(std::optional<bool> ini, bool quirk, bool forkFound)
{
    if (ini.has_value())
        return *ini;

    return quirk && forkFound;
}

namespace detail
{
// The file offset of an RVA, through the section that holds it. Returns false outside every section or the file.
inline bool RvaToOffset(const IMAGE_SECTION_HEADER* sections, unsigned count, uint32_t rva, size_t fileSize,
                        size_t* offset)
{
    for (unsigned i = 0; i < count; i++)
    {
        const auto& s = sections[i];
        const uint32_t size = s.SizeOfRawData > s.Misc.VirtualSize ? s.SizeOfRawData : s.Misc.VirtualSize;

        if (rva < s.VirtualAddress || rva - s.VirtualAddress >= size)
            continue;

        const size_t at = (size_t) s.PointerToRawData + (rva - s.VirtualAddress);

        if (at >= fileSize)
            return false;

        *offset = at;
        return true;
    }

    return false;
}
} // namespace detail

// Whether the PE image in `data` (a whole file) names `exportName` in its export table. Reads the file only: nothing is
// loaded or run. False for anything that is not a well-formed PE.
inline bool ImageExports(const uint8_t* data, size_t size, const char* exportName)
{
    if (data == nullptr || size < sizeof(IMAGE_DOS_HEADER))
        return false;

    const auto dos = (const IMAGE_DOS_HEADER*) data;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (size_t) dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > size)
        return false;

    const auto nt = (const IMAGE_NT_HEADERS32*) (data + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_DATA_DIRECTORY* exportDir = nullptr;
    const uint8_t* optional = (const uint8_t*) &nt->OptionalHeader;

    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        if ((size_t) dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > size)
            return false;

        const auto oh = (const IMAGE_OPTIONAL_HEADER64*) optional;

        if (oh->NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;

        exportDir = &oh->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;

        exportDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }
    else
    {
        return false;
    }

    if (exportDir->VirtualAddress == 0 || exportDir->Size == 0)
        return false;

    const size_t sectionsAt = (size_t) dos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                              nt->FileHeader.SizeOfOptionalHeader;
    const unsigned sectionCount = nt->FileHeader.NumberOfSections;

    if (sectionsAt + (size_t) sectionCount * sizeof(IMAGE_SECTION_HEADER) > size)
        return false;

    const auto sections = (const IMAGE_SECTION_HEADER*) (data + sectionsAt);

    size_t dirAt = 0;

    if (!detail::RvaToOffset(sections, sectionCount, exportDir->VirtualAddress, size, &dirAt) ||
        dirAt + sizeof(IMAGE_EXPORT_DIRECTORY) > size)
        return false;

    const auto dir = (const IMAGE_EXPORT_DIRECTORY*) (data + dirAt);
    size_t namesAt = 0;

    if (dir->NumberOfNames == 0 ||
        !detail::RvaToOffset(sections, sectionCount, dir->AddressOfNames, size, &namesAt) ||
        namesAt + (size_t) dir->NumberOfNames * sizeof(uint32_t) > size)
        return false;

    const size_t wantLength = strlen(exportName);

    for (uint32_t i = 0; i < dir->NumberOfNames; i++)
    {
        uint32_t nameRva = 0;
        memcpy(&nameRva, data + namesAt + (size_t) i * sizeof(uint32_t), sizeof(nameRva));

        size_t nameAt = 0;

        if (!detail::RvaToOffset(sections, sectionCount, nameRva, size, &nameAt) || nameAt + wantLength + 1 > size)
            continue;

        if (memcmp(data + nameAt, exportName, wantLength + 1) == 0)
            return true;
    }

    return false;
}

// ImageExports on a file, mapped read-only. False when it cannot be opened.
inline bool FileExports(const std::wstring& path, const char* exportName)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize {};
    bool found = false;

    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart > 0 && fileSize.QuadPart < (1ll << 31))
    {
        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);

        if (mapping != nullptr)
        {
            const auto view = (const uint8_t*) MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);

            if (view != nullptr)
            {
                found = ImageExports(view, (size_t) fileSize.QuadPart, exportName);
                UnmapViewOfFile(view);
            }

            CloseHandle(mapping);
        }
    }

    CloseHandle(file);
    return found;
}

} // namespace REFrameworkCompatCore

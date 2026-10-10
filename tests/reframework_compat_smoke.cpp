// Host check of misc/REFrameworkCompatCore.h: the [Hotfix] REFrameworkCompat decision and the on-disk export check
// that finds the REFramework fork in dinput8.dll. No game, no GPU. This exe exports the fork's function itself, so it
// is its own "fork" file.
// cl /std:c++20 /EHsc /W4 tests/reframework_compat_smoke.cpp
#include "../OptiScaler/misc/REFrameworkCompatCore.h"

#include <cstdio>
#include <random>
#include <vector>

using namespace REFrameworkCompatCore;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

extern "C" __declspec(dllexport) uint32_t WINAPI REFramework_XeFG_PreRetireSwapchainV1(IUnknown*, void*, HWND)
{
    return 0;
}

static std::vector<uint8_t> ReadAll(const std::wstring& path)
{
    std::vector<uint8_t> data;
    FILE* f = nullptr;

    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return data;

    fseek(f, 0, SEEK_END);
    data.resize((size_t) ftell(f));
    fseek(f, 0, SEEK_SET);
    data.resize(fread(data.data(), 1, data.size(), f));
    fclose(f);
    return data;
}

int main()
{
    // The switch: ini wins, auto needs the quirk and the fork
    CHECK(Resolve(true, false, false));
    CHECK(Resolve(true, true, true));
    CHECK(!Resolve(false, true, true));
    CHECK(!Resolve(false, false, false));
    CHECK(Resolve(std::nullopt, true, true));
    CHECK(!Resolve(std::nullopt, true, false));
    CHECK(!Resolve(std::nullopt, false, true));
    CHECK(!Resolve(std::nullopt, false, false));

    wchar_t self[MAX_PATH] {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    // This exe exports the fork's function (PE32+)
    CHECK(FileExports(self, kPreRetireExport));
    CHECK(!FileExports(self, "REFramework_XeFG_PreRetireSwapchainV2"));
    CHECK(!FileExports(self, "REFramework_XeFG_PreRetireSwapchainV")); // a prefix is not a match

    // A real system DLL, 64-bit and 32-bit
    wchar_t system[MAX_PATH] {};
    GetSystemDirectoryW(system, MAX_PATH);
    CHECK(FileExports(std::wstring(system) + L"\\kernel32.dll", "CreateFileW"));
    CHECK(!FileExports(std::wstring(system) + L"\\kernel32.dll", kPreRetireExport));
    CHECK(!FileExports(std::wstring(system) + L"\\dinput8.dll", kPreRetireExport)); // Windows' own dinput8

    wchar_t windows[MAX_PATH] {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring wow = std::wstring(windows) + L"\\SysWOW64\\kernel32.dll";

    if (GetFileAttributesW(wow.c_str()) != INVALID_FILE_ATTRIBUTES)
        CHECK(FileExports(wow, "CreateFileW"));

    // Not there, not a PE
    CHECK(!FileExports(L"Z:\\no\\such\\dinput8.dll", kPreRetireExport));
    CHECK(!FileExports(L"tests\\reframework_compat_smoke.cpp", kPreRetireExport));
    CHECK(!ImageExports(nullptr, 0, kPreRetireExport));

    // Cut short anywhere: never a match past the end, never a read past it (an access violation fails the test)
    const auto image = ReadAll(self);
    CHECK(image.size() > 4096);
    CHECK(ImageExports(image.data(), image.size(), kPreRetireExport));

    for (size_t cut = 0; cut < image.size(); cut += (cut < 4096 ? 7 : 4093))
    {
        std::vector<uint8_t> part(image.begin(), image.begin() + cut); // its own allocation: reads past it show up
        ImageExports(part.data(), part.size(), kPreRetireExport);
    }

    // Corrupted headers and tables: no crash
    std::mt19937 rng(12345);

    for (int round = 0; round < 3000; round++)
    {
        auto broken = image;
        const int flips = 1 + (int) (rng() % 8);

        for (int i = 0; i < flips; i++)
        {
            // Mostly in the headers, where the offsets live
            const size_t at = (rng() % 4 != 0) ? rng() % 1024 : rng() % broken.size();
            broken[at] = (uint8_t) rng();
        }

        ImageExports(broken.data(), broken.size(), kPreRetireExport);
    }

    if (fails == 0)
        printf("reframework_compat_smoke: all checks passed\n");

    return fails == 0 ? 0 : 1;
}

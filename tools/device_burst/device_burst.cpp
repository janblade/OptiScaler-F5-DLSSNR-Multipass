// Test-only plugin, never shipped or packaged: a "device burst" for OptiScaler's ASI plugin loading. About ten seconds after it
// is loaded, on a thread of its own, it makes and immediately destroys what AC Odyssey does after its swapchain:
//   - a D3D11 device at each feature level 11_0, 10_1, 10_0, 9_3, 9_2, 9_1 (in a dxvk game these go to dxvk, which makes a
//     Vulkan device for each),
//   - seven D3D12 devices.
// Then, unless the environment variable DEVICE_BURST_ONCE is set, once more ten seconds later. What it did, and what each call
// returned, goes to device_burst.log beside the game's exe.
//
// Build (vcvars64, from this folder):
//   cl /nologo /std:c++20 /EHsc /O2 /LD device_burst.cpp /Fe:device_burst.asi d3d11.lib d3d12.lib dxgi.lib user32.lib
//
// Use: copy device_burst.asi into the "plugins" folder beside OptiScaler's DLL (OptiScaler.ini [Plugins] Path=auto, the
// default, means <folder of the OptiScaler DLL>\plugins) and set in OptiScaler.ini
//   [Plugins]
//   LoadAsiPlugins=true

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{

FILE* g_log = nullptr;

void Log(const char* format, ...)
{
    if (g_log == nullptr)
        return;

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);

    fputc('\n', g_log);
    fflush(g_log);
}

void Burst(int round)
{
    Log("round %d: start", round);

    const struct
    {
        D3D_FEATURE_LEVEL level;
        const char* name;
    } levels[] = { { D3D_FEATURE_LEVEL_11_0, "11_0" }, { D3D_FEATURE_LEVEL_10_1, "10_1" },
                   { D3D_FEATURE_LEVEL_10_0, "10_0" }, { D3D_FEATURE_LEVEL_9_3, "9_3" },
                   { D3D_FEATURE_LEVEL_9_2, "9_2" },   { D3D_FEATURE_LEVEL_9_1, "9_1" } };

    for (const auto& l : levels)
    {
        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL got = {};
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &l.level, 1, D3D11_SDK_VERSION,
                                             &device, &got, nullptr);
        Log("D3D11CreateDevice FL %s -> 0x%08X (device %p, got FL 0x%X)", l.name, (unsigned) hr, (void*) device.Get(),
            (unsigned) got);
        device.Reset();
        Log("  destroyed");
        Sleep(50);
    }

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;

    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        factory->EnumAdapters(0, &adapter);

    for (int i = 0; i < 7; ++i)
    {
        ComPtr<ID3D12Device> device;
        const HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        Log("D3D12CreateDevice #%d -> 0x%08X (device %p)", i + 1, (unsigned) hr, (void*) device.Get());
        device.Reset();
        Log("  destroyed");
        Sleep(50);
    }

    Log("round %d: done", round);
}

DWORD WINAPI Run(LPVOID)
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path = exe;
    const auto slash = path.find_last_of("\\/");
    path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "device_burst.log";
    fopen_s(&g_log, path.c_str(), "a");
    Log("loaded in %s; first burst in 10 s", exe);

    Sleep(10000);
    Burst(1);

    char once[8] = {};

    if (GetEnvironmentVariableA("DEVICE_BURST_ONCE", once, sizeof(once)) == 0)
    {
        Sleep(10000);
        Burst(2);
    }

    Log("finished");
    fclose(g_log);
    g_log = nullptr;
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        // The thread starts after the loader lock is let go
        if (HANDLE thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr); thread != nullptr)
            CloseHandle(thread);
    }

    return TRUE;
}

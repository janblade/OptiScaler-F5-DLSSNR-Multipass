#pragma once
#include "SysUtils.h"

#include <filesystem>

#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>

namespace Util
{
struct MonitorInfo
{
    HMONITOR handle;
    int x;
    int y;
    int width;
    int height;
    RECT monitorRect;  // full monitor bounds
    RECT workRect;     // work area (taskbar excluded)
    std::wstring name; // e.g., \\.\DISPLAY1
};

struct Luid
{
    DWORD LowPart;
    LONG HighPart;

    auto operator<=>(const Luid&) const = default;
};

std::filesystem::path ExePath();
std::filesystem::path DllPath();
std::optional<std::filesystem::path> NvngxPath();
std::filesystem::path ReShadePath();

double MillisecondsNow();
std::wstring ToLower(std::wstring value);

HWND GetProcessWindow();
bool GetFileVersion(std::wstring dllPath, version_t* fileVersionOut, version_t* productVersionOut = nullptr);
bool IsSubpath(const std::filesystem::path& path, const std::filesystem::path& base);
bool GetRealWindowsVersion(OSVERSIONINFOW& osInfo);
std::string GetWindowsName(const OSVERSIONINFOW& os);
void GetExeInfo();
std::wstring GetWindowTitle(HWND hwnd);
std::optional<std::filesystem::path> FindFilePath(const std::filesystem::path& startDir,
                                                  const std::filesystem::path& fileName);
std::string WhoIsTheCaller(void* returnAddress);
HMODULE GetCallerModule(void* returnAddress);
std::filesystem::path ModulePath(HMODULE module);
MonitorInfo GetMonitorInfoForWindow(HWND hwnd);
MonitorInfo GetMonitorInfoForOutput(IDXGIOutput* pOutput);
int GetActiveRefreshRate(HWND hwnd);
bool CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject);
void GetDeviceRemovedReason(ID3D11Device* pDevice);
void GetDeviceRemovedReason(ID3D12Device* pDevice);
void LoadProxyLibrary(const std::wstring& name, const std::wstring& optiPath, const std::wstring& overridePath,
                      HMODULE* memoryModule, HMODULE* loadedModule);

std::map<Luid, std::filesystem::path> GetDriverStore();

// Wall clock in ns (100 ns resolution), for the low latency layer's timing
uint64_t GetTimestamp();

void SetFlagEnvVar(const wchar_t* name, bool enabled);

template <typename T> void DelayedDestroy(std::unique_ptr<T> ptr)
{
    std::thread([p = std::move(ptr)]() mutable { std::this_thread::sleep_for(std::chrono::seconds(2)); }).detach();
}

// Releases a COM object (a texture, a heap) once the GPU can no longer be using it, by the same delay
struct ReleaseLater
{
    explicit ReleaseLater(IUnknown* object) : object(object) {}
    ~ReleaseLater() { object->Release(); }
    ReleaseLater(const ReleaseLater&) = delete;
    ReleaseLater& operator=(const ReleaseLater&) = delete;
    IUnknown* object;
};

inline void DelayedRelease(IUnknown* object)
{
    if (object != nullptr)
        DelayedDestroy(std::make_unique<ReleaseLater>(object));
}

}; // namespace Util

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        // Set a breakpoint on this line to catch DirectX API errors
        throw std::exception();
    }
}

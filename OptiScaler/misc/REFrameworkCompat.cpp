#include "pch.h"
#include "REFrameworkCompat.h"
#include "REFrameworkCompatCore.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <proxies/KernelBase_Proxy.h>

#include <tlhelp32.h>

#include <atomic>
#include <mutex>

namespace
{

using namespace REFrameworkCompatCore;

std::atomic_bool g_active { false };

// Auto with the quirk and no REFramework found at startup: Active() looks again.
std::atomic_bool g_lookAgain { false };
std::atomic<ULONGLONG> g_lastLook { 0 };
constexpr ULONGLONG kLookInterval = 3000;

std::mutex g_lookupMutex;
PreRetireFn g_preRetire = nullptr;

// A loaded module exporting REFramework's function, or null. Holds g_lookupMutex.
PreRetireFn FindPreRetireLocked()
{
    if (g_preRetire != nullptr)
        return g_preRetire;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (auto proc = KernelBaseProxy::GetProcAddress_()(entry.hModule, kPreRetireExport); proc != nullptr)
            {
                g_preRetire = (PreRetireFn) proc;
                LOG_INFO("REFramework compatibility: {} found in {}", kPreRetireExport,
                         wstring_to_string(entry.szModule));
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return g_preRetire;
}

} // namespace

namespace REFrameworkCompat
{

void Init(bool quirk)
{
    const auto ini = Config::Instance()->REFrameworkCompat.has_value()
                         ? std::optional<bool>(Config::Instance()->REFrameworkCompat.value())
                         : std::nullopt;

    // The fork is dinput8.dll beside the exe; it may load after OptiScaler, so the file is read, not the module
    const auto dinput8 = Util::ExePath().parent_path() / L"dinput8.dll";
    const bool forkOnDisk = quirk && !ini.has_value() && FileExports(dinput8.wstring(), kPreRetireExport);

    g_active = Resolve(ini, quirk, forkOnDisk);
    g_lookAgain = !ini.has_value() && quirk && !forkOnDisk;

    if (ini.has_value())
        LOG_INFO("REFramework compatibility: {} (set in the ini)", *ini ? "on" : "off");
    else if (!quirk)
        LOG_DEBUG("REFramework compatibility: off (auto, not an RE Engine game)");
    else if (forkOnDisk)
        LOG_INFO("REFramework compatibility: on (auto, dinput8.dll is a REFramework that follows XeFG)");
    else
        LOG_INFO("REFramework compatibility: off for now (auto, no REFramework that follows XeFG beside the exe; "
                 "checked again at swapchain teardown)");
}

bool Active()
{
    if (g_active.load(std::memory_order_acquire))
        return true;

    if (!g_lookAgain.load(std::memory_order_acquire))
        return false;

    const auto now = GetTickCount64();
    auto last = g_lastLook.load(std::memory_order_relaxed);

    if (now - last < kLookInterval || !g_lastLook.compare_exchange_strong(last, now))
        return false;

    std::lock_guard lock(g_lookupMutex);

    if (FindPreRetireLocked() == nullptr)
        return false;

    g_lookAgain = false;
    g_active = true;
    LOG_INFO("REFramework compatibility: on (auto, a loaded REFramework follows XeFG)");
    return true;
}

void BeforeXeFGSwapchainRetire(IUnknown* publicProxy, void* xefgContext, HWND hwnd)
{
    if (!Active())
        return;

    // REFramework's handoff takes its own locks and may already be gone at exit; the fork's author skips it there too
    if (State::Instance().isShuttingDown)
    {
        LOG_DEBUG("REFramework compatibility: shutting down, REFramework not asked to let go of XeFG's swapchain");
        return;
    }

    if (publicProxy == nullptr || xefgContext == nullptr)
    {
        LOG_DEBUG("REFramework compatibility: no XeFG swapchain to hand back (proxy {:X}, context {:X})",
                  (size_t) publicProxy, (size_t) xefgContext);
        return;
    }

    PreRetireFn preRetire = nullptr;

    {
        std::lock_guard lock(g_lookupMutex);
        preRetire = FindPreRetireLocked();
    }

    if (preRetire == nullptr)
    {
        LOG_DEBUG("REFramework compatibility: no loaded REFramework exports {}", kPreRetireExport);
        return;
    }

    const auto result = preRetire(publicProxy, xefgContext, hwnd);

    switch (result)
    {
    case (uint32_t) PreRetireResult::NotTracked:
        LOG_INFO("REFramework compatibility: REFramework was not drawing on XeFG's swapchain {:X}",
                 (size_t) publicProxy);
        break;

    case (uint32_t) PreRetireResult::Detached:
        LOG_INFO("REFramework compatibility: REFramework let go of XeFG's swapchain {:X}", (size_t) publicProxy);
        break;

    default:
        // Tearing down anyway is what happened before this switch existed: never worse than without it
        LOG_ERROR("REFramework compatibility: REFramework could not let go of XeFG's swapchain {:X} (answer {}); "
                  "tearing it down anyway",
                  (size_t) publicProxy, result);
        break;
    }
}

} // namespace REFrameworkCompat

#pragma once
#include "SysUtils.h"
#include <misc/DeferredModuleScan.h>
#include <misc/LoaderBoundary.h>

class LibraryLoadHooks
{
  private:
    inline static constinit DeferredModuleScan _deferredScan;
    inline static constinit loader_boundary::KnownNoInitImages _noInit;
    inline static constinit std::atomic<HMODULE> _deferredImages[32] {};
    static void ScanLoadedModules();
    static void CheckModulesInMemory();
    static bool StartsWithInsensitive(std::wstring_view str, std::wstring_view prefix);

  public:
    // A d3d12.dll the game loaded where OptiScaler could not set it up (a thread without a TLS array, its own load of
    // the file failed, or the binding was busy). Kept (one reference) until CatchUpDeferredLoads sets it up.
    static void RecordDeferredLoad(HMODULE module);
    // Sets up the recorded d3d12.dll. Cheap when nothing is recorded; called after attach (GPU info worker) and at
    // every DXGI factory or swap chain creation, so a busy or late load is retried.
    static void CatchUpDeferredLoads();

    static HMODULE LoadLibraryCheckA(std::string libName, LPCSTR lpLibFullPath);
    // ownLoadFailed: set when a branch matched but OptiScaler's own load of the file failed. The caller then runs the
    // original load and passes its module to ObserveOrdinaryModule.
    static HMODULE LoadLibraryCheckW(std::wstring libName, LPCWSTR lpLibFullPath, bool* ownLoadFailed = nullptr);

    enum class RecoveryResult
    {
        Ready,
        Retry,
        Unsupported,
        NotApplicable
    };
    // Sets up d3d12.dll from a module the game loaded itself. Other modules: Unsupported / NotApplicable.
    static RecoveryResult ObserveOrdinaryModule(HMODULE module);
    // DONT_RESOLVE_DLL_REFERENCES images (loader_boundary::KnownNoInitImages)
    static void RecordNonExecutable(HMODULE module, HMODULE previous);
    static void ForgetNonExecutable(HMODULE module);
    static bool KnownNonExecutable(HMODULE module);
    static std::optional<NTSTATUS> FreeLibrary(PVOID library);
    // True for the modules FreeLibrary keeps loaded. Reads plain globals only, so it is safe on threads without a
    // static-TLS array (misc/ThreadTls.h), where FreeLibrary itself can't run.
    static bool IsKeptLoaded(PVOID library);

    static HMODULE LoadNvApi(LPCWSTR requestedName = L"nvapi64.dll");
    // static HMODULE LoadFfxapiVk(std::wstring originalPath);
    // static HMODULE LoadFfxapiDx12(std::wstring originalPath);
    // static HMODULE LoadLibxessDx11(std::wstring originalPath);
    // static HMODULE LoadLibxess(std::wstring originalPath);
    static HMODULE LoadNvngxDlss(std::wstring originalPath);

    static bool IsApiSetName(const std::wstring_view& n);
    static bool EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix);
    static bool EndsWithInsensitive(const UNICODE_STRING& text, std::wstring_view suffix);
};

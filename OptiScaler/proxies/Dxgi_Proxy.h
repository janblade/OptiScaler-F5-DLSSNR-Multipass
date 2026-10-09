#pragma once

#include "SysUtils.h"

#include <State.h>

#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>

#include <dxgi1_6.h>

class DxgiProxy
{
  public:
    typedef HRESULT (*PFN_CreateDxgiFactory)(REFIID riid, IDXGIFactory** ppFactory);
    typedef HRESULT (*PFN_CreateDxgiFactory1)(REFIID riid, IDXGIFactory1** ppFactory);
    typedef HRESULT (*PFN_CreateDxgiFactory2)(UINT Flags, REFIID riid, IDXGIFactory2** ppFactory);
    typedef HRESULT (*PFN_DeclareAdepterRemovalSupport)();
    typedef HRESULT (*PFN_GetDebugInterface)(UINT Flags, REFIID riid, void** ppDebug);

    static void Init(HMODULE module = nullptr)
    {
        if (_dll != nullptr)
            return;

        if (module == nullptr)
        {
            _dll = GetModuleHandle(L"dxgi.dll");

            if (_dll == dllModule)
                _dll = originalModule;

            if (_dll == nullptr)
            {
                _dll = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        else
        {
            _dll = module;
        }

        if (_dll == nullptr)
            return;

        LOG_DEBUG("dxgi.dll loaded at {:X}", (uintptr_t) _dll);

        _CreateDxgiFactory = (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory");
        _CreateDxgiFactory1 = (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory1");
        _CreateDxgiFactory2 = (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory2");
        _DeclareAdepterRemovalSupport = (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            _dll, "DXGIDeclareAdapterRemovalSupport");
        _GetDebugInterface = (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(_dll, "DXGIGetDebugInterface1");
    }

    static HMODULE Module() { return _dll; }

    // Marks this thread as inside a DXGI factory creation OptiScaler made or hooked. A DXGI that is a layer on Vulkan
    // (dxvk) creates its Vulkan instance inside CreateDXGIFactory, holding a lock it cannot take twice, and the Vulkan
    // loader can load an overlay's Vulkan layer right there. That layer's DLL loads reach LibraryLoadHooks' overlay
    // branch, which must then not call CreateDXGIFactory again on this thread: Batman: Arkham Knight on dxvk with the
    // Steam overlay hung at startup on exactly that (IdentifyGpu -> dxvk -> vkCreateInstance -> Steam overlay layer ->
    // overlay DLL load -> CreateDXGIFactory -> the same dxvk lock).
    struct ScopedFactoryCreation
    {
        ScopedFactoryCreation() { ++_factoryCreationDepth; }
        ~ScopedFactoryCreation() { --_factoryCreationDepth; }
        ScopedFactoryCreation(const ScopedFactoryCreation&) = delete;
        ScopedFactoryCreation& operator=(const ScopedFactoryCreation&) = delete;
    };

    static bool InsideFactoryCreation() { return _factoryCreationDepth > 0; }

    // Windows' own DXGI factory, for a swapchain on a real D3D12 queue when the loaded dxgi.dll is a layer that cannot
    // make one: dxvk's returns DXGI_ERROR_UNSUPPORTED, so frame generation's D3D12 swapchain for a dxvk game
    // (native/VkPresentBridge.h, NativeDxvkVulkan) comes from here. Loaded from System32 the way OptiScaler loads it for
    // itself as dxgi.dll. Not hooked: its methods are called directly. The caller owns the reference; null when the
    // system DLL cannot be told apart from the loaded one or the factory fails.
    static IDXGIFactory2* CreateSystemFactory2()
    {
        static const PFN_CreateDxgiFactory2 create = []() -> PFN_CreateDxgiFactory2
        {
            auto module = NtdllProxy::LoadLibraryExW_Ldr(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

            if (module == nullptr || module == _dll)
                return nullptr;

            return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(module, "CreateDXGIFactory2");
        }();

        if (create == nullptr)
            return nullptr;

        IDXGIFactory2* factory = nullptr;
        ScopedFactoryCreation factoryCreation {};

        if (FAILED(create(0, __uuidof(IDXGIFactory2), &factory)))
            return nullptr;

        return factory;
    }

    static PFN_CreateDxgiFactory CreateDxgiFactory_() { return _CreateDxgiFactory; }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_() { return _CreateDxgiFactory1; }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_() { return _CreateDxgiFactory2; }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_() { return _DeclareAdepterRemovalSupport; }
    static PFN_GetDebugInterface GetDebugInterface_() { return _GetDebugInterface; }

    static PFN_CreateDxgiFactory CreateDxgiFactory_Hooked()
    {
        return (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory");
    }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_Hooked()
    {
        return (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory1");
    }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_Hooked()
    {
        return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(_dll, "CreateDXGIFactory2");
    }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_Hooked()
    {
        return (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            _dll, "DXGIDeclareAdepterRemovalSupport");
    }
    static PFN_GetDebugInterface GetDebugInterface_Hooked()
    {
        return (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(_dll, "DXGIGetDebugInterface1");
    }

    static PFN_CreateDxgiFactory Hook_CreateDxgiFactory(PVOID method)
    {
        auto addr = CreateDxgiFactory_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_CreateDxgiFactory = addr;
        return addr;
    }

    static PFN_CreateDxgiFactory1 Hook_CreateDxgiFactory1(PVOID method)
    {
        auto addr = CreateDxgiFactory1_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_CreateDxgiFactory1 = addr;
        return addr;
    }

    static PFN_CreateDxgiFactory2 Hook_CreateDxgiFactory2(PVOID method)
    {
        auto addr = CreateDxgiFactory2_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_CreateDxgiFactory2 = addr;
        return addr;
    }

    static PFN_DeclareAdepterRemovalSupport Hook_DeclareAdepterRemovalSupport(PVOID method)
    {
        auto addr = DeclareAdepterRemovalSupport_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_DeclareAdepterRemovalSupport = addr;
        return addr;
    }

    static PFN_GetDebugInterface Hook_GetDebugInterface(PVOID method)
    {
        auto addr = GetDebugInterface_ForHook();

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook: {:X}", detourResult);
            return nullptr;
        }

        //_GetDebugInterface = addr;
        return addr;
    }

    static void Unhook(PVOID addr, PVOID method)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&) addr, method);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to unhook: {:X}", detourResult);
            return;
        }
    }

  private:
    inline static HMODULE _dll = nullptr;
    inline static thread_local int _factoryCreationDepth = 0;

    inline static PFN_CreateDxgiFactory _CreateDxgiFactory = nullptr;
    inline static PFN_CreateDxgiFactory1 _CreateDxgiFactory1 = nullptr;
    inline static PFN_CreateDxgiFactory2 _CreateDxgiFactory2 = nullptr;
    inline static PFN_DeclareAdepterRemovalSupport _DeclareAdepterRemovalSupport = nullptr;
    inline static PFN_GetDebugInterface _GetDebugInterface = nullptr;

    inline static HMODULE HookModule()
    {
        if (State::Instance().workingMode == WorkingMode::Dxgi)
            return dllModule;

        return _dll;
    }

    static PFN_CreateDxgiFactory CreateDxgiFactory_ForHook()
    {
        return (PFN_CreateDxgiFactory) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory");
    }
    static PFN_CreateDxgiFactory1 CreateDxgiFactory1_ForHook()
    {
        return (PFN_CreateDxgiFactory1) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory1");
    }
    static PFN_CreateDxgiFactory2 CreateDxgiFactory2_ForHook()
    {
        return (PFN_CreateDxgiFactory2) KernelBaseProxy::GetProcAddress_()(HookModule(), "CreateDXGIFactory2");
    }
    static PFN_DeclareAdepterRemovalSupport DeclareAdepterRemovalSupport_ForHook()
    {
        return (PFN_DeclareAdepterRemovalSupport) KernelBaseProxy::GetProcAddress_()(
            HookModule(), "DXGIDeclareAdepterRemovalSupport");
    }
    static PFN_GetDebugInterface GetDebugInterface_ForHook()
    {
        return (PFN_GetDebugInterface) KernelBaseProxy::GetProcAddress_()(HookModule(), "DXGIGetDebugInterface1");
    }
};

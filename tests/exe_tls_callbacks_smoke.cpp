// Host test for misc/ExeTlsCallbacks.h: the exe entry is not hooked when the exe has TLS callbacks.
#include "../OptiScaler/misc/ExeTlsCallbacks.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static void NoCallback(PVOID, DWORD, PVOID) {}

// A minimal mapped image: DOS header, NT headers, a TLS directory and a callback array
struct FakeImage
{
    std::vector<BYTE> bytes = std::vector<BYTE>(4096, 0);
    PIMAGE_TLS_CALLBACK callbacks[2] {};

    IMAGE_NT_HEADERS* Nt() { return reinterpret_cast<IMAGE_NT_HEADERS*>(bytes.data() + 0x80); }
    IMAGE_TLS_DIRECTORY* Tls() { return reinterpret_cast<IMAGE_TLS_DIRECTORY*>(bytes.data() + 0x400); }

    FakeImage(bool withTls, PIMAGE_TLS_CALLBACK first)
    {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;

        Nt()->Signature = IMAGE_NT_SIGNATURE;
        Nt()->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

        if (withTls)
        {
            auto& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            dir.VirtualAddress = 0x400;
            dir.Size = sizeof(IMAGE_TLS_DIRECTORY);
            callbacks[0] = first;
            Tls()->AddressOfCallBacks = reinterpret_cast<ULONG_PTR>(callbacks);
        }
    }
};

int main()
{
    {
        FakeImage image(false, nullptr);
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "no TLS directory: no callbacks");
    }
    {
        FakeImage image(true, nullptr);
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "TLS directory with an empty callback list: no callbacks");
    }
    {
        FakeImage image(true, NoCallback);
        Check(ExeTlsCallbacks::Has(image.bytes.data()), "TLS directory with a callback: has callbacks");
    }
    {
        FakeImage image(true, NoCallback);
        image.Tls()->AddressOfCallBacks = 0;
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "TLS directory without a callback array: no callbacks");
    }
    {
        FakeImage image(true, NoCallback);
        reinterpret_cast<IMAGE_DOS_HEADER*>(image.bytes.data())->e_magic = 0;
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "bad DOS signature: treated as no callbacks");
    }
    {
        FakeImage image(true, NoCallback);
        image.Nt()->Signature = 0;
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "bad NT signature: treated as no callbacks");
    }
    {
        FakeImage image(true, NoCallback);
        image.Nt()->OptionalHeader.NumberOfRvaAndSizes = IMAGE_DIRECTORY_ENTRY_TLS;
        Check(!ExeTlsCallbacks::Has(image.bytes.data()), "directory table too short for TLS: no callbacks");
    }
    Check(!ExeTlsCallbacks::Has(nullptr), "null image: no callbacks");

    // The real test process: whatever it has, the call must not crash
    (void) ExeTlsCallbacks::Has(reinterpret_cast<const BYTE*>(GetModuleHandle(nullptr)));

    if (failures == 0)
        std::printf("exe_tls_callbacks_smoke: PASS\n");

    return failures == 0 ? 0 : 1;
}

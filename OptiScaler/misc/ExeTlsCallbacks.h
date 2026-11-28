#pragma once

#include <windows.h>

// Whether a loaded exe image has TLS callbacks. They run before the exe entry, and protectors (Denuvo, VMProtect) use
// them to check the exe's code, so the exe entry must not be hooked then (it would patch the code they check).
// Header-only on the mapped image, so it is unit tested on the host with a fake image.
namespace ExeTlsCallbacks
{
inline bool Has(const BYTE* base)
{
    if (base == nullptr)
        return false;

    auto dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew <= 0)
        return false;

    auto ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);

    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS)
        return false;

    const auto& tlsData = ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];

    if (tlsData.VirtualAddress == 0 || tlsData.Size < sizeof(IMAGE_TLS_DIRECTORY))
        return false;

    auto tlsDir = reinterpret_cast<const IMAGE_TLS_DIRECTORY*>(base + tlsData.VirtualAddress);

    // A virtual address, already relocated by the loader in a mapped image
    auto callbacks = reinterpret_cast<const PIMAGE_TLS_CALLBACK*>(tlsDir->AddressOfCallBacks);

    return callbacks != nullptr && *callbacks != nullptr;
}
} // namespace ExeTlsCallbacks

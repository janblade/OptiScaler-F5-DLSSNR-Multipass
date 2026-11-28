#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

// File name of the module that holds `address` (pass _ReturnAddress() from a hook to get its caller); empty when
// there is none. Long install paths are read whole: a path cut at MAX_PATH would give a wrong file name.
inline std::wstring CallerModuleFileName(const void* address)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR) address, &module))
        return {};

    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), (DWORD) path.size());
    if (length == 0 || length >= path.size())
        return {};

    path.resize(length);
    return std::filesystem::path(path).filename().wstring();
}

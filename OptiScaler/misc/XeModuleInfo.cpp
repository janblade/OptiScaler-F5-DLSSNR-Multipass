#include "pch.h"
#include "XeModuleInfo.h"

#include <Config.h>
#include <Util.h>

#include <proxies/Ntdll_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>

#include <fstream>

#include <bcrypt.h>
#include <SoftPub.h>
#include <WinTrust.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
std::mutex lineMutex;
std::string xefgLine;
std::string xellLine;

std::wstring PathOf(HMODULE module)
{
    if (module == nullptr)
        return {};

    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::wstring() : std::wstring(path, length);
}

bool SameFolder(const std::filesystem::path& file, const std::wstring& folder)
{
    std::error_code error;
    return !folder.empty() && std::filesystem::equivalent(file.parent_path(), folder, error);
}

// Where a module's file comes from, in the words the menu uses
std::string SourceOf(const std::filesystem::path& file, const std::optional<std::wstring>& overridePath)
{
    std::error_code error;

    if (overridePath.has_value() && !overridePath->empty() &&
        (std::filesystem::equivalent(file, *overridePath, error) || SameFolder(file, *overridePath)))
        return "the ini's path";

    if (SameFolder(file, Config::Instance()->MainDllPath.value_or(L"")))
        return "OptiScaler's folder";

    if (SameFolder(file, Util::ExePath().parent_path().wstring()))
        return "the game's folder";

    return "elsewhere";
}

std::string VersionOf(const std::wstring& path)
{
    version_t version {};

    if (!Util::GetFileVersion(path, &version))
        return "no version";

    return std::format("v{}.{}.{}.{}", version.major, version.minor, version.patch, version.reserved);
}

// The file's Authenticode signature, checked offline (no revocation lookup). A file someone changed after signing
// fails it, an unsigned one has none.
std::string SignatureOf(const std::wstring& path)
{
    using PFN_WinVerifyTrust = LONG(WINAPI*)(HWND, GUID*, LPVOID);

    static const auto verify = []
    {
        const auto wintrust = NtdllProxy::LoadLibraryExW_Ldr(L"wintrust.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return wintrust != nullptr ? (PFN_WinVerifyTrust) GetProcAddress(wintrust, "WinVerifyTrust") : nullptr;
    }();

    if (verify == nullptr)
        return "signature not checked";

    WINTRUST_FILE_INFO file {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    WINTRUST_DATA data {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = verify(nullptr, &action, &data);

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &action, &data);

    if (result == ERROR_SUCCESS)
        return "signed";

    if (result == TRUST_E_NOSIGNATURE)
        return "not signed";

    return std::format("signature does not check out ({:#x}, modified?)", (uint32_t) result);
}

std::string Sha256Of(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (!file || BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) != 0)
        return {};

    std::vector<char> chunk(1 << 20);
    bool ok = true;

    while (ok && file)
    {
        file.read(chunk.data(), (std::streamsize) chunk.size());

        if (file.gcount() > 0)
            ok = BCryptHashData(hash, (PUCHAR) chunk.data(), (ULONG) file.gcount(), 0) == 0;
    }

    unsigned char digest[32] {};
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    BCryptDestroyHash(hash);

    if (!ok)
        return {};

    std::string hex;
    for (const auto byte : digest)
        hex += std::format("{:02x}", byte);

    return hex;
}

// The log line and the menu line of one module (libxess_fg.dll or libxell.dll)
std::string DescribeOne(const char* what, HMODULE module, HMODULE gameCopy, const std::wstring& path,
                        const std::optional<std::wstring>& overridePath)
{
    if (module == nullptr || path.empty())
    {
        LOG_INFO("{}: not loaded", what);
        return {};
    }

    const std::filesystem::path file(path);
    const auto source = SourceOf(file, overridePath);
    const auto version = VersionOf(path);
    const auto signature = SignatureOf(path);

    LOG_INFO("{} in use: {} (from {}), file {}, {}, SHA-256 {}", what, wstring_to_string(path), source, version,
             signature, Sha256Of(path));

    std::string line = std::format("{}: {} {} from {}, {}", what, wstring_to_string(file.filename().wstring()),
                                   version, source, signature);

    if (gameCopy != nullptr && gameCopy != module)
    {
        const auto gamePath = PathOf(gameCopy);
        const auto gameVersion = VersionOf(gamePath);

        LOG_INFO("{}: the game's own copy is loaded too: {}, file {}", what, wstring_to_string(gamePath), gameVersion);
        line += std::format("; the game's own {} is loaded too", gameVersion);
    }

    return line;
}
} // namespace

void XeModuleInfo::Describe()
{
    static std::once_flag once;

    std::call_once(once,
                   []
                   {
                       // Read on this thread: the paths and handles do not change once the proxies have loaded
                       const HMODULE xefg = XeFGProxy::Module();
                       const HMODULE xefgGame = XeFGProxy::MemModule();
                       const std::wstring xefgPath = XeFGProxy::Module_Path();
                       const auto xefgOverride = Config::Instance()->XeFGLibrary.value_or(L"");

                       const HMODULE xell = XeLLProxy::Module();
                       const HMODULE xellGame = XeLLProxy::MemModule();
                       const std::wstring xellPath = XeLLProxy::Module_Path();
                       const auto xellOverride = Config::Instance()->XeLLLibrary.value_or(L"");

                       std::thread(
                           [=]
                           {
                               auto fg = DescribeOne("XeFG", xefg, xefgGame, xefgPath, xefgOverride);
                               auto ll = DescribeOne("XeLL", xell, xellGame, xellPath, xellOverride);

                               std::scoped_lock lock(lineMutex);
                               xefgLine = std::move(fg);
                               xellLine = std::move(ll);
                           })
                           .detach();
                   });
}

std::string XeModuleInfo::XeFGLine()
{
    std::scoped_lock lock(lineMutex);
    return xefgLine;
}

std::string XeModuleInfo::XeLLLine()
{
    std::scoped_lock lock(lineMutex);
    return xellLine;
}

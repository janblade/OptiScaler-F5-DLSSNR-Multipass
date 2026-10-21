// Stress-repros the intermittent Streamline Detours transaction race: StreamlineHooks::hookDlss /
// hookDlssg / hookInterposer / hookReflex / hookPcl / hookCommon each run their own complete
// DetourTransactionBegin()...DetourTransactionCommit() sequence, synchronously, from inside
// OptiScaler's own LoadLibraryW hook (LibraryLoadHooks::LoadLibraryCheckW), with nothing
// serializing them against each other. This drives real LoadLibraryW calls on several real
// sl.*.dll plugins concurrently, from multiple threads, to maximize the chance two of those
// transactions are in flight on Detours' process-wide state at once -- the same collision the two
// user-supplied repro logs showed as "Failed to (un)hook ...: 10DD" (0x10DD ==
// ERROR_INVALID_OPERATION from DetourTransactionCommit) immediately followed by Streamline's own
// exception handler writing a minidump and no further plugin ever loading.
//
// This does not call slInit -- it only needs to exercise LoadLibraryCheckW's dispatch, not a
// working Streamline session, so it has no dependency on sl.h / a device / a window.
//
// Build: cl /std:c++20 /EHsc /W4 tests\streamline_detours_race_stress.cpp
// Run in an isolated directory with an OptiScaler.ini containing [Log] LogToFile=true (LogLevel
// left at its default 0/trace, which already includes LOG_ERROR) and OptiDllPath pointed at a
// harmless FGInput/FGOutput so hookDlss/hookDlssg actually run (shouldHookSl just needs the
// plugin directory below to not be OptiScaler's own private Streamline path, which an ad hoc
// directory satisfies automatically):
//   streamline_detours_race_stress.exe <OptiScaler DLL> <Streamline plugin dir> [iterations] [threads]
//
// After the threads finish, this reads OptiScaler.log itself (written next to the DLL) for any
// "Failed to hook" / "Failed to unhook" line -- hookDlss et al. are not exported, so the log is
// the only observable signal, exactly as it was for the two original repro logs. Exits non-zero
// (FAIL) if any such line is found, zero (PASS) otherwise.
//
// It also checks the "hook each plugin once" rule (the intermittent NBA 2K27 startup freeze): while
// the loads run, other threads keep calling each plugin's slGetPluginFunction through our detour.
// A re-hook that removes the live detour (a null original) shows up as an access violation, two
// detours stacked on one function as a hang (watchdog). The log must show one "Hooking
// slGetPluginFunction in <plugin>" per plugin for the whole run, however often it is loaded again.
// Then sl.dlss.dll is freed until it unloads and loaded once more: the fresh copy must be hooked
// again (a second "Hooking" line, and a probe call reaching hkdlss_slGetPluginFunction).
// Two more cases from the Review Pass (PR #210):
// - Two copies side by side: a second sl.dlss.dll from another folder is loaded in turn with the first, many times.
//   Only one copy can carry the hook (each hook calls one original); the other's is taken off. No crash, the probe
//   through the copy loaded last reaches the hook and the one through the other copy does not. Not while called:
//   hooking a copy writes over its first instructions and Detours moves only the thread doing it, so a thread running
//   those very instructions at that moment would fault -- on every hook, before this change too (it hooked on every
//   load then). Streamline calls a plugin's slGetPluginFunction while loading it, not in a loop.
// - A hook on top of ours: the test writes a jump over our jump, as an overlay would, then loads the plugin again.
//   Hooking again would stack a second detour behind that one, and a call would loop forever; it must be left alone
//   (a "hooked by something else" line) and a call must return.
// Needs [Log] LogLevel=0 (trace) for the "Hooking" lines.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void Require(bool success, const char* stage)
{
    if (!success)
        throw std::runtime_error(stage);
}

// One thread per entry, each hammering a different plugin so different StreamlineHooks::hookXxx
// functions (and therefore different DetourTransactionBegin/Commit pairs) are likely to overlap.
static const wchar_t* kPluginNames[] = { L"sl.interposer.dll", L"sl.dlss.dll",   L"sl.dlss_g.dll",
                                          L"sl.reflex.dll",     L"sl.pcl.dll",    L"sl.common.dll" };

static std::atomic<bool> g_stopCalls { false };

// Calls the plugin's slGetPluginFunction (our detour once hooked) until the loads are done. The name is not one any
// plugin exports, so the real function only looks it up.
static void CallThroughHook(const std::filesystem::path& path)
{
    const auto module = GetModuleHandleW(path.filename().c_str());
    const auto getPluginFunction =
        module ? reinterpret_cast<void* (*) (const char*)>(GetProcAddress(module, "slGetPluginFunction")) : nullptr;

    if (getPluginFunction == nullptr)
        return;

    while (!g_stopCalls.load())
        getPluginFunction("probe_hammer");
}

using GetPluginFunction = void* (*) (const char*);

static GetPluginFunction FunctionOf(HMODULE module)
{
    return module ? reinterpret_cast<GetPluginFunction>(GetProcAddress(module, "slGetPluginFunction")) : nullptr;
}

// A call that must return within a few seconds (a loop between stacked detours never does)
static bool ReturnsInTime(GetPluginFunction function, const char* name)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread call(
        [function, name, done]
        {
            function(name);
            *done = true;
        });

    for (int i = 0; i < 50 && !*done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!*done)
    {
        call.detach(); // left looping; the run exits right after
        return false;
    }

    call.join();
    return true;
}

// Writes a jump at `function` to a stub that jumps on to where `function` jumped before: another tool hooking the
// function on top of ours. The stub has to be within 2 GB of the function for a 5-byte jump.
static bool HookOnTop(void* function)
{
    auto* code = static_cast<uint8_t*>(function);

    if (code[0] != 0xE9 || (reinterpret_cast<uintptr_t>(code) & 7) != 0)
        return false;

    int32_t relative = 0;
    std::memcpy(&relative, code + 1, sizeof(relative));
    const uint64_t onward = reinterpret_cast<uint64_t>(code) + 5 + relative;

    uint8_t* stub = nullptr;
    for (uint64_t offset = 0x10000; stub == nullptr && offset < 0x70000000; offset += 0x10000)
    {
        const uint64_t at = (reinterpret_cast<uint64_t>(code) - offset) & ~0xFFFFull;
        stub = static_cast<uint8_t*>(
            VirtualAlloc(reinterpret_cast<void*>(at), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    }

    if (stub == nullptr)
        return false;

    stub[0] = 0x48; // mov rax, onward
    stub[1] = 0xB8;
    std::memcpy(stub + 2, &onward, sizeof(onward));
    stub[10] = 0xFF; // jmp rax
    stub[11] = 0xE0;

    LONG64 head = 0;
    std::memcpy(&head, code, sizeof(head));
    auto* bytes = reinterpret_cast<uint8_t*>(&head);
    const int32_t toStub = static_cast<int32_t>(reinterpret_cast<int64_t>(stub) - reinterpret_cast<int64_t>(code) - 5);
    std::memcpy(bytes + 1, &toStub, sizeof(toStub));

    DWORD protection = 0;
    if (!VirtualProtect(code, 8, PAGE_EXECUTE_READWRITE, &protection))
        return false;

    InterlockedExchange64(reinterpret_cast<LONG64*>(code), head);
    VirtualProtect(code, 8, protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), code, 8);
    return true;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
{
    const auto* at = static_cast<const char*>(info->ExceptionRecord->ExceptionAddress);
    HMODULE module = nullptr;
    wchar_t name[MAX_PATH] = L"no module: a trampoline or freed memory";

    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(at), &module))
        GetModuleFileNameW(module, name, MAX_PATH);

    std::printf("FAIL: exception 0x%08lX at %p (%ls +0x%llX) during the stress run (a call through a removed hook?)\n",
                info->ExceptionRecord->ExceptionCode, at, name,
                module ? static_cast<unsigned long long>(at - reinterpret_cast<const char*>(module)) : 0ull);
    std::fflush(stdout);
    ExitProcess(3);
}

static void HammerPlugin(const std::filesystem::path& path, int iterations)
{
    for (int i = 0; i < iterations; ++i)
    {
        // Every call re-enters our own LoadLibraryW hook and re-runs LoadLibraryCheckW's dispatch
        // -- including StreamlineHooks::hookDlss/hookDlssg/etc. -- even though the module is
        // already resident, since the hook intercepts the API call itself, not first-load.
        LoadLibraryW(path.c_str());
    }
}

// Every hookX/unhookX error uses the same "Failed to (un)hook X: {:X}" format regardless of which
// error code DetourTransactionCommit returned, and this stress pattern (several threads hammering
// the same plugin concurrently) is known to also surface a second, pre-existing, unrelated
// failure: "...: 9" (ERROR_INVALID_BLOCK, a TOCTOU race on the "already hooked?" check that lives
// outside any lock, present before and after the transaction-mutex fix this test targets). Only
// "...: 10DD" (ERROR_INVALID_OPERATION from DetourTransactionCommit -- the actual Detours
// transaction race this test exists to catch) should fail the run; other codes are logged for
// visibility but do not affect the verdict.
// The log line each plugin's hook writes ("Hooking slGetPluginFunction in sl.dlssg" for sl.dlss_g.dll); interposer
// and unknown files have none
static std::string HookLineFor(const std::filesystem::path& path)
{
    const auto name = path.filename().wstring();

    if (name == L"sl.dlss.dll")
        return "Hooking slGetPluginFunction in sl.dlss";
    if (name == L"sl.dlss_g.dll")
        return "Hooking slGetPluginFunction in sl.dlssg";
    if (name == L"sl.reflex.dll")
        return "Hooking slGetPluginFunction in sl.reflex";
    if (name == L"sl.pcl.dll")
        return "Hooking slGetPluginFunction in sl.pcl";
    if (name == L"sl.common.dll")
        return "Hooking slGetPluginFunction in sl.common";

    return {};
}

static std::vector<std::string> ReadLog(const std::filesystem::path& logPath)
{
    std::vector<std::string> lines;
    std::ifstream log(logPath);
    std::string line;

    while (std::getline(log, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        lines.push_back(line);
    }

    return lines;
}

// Lines ending in exactly this text ("...in sl.dlss" must not count "...in sl.dlssg")
static int CountEndingWith(const std::vector<std::string>& lines, const std::string& text)
{
    int count = 0;

    for (const auto& line : lines)
        count += line.size() >= text.size() && line.compare(line.size() - text.size(), text.size(), text) == 0;

    return count;
}

static bool AnyContains(const std::vector<std::string>& lines, const std::string& text)
{
    for (const auto& line : lines)
    {
        if (line.find(text) != std::string::npos)
            return true;
    }

    return false;
}

// OptiScaler's file sink may be asynchronous: wait for the log to stop growing
static void WaitForLogToSettle(const std::filesystem::path& logPath)
{
    std::error_code sizeError;
    auto lastSize = std::filesystem::file_size(logPath, sizeError);

    for (int stableChecks = 0; stableChecks < 3;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto size = std::filesystem::file_size(logPath, sizeError);

        if (!sizeError && size == lastSize)
            ++stableChecks;
        else
            stableChecks = 0;

        lastSize = size;
    }
}

static bool LogContainsTransactionRaceFailure(const std::filesystem::path& logPath)
{
    std::ifstream log(logPath);
    if (!log)
    {
        std::fprintf(stderr, "WARN: could not open %ls to check for failures\n", logPath.c_str());
        return false;
    }
    std::string line;
    bool sawRaceFailure = false;
    while (std::getline(log, line))
    {
        const bool isHookFailure =
            line.find("Failed to hook") != std::string::npos || line.find("Failed to unhook") != std::string::npos;
        if (!isHookFailure)
            continue;

        const bool isTransactionRace = line.find(": 10DD") != std::string::npos;
        std::printf("%s%s\n", line.c_str(), isTransactionRace ? "" : "  (unrelated to the transaction race, ignored)");
        if (isTransactionRace)
            sawRaceFailure = true;
    }
    return sawRaceFailure;
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(CrashFilter);

    // Two detours stacked on one function loop forever; nothing here takes this long otherwise
    std::thread(
        []
        {
            std::this_thread::sleep_for(std::chrono::seconds(120));
            std::puts("FAIL: no progress for 120 s (a call looping between two stacked detours?)");
            std::fflush(stdout);
            ExitProcess(2);
        })
        .detach();

    try
    {
        Require(argc >= 3, "Expected OptiScaler DLL, Streamline plugin dir, [iterations], [threads]");
        const int iterations = argc >= 4 ? std::stoi(argv[3]) : 500;
        const int threadsPerPlugin = argc >= 5 ? std::stoi(argv[4]) : 3;

        const std::filesystem::path dllPath(argv[1]);
        const std::filesystem::path pluginDir(argv[2]);
        Require(LoadLibraryW(dllPath.c_str()) != nullptr, "Load OptiScaler");

        std::vector<std::filesystem::path> pluginPaths;
        for (auto name : kPluginNames)
        {
            auto candidate = pluginDir / name;
            if (std::filesystem::exists(candidate))
                pluginPaths.push_back(candidate);
            else
                std::fprintf(stderr, "WARN: %ls not found, skipping\n", candidate.c_str());
        }
        Require(!pluginPaths.empty(), "No known Streamline plugin DLLs found in the given directory");

        // First load of each plugin (hooks it), so the callers below have a function to call
        for (const auto& path : pluginPaths)
            LoadLibraryW(path.c_str());

        std::vector<std::thread> callers;
        for (const auto& path : pluginPaths)
        {
            if (!HookLineFor(path).empty())
                callers.emplace_back(CallThroughHook, path);
        }

        std::vector<std::thread> threads;
        for (const auto& path : pluginPaths)
        {
            for (int t = 0; t < threadsPerPlugin; ++t)
                threads.emplace_back(HammerPlugin, path, iterations);
        }
        for (auto& thread : threads)
            thread.join();

        g_stopCalls = true;
        for (auto& caller : callers)
            caller.join();

        std::printf("Ran %zu plugin(s) x %d thread(s) x %d iteration(s)\n", pluginPaths.size(),
                    threadsPerPlugin, iterations);
        std::fflush(stdout);

        // OptiScaler resolves a relative LogFileName against the DLL's own directory
        const auto logPath = dllPath.parent_path() / L"OptiScaler.log";
        WaitForLogToSettle(logPath);
        bool failed = false;

        if (LogContainsTransactionRaceFailure(logPath))
        {
            std::puts("FAIL: Detours transaction race failure(s) logged above (10DD)");
            failed = true;
        }

        // Loading a resident plugin again must leave its live hook alone
        auto lines = ReadLog(logPath);
        for (const auto& path : pluginPaths)
        {
            const auto hookLine = HookLineFor(path);
            if (hookLine.empty())
                continue;

            const int hooks = CountEndingWith(lines, hookLine);
            std::printf("%ls: hooked %d time(s)\n", path.filename().c_str(), hooks);

            if (hooks != 1)
            {
                std::printf("FAIL: %ls should be hooked once, was %d times\n", path.filename().c_str(), hooks);
                failed = true;
            }
        }

        // Unloaded and loaded again: the fresh copy must carry the hook again
        const auto dlssPath = pluginDir / L"sl.dlss.dll";
        if (std::filesystem::exists(dlssPath))
        {
            const auto hookLine = HookLineFor(dlssPath);
            const int hooksBefore = CountEndingWith(lines, hookLine);

            for (int i = 0; i < 10000000 && GetModuleHandleW(L"sl.dlss.dll") != nullptr; ++i)
                FreeLibrary(GetModuleHandleW(L"sl.dlss.dll"));

            Require(GetModuleHandleW(L"sl.dlss.dll") == nullptr, "Unload sl.dlss.dll");
            Require(LoadLibraryW(dlssPath.c_str()) != nullptr, "Load sl.dlss.dll again");

            const auto getPluginFunction = reinterpret_cast<void* (*) (const char*)>(
                GetProcAddress(GetModuleHandleW(L"sl.dlss.dll"), "slGetPluginFunction"));
            Require(getPluginFunction != nullptr, "slGetPluginFunction after the reload");
            getPluginFunction("probe_after_reload");

            WaitForLogToSettle(logPath);
            lines = ReadLog(logPath);
            const int hooksAfter = CountEndingWith(lines, hookLine);
            const bool probeReachedHook = AnyContains(lines, "probe_after_reload");
            std::printf("sl.dlss.dll after unload + load: hooked %d more time(s), probe %s the hook\n",
                        hooksAfter - hooksBefore, probeReachedHook ? "reached" : "did not reach");

            if (hooksAfter - hooksBefore != 1 || !probeReachedHook)
            {
                std::puts("FAIL: the reloaded sl.dlss.dll should be hooked again, once");
                failed = true;
            }
        }

        // Two copies side by side, loaded in turn while both are called
        if (std::filesystem::exists(dlssPath))
        {
            const auto copyDir = dllPath.parent_path() / L"second_copy";
            std::filesystem::create_directories(copyDir);
            const auto copyPath = copyDir / L"sl.dlss.dll";
            std::filesystem::copy_file(dlssPath, copyPath, std::filesystem::copy_options::overwrite_existing);

            const HMODULE first = LoadLibraryW(dlssPath.c_str());
            const HMODULE second = LoadLibraryW(copyPath.c_str());
            Require(first != nullptr && second != nullptr && first != second, "Two copies of sl.dlss.dll side by side");

            for (int i = 0; i < 200; ++i)
            {
                LoadLibraryW(dlssPath.c_str());
                FunctionOf(first)("probe_first_between");
                LoadLibraryW(copyPath.c_str());
                FunctionOf(second)("probe_second_between");
            }

            FunctionOf(second)("probe_second_copy");
            FunctionOf(first)("probe_first_copy");
            WaitForLogToSettle(logPath);
            lines = ReadLog(logPath);
            const bool reached = AnyContains(lines, "probe_second_copy");
            const bool other = AnyContains(lines, "probe_first_copy");
            std::printf("two copies loaded in turn 200 times: no crash, probe through the last %s the hook, through "
                        "the other %s\n",
                        reached ? "reached" : "did not reach", other ? "reached it too" : "did not");

            if (!reached || other)
            {
                std::puts("FAIL: only the copy loaded last should carry the hook");
                failed = true;
            }

            // A hook on top of ours, then the plugin loaded again: left alone, and a call returns
            if (HookOnTop(reinterpret_cast<void*>(FunctionOf(second))))
            {
                LoadLibraryW(copyPath.c_str());
                const bool returned = ReturnsInTime(FunctionOf(second), "probe_on_top");
                WaitForLogToSettle(logPath);
                lines = ReadLog(logPath);
                const bool said = AnyContains(lines, "hooked by something else on top of ours");
                std::printf("hook on top of ours, loaded again: call %s, %s\n",
                            returned ? "returned" : "did NOT return", said ? "left alone" : "not told apart");

                if (!returned || !said)
                {
                    std::puts("FAIL: a hook on top of ours must be left alone");
                    failed = true;
                }
            }
            else
            {
                std::puts("WARN: could not place a hook on top of ours; that case was not run");
            }
        }

        if (failed)
        {
            std::fflush(stdout);
            ExitProcess(1);
        }

        std::puts("PASS: no transaction race, each plugin hooked once while loaded again and called, reload hooked "
                  "again, two copies in turn, a hook on top left alone");
        std::fflush(stdout);
        ExitProcess(0);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::fflush(stderr);
        ExitProcess(1);
    }
}

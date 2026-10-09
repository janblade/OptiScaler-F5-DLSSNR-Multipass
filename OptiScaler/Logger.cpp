#include "pch.h"
#include "Logger.h"
#include "Config.h"
#include <iostream>
#include <filesystem>
#include <DbgHelp.h>

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include <include/spdlog_sink/debug_sink.h>

#include "Util.h"
#include "LogAsyncRule.h"

// The logger in use and what its crash-path flush needs (LogAsyncRule.h). Left allocated at exit on purpose: destroying
// a thread pool at process exit joins a worker that is already gone.
static logasync::Setup* g_logSetup = nullptr;

static bool InitializeConsole()
{
    // Allocate a console for this app
    if (!AllocConsole())
        return false;

    FILE* pFile;

    // Redirect STDIN if the console has an input handle
    if (GetStdHandle(STD_INPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONIN$", "r", stdin) != 0)
            return false;
    }

    // Redirect STDOUT if the console has an output handle
    if (GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stdout) != 0)
            return false;
    }

    // Redirect STDERR if the console has an error handle
    if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stderr) != 0)
            return false;
    }

    // Clear the error state for each of the C++ standard streams
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::wcout.clear();
    std::wcerr.clear();

    // Make C++ standard streams point to console as well.
    std::ios::sync_with_stdio();

    WaitForEnter();

    return true;
}

void WaitForEnter()
{
    if (Config::Instance()->DebugWait.value_or_default())
    {
        std::cout << "Press ENTER to continue..." << std::endl;
        std::cin.get();
    }
}

void PrepareLogger()
{
    try
    {
        if (spdlog::default_logger() != nullptr)
            spdlog::default_logger().reset();

        if (Config::Instance()->LogToConsole.value_or_default() || Config::Instance()->LogToFile.value_or_default() ||
            Config::Instance()->LogToNGX.value_or_default() || Config::Instance()->LogToDebug.value_or_default())
        {
            if (Config::Instance()->OpenConsole.value_or_default())
                InitializeConsole();

            std::shared_ptr<spdlog::logger> shared_logger = nullptr;

            // Auto: a worker writes the file, so no game thread waits for a disk (a USB drive stalled presents for
            // hundreds of ms). An explicit LogAsync=false writes every line on the calling thread.
            const auto logAsync = Config::Instance()->LogAsync.has_value()
                                      ? std::optional<bool>(Config::Instance()->LogAsync.value())
                                      : std::nullopt;
            const auto logMode = logasync::ChooseMode(logAsync, Config::Instance()->LogToFile.value_or_default());

            if (logMode == logasync::Mode::AsyncBlock)
            {
                // Set the queue size for asynchronous logging
                spdlog::init_thread_pool(logasync::kBlockQueueSize,
                                         Config::Instance()->LogAsyncThreads.value_or_default());
            }

            std::vector<spdlog::sink_ptr> sinks;

            if (Config::Instance()->LogToDebug.value_or_default())
            {
                auto debug_sink = std::make_shared<spdlog::sinks::debug_sink_mt>();
                debug_sink->set_level(spdlog::level::level_enum::trace);

#ifdef LOG_ASYNC
                debug_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                debug_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(debug_sink);
            }

            if (Config::Instance()->LogToConsole.value_or_default())
            {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_level(spdlog::level::level_enum::info);
                console_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

                sinks.push_back(console_sink);
            }

            if (Config::Instance()->LogToFile.value_or_default())
            {
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    Config::Instance()->LogFileName.value_or_default(), true);
                file_sink->set_level(spdlog::level::level_enum::trace);
#ifdef LOG_ASYNC
                file_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                file_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(file_sink);
            }

            auto callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
                [](const spdlog::details::log_msg& msg)
                {
                    if (Config::Instance()->LogToNGX.value_or_default() &&
                        State::Instance().NVNGX_Logger.LoggingCallback != nullptr &&
                        State::Instance().NVNGX_Logger.MinimumLoggingLevel != NVSDK_NGX_LOGGING_LEVEL_OFF &&
                        (State::Instance().NVNGX_Logger.MinimumLoggingLevel == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ||
                         msg.level >= spdlog::level::info))
                    {
                        auto message = (char*) msg.payload.data();
                        State::Instance().NVNGX_Logger.LoggingCallback(message, NVSDK_NGX_LOGGING_LEVEL_ON,
                                                                       NVSDK_NGX_Feature_SuperSampling);
                    }
                });

            callback_sink->set_level(spdlog::level::level_enum::trace);
            callback_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

            sinks.push_back(callback_sink);

            auto* setup = new logasync::Setup(
                logasync::MakeLogger(logMode == logasync::Mode::Sync ? "multi_sink" : "multi_sink_logger", sinks,
                                     logMode, logMode == logasync::Mode::AsyncBlock ? spdlog::thread_pool() : nullptr));
            shared_logger = setup->logger;
            std::swap(g_logSetup, setup);
            delete setup; // an earlier logger's setup, if PrepareLogger ran before

            shared_logger->set_level((spdlog::level::level_enum) Config::Instance()->LogLevel.value_or_default());
            shared_logger->flush_on(spdlog::level::trace);

            spdlog::set_default_logger(shared_logger);
        }
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cerr << ex.what() << std::endl;

        auto logger = spdlog::stdout_color_mt("xess");
        logger->set_pattern("[%H:%M:%S.%f] [%L] %v");
        logger->set_level((spdlog::level::level_enum) 2);
        spdlog::set_default_logger(logger);
    }
}

// A bounded moment for the worker to write what is queued; never forever, the process is going down
static constexpr auto kFlushWait = std::chrono::milliseconds(500);

void CloseLogger()
{
    bool drained = true;

    NoteDroppedLogLines(); // a game that never reaches a periodic report still hears of it once

    if (g_logSetup != nullptr)
        drained = logasync::FlushAndDrain(*g_logSetup, kFlushWait);
    else if (spdlog::default_logger() != nullptr)
        spdlog::default_logger()->flush();

    spdlog::shutdown();

    // Stopping the worker waits for it: only when it is idle
    if (drained && g_logSetup != nullptr)
    {
        delete g_logSetup;
        g_logSetup = nullptr;
    }
}

void NoteDroppedLogLines()
{
    static std::mutex reportMutex;
    static logasync::DropReporter reporter;

    auto* setup = g_logSetup;

    if (setup == nullptr || setup->mode != logasync::Mode::AsyncDropOldest || setup->pool == nullptr)
        return;

    std::unique_lock lock(reportMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    if (const auto dropped = reporter.Poll(setup->pool->overrun_counter(), Util::MillisecondsNow()); dropped > 0)
        LOG_WARN("{} log lines dropped (the log could not keep up)", dropped);
}

static LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* exceptionInfo)
{
    try
    {
        auto dumpPath = std::filesystem::path(Config::Instance()->LogFileName.value_or_default()).parent_path() /
                        L"OptiScaler_crash.dmp";

        spdlog::critical("Unhandled exception {:X} at {:X}, writing {}",
                         (unsigned long) exceptionInfo->ExceptionRecord->ExceptionCode,
                         (size_t) exceptionInfo->ExceptionRecord->ExceptionAddress, dumpPath.string());

        // The worker writes what is queued, then the dump: a bounded wait, not a flush that could wait for a stuck disk
        if (g_logSetup != nullptr)
            logasync::FlushAndDrain(*g_logSetup, kFlushWait);
        else
            spdlog::default_logger()->flush();

        HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mdei {};
            mdei.ThreadId = GetCurrentThreadId();
            mdei.ExceptionPointers = exceptionInfo;
            mdei.ClientPointers = FALSE;

            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              (MINIDUMP_TYPE) (MiniDumpWithDataSegs | MiniDumpWithUnloadedModules |
                                               MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                              &mdei, nullptr, nullptr);

            CloseHandle(file);
        }
    }
    catch (...)
    {
        // Diagnostic aid only: never let this handler itself change how the crash is reported.
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    SetUnhandledExceptionFilter(CrashDumpHandler);
}

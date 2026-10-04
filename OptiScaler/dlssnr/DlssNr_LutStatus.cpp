#include "pch.h"
#include "DlssNr_LutStatus.h"

#include <mutex>

namespace DlssNr
{
namespace
{
std::mutex g_mutex;
LutStatus g_status;
} // namespace

void ReportLutStatus(bool wanted, const std::string& loadedPath, int size, bool failed, const std::string& error,
                     const std::string& attemptedPath, float strength)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.seen = true;
    g_status.wanted = wanted;
    g_status.loaded = wanted && !loadedPath.empty();
    g_status.loadedPath = loadedPath;
    g_status.size = size;
    g_status.failed = failed;
    g_status.error = error;
    g_status.attemptedPath = attemptedPath;
    g_status.strength = strength;
}

LutStatus ReadLutStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
} // namespace DlssNr

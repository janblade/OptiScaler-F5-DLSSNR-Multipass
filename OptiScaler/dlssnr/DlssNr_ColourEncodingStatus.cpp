#include "pch.h"
#include "DlssNr_ColourEncodingStatus.h"

#include <cstring>
#include <mutex>

namespace DlssNr
{
namespace
{
std::mutex g_mutex;
DlssNrColourEncoding::Choice g_choice;
char g_format[48] = {};
bool g_seen = false;
} // namespace

void ReportColourEncoding(const DlssNrColourEncoding::Choice& choice, const char* formatName, const char* path)
{
    const char* format = formatName ? formatName : "";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const bool same = g_seen && g_choice.encoding == choice.encoding && g_choice.automatic == choice.automatic &&
                          g_choice.mismatch == choice.mismatch && g_choice.reason == choice.reason &&
                          std::strncmp(g_format, format, sizeof(g_format) - 1) == 0;
        if (same)
            return;
        g_choice = choice;
        std::strncpy(g_format, format, sizeof(g_format) - 1);
        g_format[sizeof(g_format) - 1] = '\0';
        g_seen = true;
    }

    if (choice.mismatch)
        LOG_WARN("DLSS-NR {}{}colour encoding: {} {}", path ? path : "", path && *path ? " " : "",
                 DlssNrColourEncoding::Name(choice.encoding), DlssNrColourEncoding::MismatchWarning(choice));
}

ColourEncodingStatus ReadColourEncodingStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ColourEncodingStatus status;
    status.seen = g_seen;
    if (g_seen)
    {
        status.line = DlssNrColourEncoding::Describe(g_choice, g_format);
        status.warning = DlssNrColourEncoding::MismatchWarning(g_choice);
    }
    else
        status.line = "Waiting for a frame.";
    return status;
}
} // namespace DlssNr

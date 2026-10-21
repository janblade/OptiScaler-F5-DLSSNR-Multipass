// Not built with the precompiled header: Win32 only, so tests/sdr_to_hdr_dx11_gpu.cpp compiles it alone.
#include "SdrToHdr_Fullscreen.h"

namespace SdrToHdr
{
bool BorderlessFullscreen::Enter(HWND window, const RECT& monitor)
{
    if (_active)
        return true;

    if (window == nullptr || !IsWindow(window))
        return false;

    _style = GetWindowLongPtrW(window, GWL_STYLE);
    _exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    GetWindowRect(window, &_rect);
    _active = true;

    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(window, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return true;
}

bool BorderlessFullscreen::Leave(HWND window)
{
    if (!_active)
        return true;

    _active = false;

    if (window == nullptr || !IsWindow(window))
        return false;

    SetWindowLongPtrW(window, GWL_STYLE, _style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, _exStyle);
    SetWindowPos(window, nullptr, _rect.left, _rect.top, _rect.right - _rect.left, _rect.bottom - _rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}
} // namespace SdrToHdr

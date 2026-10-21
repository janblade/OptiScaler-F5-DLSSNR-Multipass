// Not built with the precompiled header: Win32 only, so tests/sdr_to_hdr_dx11_gpu.cpp compiles it alone.
#include "SdrToHdr_Fullscreen.h"

namespace SdrToHdr
{
bool BorderlessFullscreen::Enter(HWND window, const RECT& monitor)
{
    if (monitor.right <= monitor.left || monitor.bottom <= monitor.top || window == nullptr || !IsWindow(window))
        return false;

    // A call from inside the window messages below (or from another thread meanwhile) changes nothing
    if (_transition.exchange(true))
        return true;

    if (_active.load())
    {
        _transition = false;
        return true;
    }

    _style = GetWindowLongPtrW(window, GWL_STYLE);
    _exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    _placement = {};
    _placement.length = sizeof(_placement);
    GetWindowPlacement(window, &_placement);

    // A minimised window would stay minimised under the new style: bring it up first (the placement above remembers it)
    if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);

    SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(window, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    _active = true; // after the window changed: the state is whole for whoever looks
    _transition = false;
    return true;
}

bool BorderlessFullscreen::Leave(HWND window)
{
    if (_transition.exchange(true))
        return true;

    if (!_active.load())
    {
        _transition = false;
        return true;
    }

    bool done = false;

    if (window != nullptr && IsWindow(window))
    {
        SetWindowLongPtrW(window, GWL_STYLE, _style);
        SetWindowLongPtrW(window, GWL_EXSTYLE, _exStyle);
        // The placement carries the normal rectangle and whether it was minimised or maximised (GetWindowRect of a minimised
        // window is (-32000, -32000))
        SetWindowPlacement(window, &_placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        done = true;
    }

    _active = false;
    _transition = false;
    return done;
}
} // namespace SdrToHdr

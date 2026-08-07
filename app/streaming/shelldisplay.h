#pragma once

#include "SDL_compat.h"

#include <QString>

// Asks GNOME Shell where a window is, through the MoonlightDisplay extension.
// The compositor is the only source that can answer on Wayland: SDL derives its
// answer from wl_surface enter/leave events, which also fire when a window
// merely overlaps another output, and it never learns a window position.
namespace ShellDisplay
{

struct WindowMonitor
{
    // True only when the shell answered and knows the window
    bool found = false;

    // The shell's own monitor index. For logs; the caller matches on the rect.
    int monitorIndex = -1;

    // The monitor rect in compositor-logical pixels, which is the space SDL
    // display bounds use here because main() disables
    // SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY
    SDL_Rect rect = {};

    // The window's workspace, or -1 when it has no one workspace and when the
    // extension is too old to report one. The monitor rect alone cannot tell a
    // move from a scroll, because a tiling window manager that scrolls a window
    // across an output boundary changes the monitor the shell reports. The
    // workspace only changes when the window really is moved.
    int workspaceIndex = -1;

    // Why the answer is unknown. Set only when found is false.
    QString unavailableReason;
};

// Looks the window up by title, then by application id if the title is unknown
// to the shell. Never blocks for more than a fraction of a second: any error,
// timeout, or missing extension comes back as found = false.
WindowMonitor getWindowMonitor(const QString& windowTitle, const QString& appId);

}

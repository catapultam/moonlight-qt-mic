#pragma once

#include "SDL_compat.h"

class StreamUtils
{
public:
    // Largest display dimension we're willing to offer as a stream resolution
    static constexpr int k_MaxSupportedDimension = 8192;

    // Frame rates we're willing to ask a host for
    static constexpr int k_MinSupportedFps = 10;
    static constexpr int k_MaxSupportedFps = 480;

    static
    Uint32 getPlatformWindowFlags();

    static
    void scaleSourceToDestinationSurface(SDL_Rect* src, SDL_Rect* dst);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_FRect* rect, int viewportWidth, int viewportHeight);

    static
    void screenSpaceToNormalizedDeviceCoords(SDL_Rect* src, SDL_FRect* dst, int viewportWidth, int viewportHeight);

    static
    bool getNativeDesktopMode(int displayIndex, SDL_DisplayMode* mode, SDL_Rect* safeArea);

    static
    int getDisplayRefreshRate(SDL_Window* window);

    static
    int normalizeRefreshRate(int refreshRate);

    static
    bool hasFastAes();

    static
    int getDrmFdForWindow(SDL_Window* window, bool* needsClose);

    static
    int getDrmFd(bool preferRenderNode);

    static
    void enterAsyncLoggingMode();

    static
    void exitAsyncLoggingMode();
};

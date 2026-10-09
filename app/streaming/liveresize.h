#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Helpers of the live resize feature. They have no SDL or Qt dependency,
// so app/tests/liveresize_test.cpp can build them alone.
namespace LiveResize {

// Reason codes of RESIZE_REFUSED. The values match LI_RESIZE_REFUSED_* in
// Limelight.h. Code 7 is set by the client itself.
enum Reason : uint16_t {
    ReasonBusy = 1,
    ReasonNotVirtualDisplay = 2,
    ReasonMultipleClients = 3,
    ReasonSizeLimit = 4,
    ReasonDisplayFailed = 5,
    ReasonEncoderFailed = 6,
    ReasonNotSupported = 7,
};

// 4:2:0 encoders and NV12 textures need even sizes. Round each dimension down.
inline void roundDownEven(int& width, int& height)
{
    width &= ~1;
    height &= ~1;
}

// Writes the overlay text for a refusal into buffer and returns buffer.
inline const char* reasonText(uint16_t reason, int width, int height, char* buffer, size_t length)
{
    switch (reason) {
    case ReasonBusy:
        snprintf(buffer, length, "Host is busy with a resize");
        break;
    case ReasonNotVirtualDisplay:
        snprintf(buffer, length, "Host does not stream a virtual display");
        break;
    case ReasonMultipleClients:
        snprintf(buffer, length, "Another client is connected");
        break;
    case ReasonSizeLimit:
        snprintf(buffer, length, "Host rejected the size %dx%d", width, height);
        break;
    case ReasonDisplayFailed:
        snprintf(buffer, length, "Host could not change the display");
        break;
    case ReasonEncoderFailed:
        snprintf(buffer, length, "Host encoder rejected the size");
        break;
    case ReasonNotSupported:
        snprintf(buffer, length, "Host does not support live resize");
        break;
    default:
        snprintf(buffer, length, "Host refused the resize (reason %u)", reason);
        break;
    }
    return buffer;
}

// The one request that can be pending at a time.
struct PendingRequest {
    bool active = false;
    int width = 0;
    int height = 0;
    uint32_t requestId = 0;

    // Returns false when a request is already pending.
    bool begin(int w, int h, uint32_t id)
    {
        if (active) {
            return false;
        }
        active = true;
        width = w;
        height = h;
        requestId = id;
        return true;
    }

    // True when a refusal with this id belongs to the pending request.
    bool matchesRefusal(uint32_t id) const
    {
        return active && id == requestId;
    }

    // True when a decoded frame of this size completes the pending request.
    bool matchesFrame(int w, int h) const
    {
        return active && w == width && h == height;
    }

    void clear()
    {
        active = false;
        width = 0;
        height = 0;
        requestId = 0;
    }
};

enum class FrameSizeClass {
    Original,   // the size the decoder was created with
    Padding,    // encoder padding to crop (larger by less than 64 in both dimensions)
    NewSize,    // the stream size changed; recreate the decoder
};

// Classifies the size of a decoded frame. expectedW/H is the size of a pending
// resize request, or 0 when none is pending. originalW/H is the size the
// decoder was created with.
inline FrameSizeClass classifyFrameSize(int frameW, int frameH, int expectedW, int expectedH, int originalW, int originalH)
{
    if (frameW == originalW && frameH == originalH) {
        return FrameSizeClass::Original;
    }

    if (expectedW != 0 && frameW == expectedW && frameH == expectedH) {
        return FrameSizeClass::NewSize;
    }

    int cropWidth = frameW - originalW;
    int cropHeight = frameH - originalH;
    if (cropWidth >= 0 && cropWidth < 64 && cropHeight >= 0 && cropHeight < 64) {
        return FrameSizeClass::Padding;
    }

    return FrameSizeClass::NewSize;
}

}

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

// True when a frame of frameW x frameH is a frame of baseW x baseH with
// encoder padding: larger by 0 to 63 in both dimensions.
inline bool isPaddedSize(int frameW, int frameH, int baseW, int baseH)
{
    int cropWidth = frameW - baseW;
    int cropHeight = frameH - baseH;
    return cropWidth >= 0 && cropWidth < 64 && cropHeight >= 0 && cropHeight < 64;
}

// Classifies the size of a decoded frame. expectedW/H is the size of a pending
// resize request, or 0 when none is pending. originalW/H is the size the
// decoder was created with.
//
// For NewSize, the function writes the new stream size to streamW/H (when
// they are not null). This is the requested size when the frame has that
// size or that size with encoder padding; then the new decoder crops the
// padding. Else it is the frame size. For the other classes, the function
// does not write streamW/H.
//
// requestFrameW/H is the size of the last frame that the decoder decoded
// before it saw the request, or 0 when there was none. keyFrame is true when
// this frame is a key frame. On an encoder that pads, an old frame can be a
// padded size of the request. Thus a frame of requestFrameW x requestFrameH
// is a frame of the new stream only when it is a key frame.
inline FrameSizeClass classifyFrameSize(int frameW, int frameH, int expectedW, int expectedH, int originalW, int originalH,
                                        int* streamW = nullptr, int* streamH = nullptr,
                                        int requestFrameW = 0, int requestFrameH = 0, bool keyFrame = false)
{
    if (frameW == originalW && frameH == originalH) {
        return FrameSizeClass::Original;
    }

    // The requested size, with or without padding, wins over the padding rule
    // of the old size
    bool sameSizeAsOldFrame = frameW == requestFrameW && frameH == requestFrameH;
    if (expectedW != 0 && isPaddedSize(frameW, frameH, expectedW, expectedH) &&
            (!sameSizeAsOldFrame || keyFrame)) {
        if (streamW != nullptr && streamH != nullptr) {
            *streamW = expectedW;
            *streamH = expectedH;
        }
        return FrameSizeClass::NewSize;
    }

    if (isPaddedSize(frameW, frameH, originalW, originalH)) {
        return FrameSizeClass::Padding;
    }

    // The host changed the size on its own, or sent a size that we did not request
    if (streamW != nullptr && streamH != nullptr) {
        *streamW = frameW;
        *streamH = frameH;
    }
    return FrameSizeClass::NewSize;
}

}

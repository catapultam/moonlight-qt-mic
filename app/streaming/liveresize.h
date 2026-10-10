#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// Helpers of the live resize feature. They have no SDL or Qt dependency,
// so app/tests/liveresize_test.cpp can build them alone.
namespace LiveResize {

// Reason codes of RESIZE_REFUSED. The values match LI_RESIZE_REFUSED_* in
// Limelight.h. Codes 7 and 8 are set by the client itself.
enum Reason : uint16_t {
    ReasonBusy = 1,
    ReasonNotVirtualDisplay = 2,
    ReasonMultipleClients = 3,
    ReasonSizeLimit = 4,
    ReasonDisplayFailed = 5,
    ReasonEncoderFailed = 6,
    ReasonNotSupported = 7,
    ReasonTimeout = 8,
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
    case ReasonTimeout:
        snprintf(buffer, length, "Host did not answer the resize request");
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

// What the session does with a refusal or a timeout.
struct RefusalAction {
    bool matched = false;   // false: not for the request in flight; ignore it
    bool show = false;      // show the reason text in the status overlay
    int width = 0;          // size of the refused request
    int height = 0;
};

// The state of the resize requests: the request in flight and one "next size"
// slot. The latest target wins. The session passes the time (SDL_GetTicks())
// in, so that the test can run without SDL.
//
// Rules:
// - At most one request is in flight.
// - A target that equals the stream size is dropped.
// - An automatic target is sent only when it was stable for DebounceMs.
//   A manual target (the hotkey) is sent at once.
// - BUSY: retry the latest target after BusyRetryMs.
// - Other refusals and timeouts: an automatic target of that size is not sent
//   again. The text is shown once per (reason, size). A manual request is
//   always sent and its refusal is always shown.
class ResizeController {
public:
    static constexpr uint32_t DebounceMs = 500;
    static constexpr uint32_t BusyRetryMs = 1000;

    PendingRequest inFlight;
    bool inFlightManual = false;

    // Puts a target in the next size slot.
    void setTarget(int w, int h, bool manual, uint32_t nowMs)
    {
        if (manual) {
            // The user asked for it now: no debounce and no BUSY wait
            m_RetryArmed = false;
        }
        else if (m_HasTarget && w == m_TargetWidth && h == m_TargetHeight) {
            // The same target: keep the time when it became stable
            return;
        }

        m_HasTarget = true;
        m_TargetWidth = w;
        m_TargetHeight = h;
        m_TargetManual = manual;
        m_TargetSinceMs = nowMs;
    }

    // Returns true and writes the size when the session must send a request now.
    // Drops a target that must not be sent.
    bool takeSend(int streamW, int streamH, uint32_t nowMs, int& w, int& h, bool& manual)
    {
        if (inFlight.active || !m_HasTarget) {
            return false;
        }

        if ((m_TargetWidth == streamW && m_TargetHeight == streamH) ||
                (!m_TargetManual && isBlocked(m_TargetWidth, m_TargetHeight))) {
            m_HasTarget = false;
            return false;
        }

        if (waitMs(nowMs) > 0) {
            return false;
        }

        m_HasTarget = false;
        m_RetryArmed = false;
        w = m_TargetWidth;
        h = m_TargetHeight;
        manual = m_TargetManual;
        return true;
    }

    // The session sent the request.
    void sent(int w, int h, uint32_t id, bool manual)
    {
        inFlight.clear();
        inFlight.begin(w, h, id);
        inFlightManual = manual;
    }

    // The request in flight completed (a frame of the new size came), or the
    // stream size changed in another way.
    void ended()
    {
        inFlight.clear();
        inFlightManual = false;
    }

    // The host refused the request with this id.
    RefusalAction refused(uint32_t id, uint16_t reason, uint32_t nowMs)
    {
        RefusalAction action;
        if (!inFlight.matchesRefusal(id)) {
            return action;
        }

        action.matched = true;
        action.width = inFlight.width;
        action.height = inFlight.height;
        bool manual = inFlightManual;
        ended();

        if (reason == ReasonBusy) {
            // Retry the latest target. When the slot is empty, that is the refused size.
            if (!m_HasTarget) {
                m_HasTarget = true;
                m_TargetWidth = action.width;
                m_TargetHeight = action.height;
                m_TargetManual = manual;
                m_TargetSinceMs = nowMs - DebounceMs;
            }
            m_RetryArmed = true;
            m_RetryAtMs = nowMs + BusyRetryMs;
            action.show = manual;
            return action;
        }

        action.show = block(reason, action.width, action.height) || manual;
        return action;
    }

    // The timeout of the request with this id expired.
    RefusalAction timedOut(uint32_t id)
    {
        RefusalAction action;
        if (!inFlight.matchesRefusal(id)) {
            return action;
        }

        action.matched = true;
        action.width = inFlight.width;
        action.height = inFlight.height;
        bool manual = inFlightManual;
        ended();
        action.show = block(ReasonTimeout, action.width, action.height) || manual;
        return action;
    }

    // Milliseconds until takeSend() can send, 0 when it can send now, or -1
    // when nothing waits (no target, or a request in flight).
    int32_t wakeDelayMs(uint32_t nowMs) const
    {
        if (inFlight.active || !m_HasTarget) {
            return -1;
        }
        return waitMs(nowMs);
    }

    // Forgets all state (end of the stream).
    void reset()
    {
        *this = ResizeController();
    }

private:
    struct Refusal {
        uint16_t reason;
        int width;
        int height;
    };

    // Keep the list small; the oldest entry goes first
    static constexpr size_t MaxRefusals = 64;

    int32_t waitMs(uint32_t nowMs) const
    {
        int32_t wait = 0;
        if (!m_TargetManual) {
            // Signed difference: correct when the tick count wraps
            int32_t debounce = (int32_t)(m_TargetSinceMs + DebounceMs - nowMs);
            if (debounce > wait) {
                wait = debounce;
            }
        }
        if (m_RetryArmed) {
            int32_t retry = (int32_t)(m_RetryAtMs - nowMs);
            if (retry > wait) {
                wait = retry;
            }
        }
        return wait;
    }

    bool isBlocked(int w, int h) const
    {
        for (const Refusal& refusal : m_Refusals) {
            if (refusal.width == w && refusal.height == h) {
                return true;
            }
        }
        return false;
    }

    // Records the refusal. Returns true when this (reason, size) is new.
    bool block(uint16_t reason, int w, int h)
    {
        for (const Refusal& refusal : m_Refusals) {
            if (refusal.reason == reason && refusal.width == w && refusal.height == h) {
                return false;
            }
        }
        if (m_Refusals.size() >= MaxRefusals) {
            m_Refusals.erase(m_Refusals.begin());
        }
        m_Refusals.push_back({reason, w, h});
        return true;
    }

    bool m_HasTarget = false;
    int m_TargetWidth = 0;
    int m_TargetHeight = 0;
    bool m_TargetManual = false;
    uint32_t m_TargetSinceMs = 0;
    bool m_RetryArmed = false;
    uint32_t m_RetryAtMs = 0;
    std::vector<Refusal> m_Refusals;
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

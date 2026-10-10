// Test of app/streaming/liveresize.h. Build and run in the moonlight toolbox:
//   g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test
//   /tmp/liveresize_test
#include "streaming/liveresize.h"

#include <cassert>
#include <cstdio>
#include <cstring>

static void testRoundDownEven()
{
    int w = 2537, h = 1391;
    LiveResize::roundDownEven(w, h);
    assert(w == 2536 && h == 1390);

    w = 3840; h = 2160;
    LiveResize::roundDownEven(w, h);
    assert(w == 3840 && h == 2160);

    // A tiny window gives 0x0; the host refuses it with SIZE_LIMIT
    w = 1; h = 1;
    LiveResize::roundDownEven(w, h);
    assert(w == 0 && h == 0);
}

static void testPendingRequest()
{
    LiveResize::PendingRequest pending;
    assert(!pending.active);
    assert(!pending.matchesRefusal(1));
    assert(!pending.matchesFrame(2536, 1390));

    assert(pending.begin(2536, 1390, 1));
    assert(pending.active);
    // A second request while one is pending is refused locally
    assert(!pending.begin(100, 100, 2));
    assert(pending.width == 2536 && pending.height == 1390 && pending.requestId == 1);

    assert(pending.matchesRefusal(1));
    assert(!pending.matchesRefusal(2));
    assert(pending.matchesFrame(2536, 1390));
    assert(!pending.matchesFrame(2536, 1392));

    // Review Focus 1: after a timeout and a new request, the old refusal is ignored
    pending.clear();
    assert(!pending.active);
    assert(pending.begin(3840, 2160, 2));
    assert(!pending.matchesRefusal(1));
    assert(pending.matchesRefusal(2));
}

static void testReasonText()
{
    char text[128];

    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonBusy, 0, 0, text, sizeof(text)), "Host is busy with a resize") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonNotVirtualDisplay, 0, 0, text, sizeof(text)), "Host does not stream a virtual display") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonMultipleClients, 0, 0, text, sizeof(text)), "Another client is connected") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonSizeLimit, 2536, 1390, text, sizeof(text)), "Host rejected the size 2536x1390") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonDisplayFailed, 0, 0, text, sizeof(text)), "Host could not change the display") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonEncoderFailed, 0, 0, text, sizeof(text)), "Host encoder rejected the size") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonNotSupported, 0, 0, text, sizeof(text)), "Host does not support live resize") == 0);
    assert(strcmp(LiveResize::reasonText(LiveResize::ReasonTimeout, 0, 0, text, sizeof(text)), "Host did not answer the resize request") == 0);
    // An unknown code from a newer host still gives a message
    assert(strcmp(LiveResize::reasonText(42, 0, 0, text, sizeof(text)), "Host refused the resize (reason 42)") == 0);
}

static void testClassifyFrameSize()
{
    using LiveResize::FrameSizeClass;
    using LiveResize::classifyFrameSize;

    // Same size as the decoder was created with
    assert(classifyFrameSize(2560, 1600, 0, 0, 2560, 1600) == FrameSizeClass::Original);

    // Encoder padding: larger by less than 64 in both dimensions, no request pending
    assert(classifyFrameSize(2576, 1616, 0, 0, 2560, 1600) == FrameSizeClass::Padding);

    // The requested size wins over the padding rule
    assert(classifyFrameSize(2570, 1610, 2570, 1610, 2560, 1600) == FrameSizeClass::NewSize);

    // A smaller frame is never padding
    assert(classifyFrameSize(2536, 1390, 2536, 1390, 2560, 1600) == FrameSizeClass::NewSize);

    // Review Focus 3: the host changed the size on its own, no request pending
    assert(classifyFrameSize(2536, 1390, 0, 0, 2560, 1600) == FrameSizeClass::NewSize);
    assert(classifyFrameSize(2624, 1664, 0, 0, 2560, 1600) == FrameSizeClass::NewSize);

    // The expected size does not match this frame, so the old rules apply
    assert(classifyFrameSize(2576, 1616, 3840, 2160, 2560, 1600) == FrameSizeClass::Padding);
}

static void testNewStreamSize()
{
    using LiveResize::FrameSizeClass;
    using LiveResize::classifyFrameSize;
    int width, height;

    // The frame has the requested size: the stream size is the frame size
    width = height = -1;
    assert(classifyFrameSize(2536, 1390, 2536, 1390, 2560, 1600, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2536 && height == 1390);

    // Padding encoder, smaller request: the stream size is the requested size,
    // so the new decoder crops the padding
    width = height = -1;
    assert(classifyFrameSize(2560, 1392, 2536, 1390, 2560, 1600, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2536 && height == 1390);

    // Padding encoder, request larger by 10: the requested size wins over the
    // padding rule of the old size
    width = height = -1;
    assert(classifyFrameSize(2576, 1616, 2570, 1610, 2560, 1600, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2570 && height == 1610);

    // Padding of the requested size is less than 64 in both dimensions
    assert(classifyFrameSize(2600, 1474, 2536, 1390, 2560, 1600) == FrameSizeClass::NewSize);
    width = height = -1;
    assert(classifyFrameSize(2600, 1474, 2536, 1390, 2560, 1600, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2600 && height == 1474);

    // No request pending: the stream size is the frame size
    width = height = -1;
    assert(classifyFrameSize(2536, 1390, 0, 0, 2560, 1600, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2536 && height == 1390);

    // The outputs do not change for Original and Padding
    width = height = -1;
    assert(classifyFrameSize(2560, 1600, 2536, 1390, 2560, 1600, &width, &height) == FrameSizeClass::Original);
    assert(classifyFrameSize(2576, 1616, 0, 0, 2560, 1600, &width, &height) == FrameSizeClass::Padding);
    assert(width == -1 && height == -1);
}

static void testOldFrameAfterRequest()
{
    using LiveResize::FrameSizeClass;
    using LiveResize::classifyFrameSize;
    int width, height;

    // The stream is 1920x1080 and the encoder sends 1920x1088. The last frame
    // before the request was 1920x1088. The request is 1900x1060.

    // An old frame (same coded size, not a key frame) is still padding
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1900, 1060, 1920, 1080, &width, &height,
                             1920, 1088, false) == FrameSizeClass::Padding);
    assert(width == -1 && height == -1);

    // A key frame of the new stream with the same coded size is the new size
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1900, 1060, 1920, 1080, &width, &height,
                             1920, 1088, true) == FrameSizeClass::NewSize);
    assert(width == 1900 && height == 1060);

    // A frame of a different coded size is the new size without a key frame
    width = height = -1;
    assert(classifyFrameSize(1904, 1072, 1900, 1060, 1920, 1080, &width, &height,
                             1920, 1088, false) == FrameSizeClass::NewSize);
    assert(width == 1900 && height == 1060);

    // The request equals the old coded size exactly: an old frame is still padding
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1920, 1088, 1920, 1080, &width, &height,
                             1920, 1088, false) == FrameSizeClass::Padding);
    assert(width == -1 && height == -1);

    // The same with a key frame after the request: the new size
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1920, 1088, 1920, 1080, &width, &height,
                             1920, 1088, true) == FrameSizeClass::NewSize);
    assert(width == 1920 && height == 1088);

    // No frame before the request (0x0): the frame size differs, so the new size
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1900, 1060, 1920, 1080, &width, &height,
                             0, 0, false) == FrameSizeClass::NewSize);
    assert(width == 1900 && height == 1060);
}

// A decoder recreate (device reset) during a pending request: the session sets the
// expected size on the new decoder again. The new decoder decoded no frame before it saw
// the request, thus its request frame size is 0x0.
static void testDecoderRecreate()
{
    using LiveResize::FrameSizeClass;
    using LiveResize::classifyFrameSize;
    int width, height;

    // The new decoder was created at the old size 1920x1080. A padding encoder sends the
    // new stream at 2560x1392 for the request 2536x1390: the stream size is the request
    width = height = -1;
    assert(classifyFrameSize(2560, 1392, 2536, 1390, 1920, 1080, &width, &height,
                             0, 0, true) == FrameSizeClass::NewSize);
    assert(width == 2536 && height == 1390);

    // Without the expected size the stream size would be the padded frame size
    width = height = -1;
    assert(classifyFrameSize(2560, 1392, 0, 0, 1920, 1080, &width, &height) == FrameSizeClass::NewSize);
    assert(width == 2560 && height == 1392);

    // Known limit: the first frame after the recreate is an IDR of the old stream. On a
    // padding encoder (1920x1088 for 1920x1080) it applies a small shrink request
    // (1900x1060) before the host changes the size.
    width = height = -1;
    assert(classifyFrameSize(1920, 1088, 1900, 1060, 1920, 1080, &width, &height,
                             0, 0, true) == FrameSizeClass::NewSize);
    assert(width == 1900 && height == 1060);
}

// A controller with a request of 1920x1080 (id 1) in flight. The stream is 2560x1600.
static void sendFirst(LiveResize::ResizeController& c, uint32_t now)
{
    int w, h;
    bool manual;
    c.setTarget(1920, 1080, false, now);
    assert(!c.takeSend(2560, 1600, now + 499, w, h, manual));
    assert(c.takeSend(2560, 1600, now + 500, w, h, manual));
    assert(w == 1920 && h == 1080 && !manual);
    c.sent(w, h, 1, manual);
    assert(c.inFlight.active && c.inFlight.requestId == 1);
}

static void testControllerDebounce()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    // No target: nothing to send and nothing to wait for
    assert(!c.takeSend(2560, 1600, 0, w, h, manual));
    assert(c.wakeDelayMs(0) == -1);

    // A new target waits 500 ms
    c.setTarget(1920, 1080, false, 1000);
    assert(c.wakeDelayMs(1000) == 500);
    assert(c.wakeDelayMs(1200) == 300);

    // The same target again does not restart the wait
    c.setTarget(1920, 1080, false, 1300);
    assert(c.wakeDelayMs(1300) == 200);

    // A different target restarts the wait
    c.setTarget(1900, 1060, false, 1400);
    assert(!c.takeSend(2560, 1600, 1500, w, h, manual));
    assert(c.wakeDelayMs(1500) == 400);
    assert(c.takeSend(2560, 1600, 1900, w, h, manual));
    assert(w == 1900 && h == 1060 && !manual);
    c.sent(w, h, 1, manual);

    // Nothing waits while a request is in flight
    assert(c.wakeDelayMs(1900) == -1);
}

static void testControllerTickWrap()
{
    // SDL_GetTicks() wraps after 49 days; the wait must still be 500 ms
    LiveResize::ResizeController c;
    int w, h;
    bool manual;
    uint32_t start = 0xFFFFFF00u;

    c.setTarget(1920, 1080, false, start);
    assert(!c.takeSend(2560, 1600, start + 499, w, h, manual));
    assert(c.takeSend(2560, 1600, start + 500, w, h, manual));
}

static void testControllerEqualToCurrent()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    // A target equal to the stream size does nothing
    c.setTarget(2560, 1600, false, 0);
    assert(!c.takeSend(2560, 1600, 1000, w, h, manual));
    assert(c.wakeDelayMs(1000) == -1);

    // Also for a manual request
    c.setTarget(2560, 1600, true, 0);
    assert(!c.takeSend(2560, 1600, 0, w, h, manual));
    assert(c.wakeDelayMs(0) == -1);
}

static void testControllerRapidTargets()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    // A drag during the request: many targets, the last one wins
    c.setTarget(1800, 1000, false, 600);
    c.setTarget(1700, 980, false, 650);
    c.setTarget(1600, 900, false, 700);
    c.setTarget(1600, 900, false, 750);
    assert(!c.takeSend(2560, 1600, 1500, w, h, manual));

    // The request in flight completes
    c.ended();
    assert(c.takeSend(1920, 1080, 1500, w, h, manual));
    assert(w == 1600 && h == 900);
    c.sent(w, h, 2, manual);

    // Exactly one follow-up send
    c.ended();
    assert(!c.takeSend(1600, 900, 5000, w, h, manual));
    assert(c.wakeDelayMs(5000) == -1);
}

static void testControllerFollowUpDebounce()
{
    // The follow-up waits until its own target is stable for 500 ms
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);
    c.setTarget(1600, 900, false, 1000);
    c.ended();
    assert(!c.takeSend(1920, 1080, 1200, w, h, manual));
    assert(c.wakeDelayMs(1200) == 300);
    assert(c.takeSend(1920, 1080, 1500, w, h, manual));
    assert(w == 1600 && h == 900);
}

static void testControllerDuplicateTargets()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    // The size in flight again: nothing more to send after it completes
    c.setTarget(1920, 1080, false, 600);
    c.ended();
    assert(!c.takeSend(1920, 1080, 2000, w, h, manual));

    // The same size as the stream: nothing
    c.setTarget(1920, 1080, false, 3000);
    assert(!c.takeSend(1920, 1080, 4000, w, h, manual));

    // A target that goes away and comes back to the size in flight
    LiveResize::ResizeController d;
    sendFirst(d, 0);
    d.setTarget(1600, 900, false, 600);
    d.setTarget(1920, 1080, false, 700);
    d.ended();
    assert(!d.takeSend(1920, 1080, 2000, w, h, manual));

    // The stream size comes back while a request is in flight: the latest target
    // (the old stream size) differs from the new stream size, so send it
    LiveResize::ResizeController e;
    sendFirst(e, 0);
    e.setTarget(2560, 1600, false, 600);
    e.ended();
    assert(e.takeSend(1920, 1080, 1100, w, h, manual));
    assert(w == 2560 && h == 1600);
}

static void testControllerBusyRetry()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonBusy, 1000);
    assert(action.matched && !action.show);
    assert(!c.inFlight.active);

    // Retry the same size after 1 s
    assert(c.wakeDelayMs(1000) == 1000);
    assert(!c.takeSend(2560, 1600, 1999, w, h, manual));
    assert(c.takeSend(2560, 1600, 2000, w, h, manual));
    assert(w == 1920 && h == 1080);
    c.sent(w, h, 2, manual);

    // BUSY with a newer target: retry the newer target after 1 s
    c.setTarget(1600, 900, false, 2100);
    action = c.refused(2, LiveResize::ReasonBusy, 2200);
    assert(action.matched && !action.show);
    assert(!c.takeSend(2560, 1600, 3100, w, h, manual));
    assert(c.takeSend(2560, 1600, 3200, w, h, manual));
    assert(w == 1600 && h == 900);
}

static void testControllerRefusalNoRetry()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonSizeLimit, 1000);
    assert(action.matched && action.show);
    assert(action.width == 1920 && action.height == 1080);
    assert(!c.inFlight.active);

    // The same size is not tried again automatically
    assert(!c.takeSend(2560, 1600, 5000, w, h, manual));
    c.setTarget(1600, 900, false, 6000);
    c.setTarget(1920, 1080, false, 6100);
    assert(!c.takeSend(2560, 1600, 7000, w, h, manual));
    assert(c.wakeDelayMs(7000) == -1);

    // A different size is tried
    c.setTarget(1600, 900, false, 8000);
    assert(c.takeSend(2560, 1600, 8500, w, h, manual));
    assert(w == 1600 && h == 900);
    c.sent(w, h, 2, manual);

    // The same reason for a different size is shown once
    action = c.refused(2, LiveResize::ReasonSizeLimit, 9000);
    assert(action.matched && action.show);

    // A manual request of a refused size is sent, and its refusal is shown again
    c.setTarget(1920, 1080, true, 10000);
    assert(c.takeSend(2560, 1600, 10000, w, h, manual));
    assert(w == 1920 && h == 1080 && manual);
    c.sent(w, h, 3, manual);
    action = c.refused(3, LiveResize::ReasonSizeLimit, 10500);
    assert(action.matched && action.show);
}

static void testControllerShowOnce()
{
    // An automatic request of a size that was refused manually: not sent, so not
    // shown again. A refusal of the same size with a new reason is shown.
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    c.setTarget(1920, 1080, true, 0);
    assert(c.takeSend(2560, 1600, 0, w, h, manual));
    c.sent(w, h, 1, manual);
    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonMultipleClients, 100);
    assert(action.show);

    c.setTarget(1920, 1080, false, 1000);
    assert(!c.takeSend(2560, 1600, 2000, w, h, manual));

    c.setTarget(1920, 1080, true, 3000);
    assert(c.takeSend(2560, 1600, 3000, w, h, manual));
    c.sent(w, h, 2, manual);
    action = c.refused(2, LiveResize::ReasonDisplayFailed, 3100);
    assert(action.show);

    // Automatic: the same (reason, size) a second time is not shown
    LiveResize::ResizeController d;
    sendFirst(d, 0);
    action = d.refused(1, LiveResize::ReasonDisplayFailed, 1000);
    assert(action.show);
    d.setTarget(1600, 900, false, 2000);
    assert(d.takeSend(2560, 1600, 2500, w, h, manual));
    d.sent(w, h, 2, manual);
    action = d.refused(2, LiveResize::ReasonDisplayFailed, 3000);
    assert(action.show);
    // An automatic request of 1600x900 gets the same refusal again: not shown.
    // (Only a manual request can send a refused size; here it is sent as automatic.)
    d.setTarget(1600, 900, true, 4000);
    assert(d.takeSend(2560, 1600, 4000, w, h, manual));
    d.sent(w, h, 3, false);
    action = d.refused(3, LiveResize::ReasonDisplayFailed, 4500);
    assert(action.matched && !action.show);
}

static void testControllerOldRefusal()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    // A refusal for another request id is ignored
    LiveResize::RefusalAction action = c.refused(7, LiveResize::ReasonSizeLimit, 1000);
    assert(!action.matched && !action.show);
    assert(c.inFlight.active && c.inFlight.requestId == 1);

    // Also after the request completed
    c.ended();
    action = c.refused(1, LiveResize::ReasonSizeLimit, 2000);
    assert(!action.matched && !action.show);

    // The size was not marked as refused
    c.setTarget(1600, 900, false, 3000);
    assert(c.takeSend(1920, 1080, 3500, w, h, manual));
    c.sent(w, h, 2, manual);
    c.ended();
    c.setTarget(1920, 1080, false, 4000);
    assert(c.takeSend(1600, 900, 4500, w, h, manual));
}

static void testControllerTimeout()
{
    LiveResize::ResizeController c;
    c.autoMode = true;
    int w, h;
    bool manual;

    sendFirst(c, 0);

    // A timeout for another request id is ignored
    LiveResize::RefusalAction action = c.timedOut(5, 10500);
    assert(!action.matched);
    assert(c.inFlight.active);

    // The timeout ends the request and is shown once for this size
    action = c.timedOut(1, 10500);
    assert(action.matched && action.show);
    assert(!c.inFlight.active);

    // M1: one automatic retry of the same size after 10 s
    assert(c.wakeDelayMs(10500) == 10000);
    assert(!c.takeSend(2560, 1600, 20499, w, h, manual));
    assert(c.takeSend(2560, 1600, 20500, w, h, manual));
    assert(w == 1920 && h == 1080 && !manual);
    c.sent(w, h, 2, manual);

    // The second timeout of the same size: not shown again, and the size is
    // blocked until the target changes
    action = c.timedOut(2, 30500);
    assert(action.matched && !action.show);
    assert(!c.takeSend(2560, 1600, 60000, w, h, manual));
    c.setTarget(1920, 1080, false, 61000);
    assert(!c.takeSend(2560, 1600, 62000, w, h, manual));

    // A different size is sent
    c.setTarget(1600, 900, false, 63000);
    assert(c.takeSend(2560, 1600, 63500, w, h, manual));
    c.sent(w, h, 3, manual);
    action = c.timedOut(3, 73500);
    assert(action.matched && action.show);

    // The target changed, so the old size can be sent again
    c.setTarget(1920, 1080, false, 74000);
    assert(c.takeSend(2560, 1600, 74500, w, h, manual));
    assert(w == 1920 && h == 1080);

    // A newer target in the slot is sent without the 10 s wait
    LiveResize::ResizeController d;
    d.autoMode = true;
    sendFirst(d, 0);
    d.setTarget(1600, 900, false, 600);
    action = d.timedOut(1, 10500);
    assert(action.matched);
    assert(d.takeSend(2560, 1600, 10500, w, h, manual));
    assert(w == 1600 && h == 900);

    // Automatic mode off: a manual request that times out is not retried
    LiveResize::ResizeController e;
    e.setTarget(1920, 1080, true, 0);
    assert(e.takeSend(2560, 1600, 0, w, h, manual));
    e.sent(w, h, 1, manual);
    action = e.timedOut(1, 10000);
    assert(action.matched && action.show);
    assert(e.wakeDelayMs(10000) == -1);
    assert(!e.takeSend(2560, 1600, 30000, w, h, manual));
}

static void testControllerManual()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    // A manual request with nothing in flight is sent at once
    c.setTarget(1920, 1080, true, 0);
    assert(c.wakeDelayMs(0) == 0);
    assert(c.takeSend(2560, 1600, 0, w, h, manual));
    assert(w == 1920 && h == 1080 && manual);
    c.sent(w, h, 1, manual);
    assert(c.inFlightManual);

    // A manual request while one is in flight fills the next size slot
    c.setTarget(1600, 900, true, 100);
    assert(!c.takeSend(2560, 1600, 100, w, h, manual));
    c.ended();
    assert(c.takeSend(1920, 1080, 200, w, h, manual));
    assert(w == 1600 && h == 900 && manual);
    c.sent(w, h, 2, manual);

    // A manual refusal is always shown, BUSY too. With automatic mode off,
    // a manual BUSY is not retried (I1).
    LiveResize::RefusalAction action = c.refused(2, LiveResize::ReasonBusy, 300);
    assert(action.matched && action.show);
    assert(c.wakeDelayMs(300) == -1);
    assert(!c.takeSend(1920, 1080, 1300, w, h, manual));
    assert(!c.takeSend(1920, 1080, 60000, w, h, manual));

    // An automatic target after a manual one wins and waits for the debounce
    sendFirst(c, 5000);
    c.setTarget(1280, 720, true, 5600);
    c.setTarget(1366, 768, false, 5700);
    c.ended();
    assert(!c.takeSend(1920, 1080, 6000, w, h, manual));
    assert(c.takeSend(1920, 1080, 6200, w, h, manual));
    assert(w == 1366 && h == 768 && !manual);

    // A manual target after an automatic one is sent at once
    LiveResize::ResizeController d;
    d.setTarget(1280, 720, false, 0);
    d.setTarget(1366, 768, true, 100);
    assert(d.takeSend(2560, 1600, 100, w, h, manual));
    assert(w == 1366 && h == 768 && manual);
}

static void testControllerManualBusyAuto()
{
    // I1: automatic mode on: a manual BUSY shows the text once and is retried
    // as an automatic request, with no more text
    LiveResize::ResizeController c;
    c.autoMode = true;
    int w, h;
    bool manual;

    c.setTarget(1920, 1080, true, 0);
    assert(c.takeSend(2560, 1600, 0, w, h, manual));
    c.sent(w, h, 1, manual);
    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonBusy, 100);
    assert(action.matched && action.show);
    assert(!c.takeSend(2560, 1600, 1099, w, h, manual));
    assert(c.takeSend(2560, 1600, 1100, w, h, manual));
    assert(w == 1920 && h == 1080 && !manual);
    c.sent(w, h, 2, manual);
    action = c.refused(2, LiveResize::ReasonBusy, 1200);
    assert(action.matched && !action.show);
}

static void testControllerBusyBackoff()
{
    // I1: automatic BUSY retries back off 1 s, 2 s, 4 s, 8 s, 8 s, ... and stop
    // after 30 s of BUSY for the same target
    LiveResize::ResizeController c;
    c.autoMode = true;
    int w, h;
    bool manual;
    uint32_t now = 0;
    uint32_t id = 1;

    sendFirst(c, 0);
    now = 1000;
    const int32_t delays[] = { 1000, 2000, 4000, 8000, 8000, 8000 };
    for (int32_t delay : delays) {
        LiveResize::RefusalAction action = c.refused(id, LiveResize::ReasonBusy, now);
        assert(action.matched && !action.show);
        assert(c.wakeDelayMs(now) == delay);
        assert(!c.takeSend(2560, 1600, now + delay - 1, w, h, manual));
        now += delay;
        assert(c.takeSend(2560, 1600, now, w, h, manual));
        assert(w == 1920 && h == 1080);
        c.sent(w, h, ++id, manual);
    }
    // First BUSY at 1000; now is 32000, more than 30 s later: stop
    LiveResize::RefusalAction action = c.refused(id, LiveResize::ReasonBusy, now);
    assert(action.matched && !action.show);
    assert(c.wakeDelayMs(now) == -1);
    assert(!c.takeSend(2560, 1600, now + 60000, w, h, manual));

    // The same target again (a pump reads the window again): not sent
    c.setTarget(1920, 1080, false, now + 100);
    assert(!c.takeSend(2560, 1600, now + 1000, w, h, manual));

    // A new different target restarts it
    c.setTarget(1600, 900, false, now + 2000);
    assert(c.takeSend(2560, 1600, now + 2500, w, h, manual));
    c.sent(w, h, ++id, manual);
    action = c.refused(id, LiveResize::ReasonBusy, now + 2600);
    assert(c.wakeDelayMs(now + 2600) == 1000);

    // Back to the old size: the target changed, so it is sent
    c.setTarget(1920, 1080, false, now + 3000);
    assert(c.takeSend(2560, 1600, now + 3600, w, h, manual));
    assert(w == 1920 && h == 1080);
    c.sent(w, h, ++id, manual);
    // The back-off of this size starts again at 1 s
    action = c.refused(id, LiveResize::ReasonBusy, now + 3700);
    assert(c.wakeDelayMs(now + 3700) == 1000);

    // A success resets the back-off
    LiveResize::ResizeController d;
    d.autoMode = true;
    sendFirst(d, 0);
    d.refused(1, LiveResize::ReasonBusy, 1000);
    assert(d.takeSend(2560, 1600, 2000, w, h, manual));
    d.sent(w, h, 2, manual);
    d.ended();
    d.setTarget(1600, 900, false, 3000);
    assert(d.takeSend(1920, 1080, 3500, w, h, manual));
    d.sent(w, h, 3, manual);
    d.refused(3, LiveResize::ReasonBusy, 4000);
    assert(d.wakeDelayMs(4000) == 1000);
}

static void testControllerRefusalExpiry()
{
    // M2: a refused size is blocked for 60 s only
    LiveResize::ResizeController c;
    c.autoMode = true;
    int w, h;
    bool manual;

    sendFirst(c, 0);
    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonMultipleClients, 1000);
    assert(action.show);
    c.setTarget(1920, 1080, false, 30000);
    assert(!c.takeSend(2560, 1600, 60999, w, h, manual));
    c.setTarget(1920, 1080, false, 61000);
    assert(c.takeSend(2560, 1600, 61500, w, h, manual));
    c.sent(w, h, 2, manual);
    // The entry expired, so the text is shown again
    action = c.refused(2, LiveResize::ReasonMultipleClients, 62000);
    assert(action.show);

    // M2: a stream size change clears the list
    LiveResize::ResizeController d;
    d.autoMode = true;
    sendFirst(d, 0);
    d.refused(1, LiveResize::ReasonMultipleClients, 1000);
    d.setTarget(1600, 900, false, 2000);
    assert(d.takeSend(2560, 1600, 2500, w, h, manual));
    d.sent(w, h, 2, manual);
    d.ended();
    d.setTarget(1920, 1080, false, 3000);
    assert(d.takeSend(1600, 900, 3500, w, h, manual));
    assert(w == 1920 && h == 1080);
}

static void testControllerSendFailed()
{
    // M3: a send failure keeps the target and retries with the back-off
    LiveResize::ResizeController c;
    c.autoMode = true;
    int w, h;
    bool manual;

    c.setTarget(1920, 1080, false, 0);
    assert(c.takeSend(2560, 1600, 500, w, h, manual));
    c.sendFailed(w, h, manual, 500);
    assert(!c.inFlight.active);
    assert(c.wakeDelayMs(500) == 1000);
    assert(!c.takeSend(2560, 1600, 1499, w, h, manual));
    assert(c.takeSend(2560, 1600, 1500, w, h, manual));
    assert(w == 1920 && h == 1080);
    c.sendFailed(w, h, manual, 1500);
    assert(c.wakeDelayMs(1500) == 2000);

    // A newer target in the slot is not replaced by the failed one
    LiveResize::ResizeController d;
    d.autoMode = true;
    d.setTarget(1920, 1080, false, 0);
    assert(d.takeSend(2560, 1600, 500, w, h, manual));
    d.setTarget(1600, 900, false, 600);
    d.sendFailed(1920, 1080, false, 700);
    assert(d.takeSend(2560, 1600, 1700, w, h, manual));
    assert(w == 1600 && h == 900);

    // Automatic mode off: a manual send failure is not retried
    LiveResize::ResizeController e;
    e.setTarget(1920, 1080, true, 0);
    assert(e.takeSend(2560, 1600, 0, w, h, manual));
    e.sendFailed(w, h, manual, 0);
    assert(e.wakeDelayMs(0) == -1);
    assert(!e.takeSend(2560, 1600, 5000, w, h, manual));
}

static void testControllerRefusalDropsSlot()
{
    // M4: a manual target of the refused size in the slot is dropped
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    c.setTarget(1920, 1080, true, 0);
    assert(c.takeSend(2560, 1600, 0, w, h, manual));
    c.sent(w, h, 1, manual);
    c.setTarget(1920, 1080, true, 100);
    LiveResize::RefusalAction action = c.refused(1, LiveResize::ReasonSizeLimit, 200);
    assert(action.matched && action.show);
    assert(!c.takeSend(2560, 1600, 200, w, h, manual));
    assert(c.wakeDelayMs(200) == -1);

    // A target of another size stays
    LiveResize::ResizeController d;
    d.setTarget(1920, 1080, true, 0);
    assert(d.takeSend(2560, 1600, 0, w, h, manual));
    d.sent(w, h, 1, manual);
    d.setTarget(1600, 900, true, 100);
    d.refused(1, LiveResize::ReasonSizeLimit, 200);
    assert(d.takeSend(2560, 1600, 200, w, h, manual));
    assert(w == 1600 && h == 900);
}

static void testControllerReset()
{
    LiveResize::ResizeController c;
    int w, h;
    bool manual;

    sendFirst(c, 0);
    c.setTarget(1600, 900, false, 600);
    c.reset();
    assert(!c.inFlight.active);
    assert(!c.takeSend(2560, 1600, 5000, w, h, manual));
    assert(c.wakeDelayMs(5000) == -1);
}

int main()
{
    testRoundDownEven();
    testPendingRequest();
    testReasonText();
    testClassifyFrameSize();
    testNewStreamSize();
    testOldFrameAfterRequest();
    testDecoderRecreate();
    testControllerDebounce();
    testControllerTickWrap();
    testControllerEqualToCurrent();
    testControllerRapidTargets();
    testControllerFollowUpDebounce();
    testControllerDuplicateTargets();
    testControllerBusyRetry();
    testControllerRefusalNoRetry();
    testControllerShowOnce();
    testControllerOldRefusal();
    testControllerTimeout();
    testControllerManual();
    testControllerManualBusyAuto();
    testControllerBusyBackoff();
    testControllerRefusalExpiry();
    testControllerSendFailed();
    testControllerRefusalDropsSlot();
    testControllerReset();
    puts("liveresize_test: all checks passed");
    return 0;
}

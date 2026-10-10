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

int main()
{
    testRoundDownEven();
    testPendingRequest();
    testReasonText();
    testClassifyFrameSize();
    testNewStreamSize();
    testOldFrameAfterRequest();
    testDecoderRecreate();
    puts("liveresize_test: all checks passed");
    return 0;
}

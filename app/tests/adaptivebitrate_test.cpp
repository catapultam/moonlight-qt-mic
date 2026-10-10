// Test of app/streaming/adaptivebitrate.h. Build and run in the moonlight toolbox:
//   g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/adaptivebitrate_test.cpp -o /tmp/adaptivebitrate_test
//   /tmp/adaptivebitrate_test
#include "streaming/adaptivebitrate.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace AdaptiveBitrate;

// A fake stream: 60 fps (15 frames each tick), 100 video packets each tick, RTT 10 ms.
struct Sim {
    Controller c;
    Sample s;
    uint32_t nextId = 1;
    uint32_t framesPerTick = 15;
    uint32_t packetsPerTick = 100;

    explicit Sim(uint32_t ceiling, bool adaptive = true)
    {
        s.nowMs = 1000;
        s.rttMs = 10;
        s.measuredMbps = ceiling * 0.8 / 1000.0;
        c.start(ceiling, s.nowMs, adaptive);
    }

    Decision step(uint32_t lost = 0, uint32_t rtt = 10, uint32_t recovered = 0)
    {
        s.nowMs += TICK_MS;
        s.framesFinished += framesPerTick;
        s.framesLost += lost;
        s.packetsVideo += packetsPerTick;
        s.packetsFecRecovered += recovered;
        s.rttMs = rtt;
        return c.tick(s);
    }

    uint32_t send(const Decision& d)
    {
        uint32_t id = nextId++;
        c.sent(d, id, s.nowMs);
        return id;
    }

    // The host answer. The encoder runs at 80 % of the accepted value and the
    // measured rate follows the encoder.
    StatusResult answer(uint32_t id, uint32_t requested, uint16_t status = StatusApplied, uint32_t accepted = 0)
    {
        if (accepted == 0) {
            accepted = requested;
        }
        uint32_t encoder = accepted * 8 / 10;
        s.measuredMbps = encoder / 1000.0;
        return c.onStatus(id, status, requested, accepted, encoder, s.nowMs);
    }

    void sendAndAnswer(const Decision& d, uint16_t status = StatusApplied)
    {
        uint32_t id = send(d);
        answer(id, d.targetKbps, status);
    }

    // Runs to the start request and answers it with UNCHANGED
    Decision runStart()
    {
        for (int i = 0; i < 40; i++) {
            Decision d = step();
            if (d.send) {
                assert(d.reason == Reason::Start);
                sendAndAnswer(d, StatusUnchanged);
                return d;
            }
        }
        assert(false);
        return Decision();
    }

    // Steps until a decision sends; returns the step count, or -1 after maxSteps
    int stepUntilSend(Decision& out, int maxSteps, uint32_t lost = 0, uint32_t rtt = 10, uint32_t recovered = 0)
    {
        for (int i = 1; i <= maxSteps; i++) {
            out = step(lost, rtt, recovered);
            if (out.send) {
                return i;
            }
        }
        return -1;
    }
};

// Spec 8.1 test 1
static void testCleanNetwork()
{
    Sim sim(40000);
    Decision start = sim.runStart();
    assert(start.targetKbps == 40000);
    for (int i = 0; i < 200; i++) {
        assert(!sim.step().send);
    }
    assert(sim.c.targetKbps() == 40000);
}

// Spec 8.1 test 2
static void testStartSettle()
{
    Sim sim(40000);
    for (int i = 0; i < 15; i++) {
        assert(!sim.step(5, 80, 10).send);
    }
    Decision d = sim.step(5, 80, 10);
    assert(d.send && d.reason == Reason::Start && d.targetKbps == 40000);
}

// Spec 8.1 test 3
static void testSustainedLoss()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) == 8);  // two lossy 1 s windows
    assert(d.reason == Reason::Loss && d.fromKbps == 40000 && d.targetKbps == 30000);
    uint64_t firstMs = sim.s.nowMs;
    sim.sendAndAnswer(d);
    assert(sim.stepUntilSend(d, 40, 1) > 0);
    assert(sim.s.nowMs - firstMs >= MIN_DEC_INTERVAL_MS);
    assert(d.reason == Reason::Loss && d.targetKbps == 22500);
}

// Spec 8.1 test 4: the Wi-Fi stall of the memory note (D8)
static void testIsolatedBurst()
{
    Sim sim(40000);
    sim.runStart();
    for (int i = 0; i < 12; i++) {
        assert(!sim.step().send);
    }
    Decision d = sim.step(7, 200);  // 7 of 60 frames in the window (12 %), one RTT spike
    bool isolatedSeen = d.isolated;
    assert(!d.send);
    for (int i = 0; i < 40; i++) {
        d = sim.step();
        isolatedSeen = isolatedSeen || d.isolated;
        assert(!d.send);
    }
    assert(isolatedSeen && sim.c.targetKbps() == 40000);
}

// Review Focus 4: the stall falls on two ticks across a window boundary
static void testStallAcrossWindows()
{
    Sim sim(40000);
    sim.runStart();
    for (int i = 0; i < 12; i++) {
        assert(!sim.step().send);
    }
    assert(!sim.step(4, 150).send);
    assert(!sim.step(3, 120).send);
    for (int i = 0; i < 40; i++) {
        assert(!sim.step().send);
    }
    assert(sim.c.targetKbps() == 40000);
}

// Spec 8.1 test 5
static void testHeavyLossWithRttRise()
{
    Sim sim(40000);
    sim.runStart();
    for (int i = 0; i < 8; i++) {
        assert(!sim.step().send);
    }
    Decision d;
    assert(sim.stepUntilSend(d, 4, 3, 60) == 3);
    assert(d.reason == Reason::Loss && d.targetKbps == 30000);
}

// Spec 8.1 test 6, and a constant added delay (spec 8.3 step 5)
static void testDelay()
{
    Sim sim(40000);
    sim.runStart();
    for (int i = 0; i < 8; i++) {
        assert(!sim.step().send);
    }
    Decision d;
    assert(sim.stepUntilSend(d, 12, 0, 35) == 7);
    assert(d.reason == Reason::Delay && d.targetKbps == 36000);
    sim.sendAndAnswer(d);

    // The RTT stays: one decrease at most, then the baseline follows the RTT
    bool rebased = false;
    for (int i = 0; i < 80; i++) {
        Decision x = sim.step(0, 35);
        rebased = rebased || x.rebased;
        assert(!x.send || x.reason == Reason::Increase);
        if (x.send) {
            sim.sendAndAnswer(x);
        }
    }
    assert(rebased);
    assert(sim.c.minTargetKbps() == 36000);
}

// Spec 8.1 test 7
static void testFecPressure()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 12, 0, 10, 4) == 8);
    assert(d.reason == Reason::FecPressure && d.targetKbps == 36000);
    assert(d.fecPct > 3.9 && d.fecPct < 4.1);
}

// A static picture (measured 10 % of the encoder value) with sustained delay: no
// decrease. The end-to-end test cut 64 -> 10 Mbps on a static desktop on RTT spikes.
static void testStaticStreamDelayNoDecrease()
{
    Sim sim(40000);
    sim.runStart();
    sim.s.measuredMbps = 32.0 * 0.10;  // encoder 32000
    for (int i = 0; i < 8; i++) {
        assert(!sim.step().send);
    }
    Decision d;
    for (int i = 0; i < 40; i++) {
        d = sim.step(0, 35);
        assert(!d.send && !d.rebased);
    }
    assert(!d.isolated);  // sustained delay is not an isolated burst (rule 4)
    assert(sim.c.targetKbps() == 40000);
}

// A busy stream (measured 60 % of the encoder value) with sustained delay: a decrease
static void testBusyStreamDelayDecreases()
{
    Sim sim(40000);
    sim.runStart();
    sim.s.measuredMbps = 32.0 * 0.60;
    for (int i = 0; i < 8; i++) {
        assert(!sim.step().send);
    }
    Decision d;
    assert(sim.stepUntilSend(d, 12, 0, 35) == 7);
    assert(d.reason == Reason::Delay && d.targetKbps == 36000);
}

// A static picture with FEC pressure: no decrease
static void testStaticStreamFecNoDecrease()
{
    Sim sim(40000);
    sim.runStart();
    sim.s.measuredMbps = 32.0 * 0.10;
    Decision d;
    for (int i = 0; i < 40; i++) {
        d = sim.step(0, 10, 4);
        assert(!d.send && !d.isolated);
    }
    assert(sim.c.targetKbps() == 40000);
}

// A static picture with sustained loss: the loss rule still decreases
static void testStaticStreamLossDecreases()
{
    Sim sim(40000);
    sim.runStart();
    sim.s.measuredMbps = 32.0 * 0.10;
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) == 8);
    assert(d.reason == Reason::Loss && d.targetKbps == 20000);  // MAX_CUT
}

// Spec 8.1 test 8
static void testRecovery()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    sim.sendAndAnswer(d);
    uint64_t decreaseMs = sim.s.nowMs;
    assert(sim.c.minTargetKbps() == 30000 && sim.c.maxTargetKbps() == 40000);

    assert(sim.stepUntilSend(d, 60) > 0);
    assert(d.reason == Reason::Increase && d.targetKbps == 32400);  // 8 %
    assert(sim.s.nowMs - decreaseMs >= CHANGE_SETTLE_MS + STABLE_MS - TICK_MS);
    sim.sendAndAnswer(d);

    assert(sim.stepUntilSend(d, 60) > 0 && d.targetKbps == 34992);  // 8 %
    sim.sendAndAnswer(d);

    assert(sim.stepUntilSend(d, 60) > 0 && d.targetKbps == 36041);  // near the failure rate: 3 %
    uint32_t mean = sim.c.meanTargetKbps();
    assert(mean > 30000 && mean < 40000);
}

// Spec 8.1 test 9
static void testAppLimited()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0);
    sim.sendAndAnswer(d);
    sim.s.measuredMbps = 24.0 * 0.2;  // a static picture
    for (int i = 0; i < 120; i++) {
        assert(!sim.step().send);
    }
    assert(sim.c.targetKbps() == 30000);
}

// A busy stream measures 63-75 % of the encoder value (end-to-end test). On a clean
// link this must not block the increase.
static void testBusyStreamIncreases()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    sim.sendAndAnswer(d);
    sim.s.measuredMbps = 24.0 * 0.41;  // 41 % of encoder 24000, just above the guard
    assert(sim.stepUntilSend(d, 60) > 0);
    assert(d.reason == Reason::Increase && d.targetKbps == 32400);
}

// Just below the app-limited guard (39 % of the encoder value): no increase
static void testStaticStreamNoIncrease()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    sim.sendAndAnswer(d);
    sim.s.measuredMbps = 24.0 * 0.39;
    for (int i = 0; i < 120; i++) {
        assert(!sim.step().send);
    }
    assert(sim.c.targetKbps() == 30000);
}

// Review Focus 3
static void testMaxCut()
{
    Sim sim(40000);
    sim.runStart();
    sim.s.measuredMbps = 1.0;
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0);
    assert(d.reason == Reason::Loss && d.targetKbps == 20000);
}

// Spec 8.1 test 10
static void testFloorAndCeiling()
{
    {
        Sim sim(2000);
        sim.runStart();
        assert(sim.c.floorKbps() == 1500);
        Decision d;
        assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 1500);
        sim.sendAndAnswer(d);
        for (int i = 0; i < 80; i++) {
            assert(!sim.step(1).send);
        }
        assert(sim.c.targetKbps() == 1500);
    }
    {
        Sim sim(40000);
        sim.runStart();
        Decision d;
        assert(sim.stepUntilSend(d, 20, 1) > 0);
        sim.sendAndAnswer(d);
        uint32_t highest = 0;
        for (int i = 0; i < 600; i++) {
            d = sim.step();
            if (d.send) {
                assert(d.reason == Reason::Increase && d.targetKbps <= 40000);
                highest = d.targetKbps;
                sim.sendAndAnswer(d);
            }
        }
        assert(highest == 40000 && sim.c.targetKbps() == 40000);
    }
}

// Spec 8.1 test 11
static void testDeadBand()
{
    assert(!passesDeadBand(10000, 10200, 1500, 40000));  // 2 %
    assert(passesDeadBand(10000, 10300, 1500, 40000));  // 3 %
    assert(!passesDeadBand(5000, 5200, 1500, 40000));  // below 250 kbps
    assert(passesDeadBand(1600, 1500, 1500, 40000));  // to the floor
    assert(passesDeadBand(39900, 40000, 1500, 40000));  // to the ceiling
    assert(!passesDeadBand(20000, 20000, 1500, 40000));  // no change
}

// Spec 8.1 test 12
static void testPendingRequest()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0);
    uint32_t id = sim.send(d);
    assert(sim.c.pending());
    for (int i = 0; i < 8; i++) {
        assert(!sim.step(1).send);
    }
    assert(sim.c.onStatus(id - 1, StatusApplied, 40000, 40000, 32000, sim.s.nowMs) == StatusResult::Ignored);
    assert(sim.c.pending());
    assert(sim.answer(id, d.targetKbps) == StatusResult::Accepted);
    assert(!sim.c.pending());
    assert(sim.answer(id, d.targetKbps) == StatusResult::Ignored);

    // Timeout: no answer for REQUEST_TIMEOUT_MS
    assert(sim.stepUntilSend(d, 40, 1) > 0);
    id = sim.send(d);
    Decision t;
    int steps = 0;
    do {
        t = sim.step(1);
        steps++;
    } while (!t.timedOut && steps < 20);
    assert(t.timedOut && t.timedOutRequestId == id && steps == 12);
    assert(!sim.c.pending());
    // A late answer for the last request still gives the host values
    assert(sim.answer(id, d.targetKbps) == StatusResult::Accepted);
}

// Spec 8.1 test 13 and Review Focus 5
static void testHostCap()
{
    {
        Sim sim(40000);
        Decision d;
        assert(sim.stepUntilSend(d, 20) > 0 && d.reason == Reason::Start);
        uint32_t id = sim.send(d);
        assert(sim.answer(id, 40000, StatusUnchanged, 20000) == StatusResult::Accepted);
        assert(sim.c.ceilingKbps() == 20000 && sim.c.targetKbps() == 20000);
        Decision r = sim.c.setCeiling(60000, sim.s.nowMs);
        assert(sim.c.ceilingKbps() == 20000 && !r.send);
    }
    {
        // A bad host answers more than the request: the ceiling does not go up
        Sim sim(40000);
        Decision d;
        assert(sim.stepUntilSend(d, 20) > 0);
        uint32_t id = sim.send(d);
        assert(sim.answer(id, 40000, StatusApplied, 90000) == StatusResult::Accepted);
        assert(sim.c.ceilingKbps() == 40000 && sim.c.targetKbps() == 40000);
    }
}

// Spec 8.1 test 14
static void testSetCeiling()
{
    {
        // Not limited by the network: the target follows the ceiling
        Sim sim(40000);
        sim.runStart();
        Decision d = sim.c.setCeiling(20000, sim.s.nowMs);
        assert(d.send && d.reason == Reason::Ceiling && d.fromKbps == 40000 && d.targetKbps == 20000);
        sim.sendAndAnswer(d);
        for (int i = 0; i < 12; i++) {
            assert(!sim.step(1).send);  // RESTART_SETTLE_MS
        }
    }
    {
        // Limited by the network: the target stays, but not above the new ceiling
        Sim sim(40000);
        sim.runStart();
        Decision d;
        assert(sim.stepUntilSend(d, 20, 1) > 0);
        sim.sendAndAnswer(d);
        Decision r = sim.c.setCeiling(60000, sim.s.nowMs);
        assert(!r.send && sim.c.targetKbps() == 30000 && sim.c.ceilingKbps() == 60000);
        r = sim.c.setCeiling(25000, sim.s.nowMs);
        assert(r.send && r.targetKbps == 25000 && sim.c.ceilingKbps() == 25000);
    }
    {
        // A resize while a request is pending sends at once; the older answer is ignored
        Sim sim(40000);
        sim.runStart();
        Decision d;
        assert(sim.stepUntilSend(d, 20, 1) > 0);
        uint32_t oldId = sim.send(d);
        Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
        assert(r.send && r.targetKbps == 20000);
        uint32_t newId = sim.send(r);
        assert(sim.answer(oldId, d.targetKbps) == StatusResult::Ignored);
        assert(sim.answer(newId, r.targetKbps) == StatusResult::Accepted);
    }
}

// Spec 8.1 test 15
static void testRestartMode()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    sim.sendAndAnswer(d, StatusAppliedRestart);
    assert(sim.c.restartMode());
    assert(sim.stepUntilSend(d, 80) > 0);
    assert(d.reason == Reason::Increase && d.targetKbps == 34800);  // 16 %
    uint64_t firstIncreaseMs = sim.s.nowMs;
    sim.sendAndAnswer(d, StatusAppliedRestart);
    assert(sim.stepUntilSend(d, 120) > 0);
    assert(d.reason == Reason::Increase && d.targetKbps == 36888);  // near the failure rate: 6 %
    assert(sim.s.nowMs - firstIncreaseMs >= MIN_INC_INTERVAL_RESTART_MS);
}

// Spec 8.1 test 16
static void testStartRequest()
{
    Sim sim(40000);
    Decision d;
    assert(sim.stepUntilSend(d, 20) == 16 && d.reason == Reason::Start && d.targetKbps == 40000);
    uint32_t id = sim.send(d);
    for (int i = 0; i < 8; i++) {
        assert(!sim.step(3, 60, 10).send);
    }
    assert(sim.answer(id, 40000, StatusUnchanged) == StatusResult::Accepted);
    assert(sim.c.encoderKbps() == 32000);
}

// Review Focus 2
static void testStartNoAnswer()
{
    Sim sim(40000);
    int starts = 0;
    bool stopped = false;
    for (int i = 0; i < 200 && !stopped; i++) {
        Decision d = sim.step();
        if (d.send) {
            assert(d.reason == Reason::Start);
            sim.send(d);
            starts++;
        }
        stopped = d.stopped;
    }
    assert(stopped && starts == 3 && !sim.c.running());
    for (int i = 0; i < 20; i++) {
        assert(!sim.step(5).send);
    }
}

// Spec 8.1 test 17
static void testSecondStart()
{
    Sim sim(40000);
    sim.runStart();
    sim.c.start(10000, sim.s.nowMs, true);
    assert(sim.c.ceilingKbps() == 40000 && sim.c.targetKbps() == 40000);
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) == 8);  // no new start settle time
}

// Plan deviation 6: few frames or few packets give no signal
static void testSmallWindows()
{
    Sim sim(40000);
    sim.runStart();
    sim.framesPerTick = 2;
    sim.packetsPerTick = 10;
    for (int i = 0; i < 40; i++) {
        assert(!sim.step(1, 10, 4).send);
    }
}

// Spec 4.5
static void testStatusStops()
{
    const uint16_t codes[] = {StatusNotSupported, StatusInputOnly, StatusInvalid};
    for (uint16_t status : codes) {
        Sim sim(40000);
        Decision d;
        assert(sim.stepUntilSend(d, 20) > 0);
        uint32_t id = sim.send(d);
        assert(sim.answer(id, 40000, status) == StatusResult::Stopped);
        assert(!sim.c.running());
        for (int i = 0; i < 40; i++) {
            assert(!sim.step(5).send);
        }
    }
}

// Spec 4.5
static void testEncoderFailed()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    uint32_t id = sim.send(d);
    // The restart failed: the host still runs 40000 (accepted) with encoder 32000
    assert(sim.c.onStatus(id, StatusEncoderFailed, 30000, 40000, 32000, sim.s.nowMs) == StatusResult::Accepted);
    assert(sim.c.targetKbps() == 40000 && sim.c.restartMode());
    for (int i = 0; i < 12; i++) {
        assert(!sim.step(1).send);  // RESTART_SETTLE_MS
    }
    // The rules start again after the settle time
    assert(sim.stepUntilSend(d, 40, 1) > 0);
    assert(d.reason == Reason::Loss && d.targetKbps == 30000);
}

// Spec 4.5: ENCODER_FAILED after a lower ceiling sends the ceiling again
static void testEncoderFailedLowerCeiling()
{
    Sim sim(40000);
    sim.runStart();
    Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
    assert(r.send && r.targetKbps == 20000);
    uint32_t id = sim.send(r);
    assert(sim.c.onStatus(id, StatusEncoderFailed, 20000, 40000, 32000, sim.s.nowMs) == StatusResult::Accepted);
    assert(sim.c.targetKbps() == 20000 && sim.c.ceilingKbps() == 20000);
    Decision d;
    assert(sim.stepUntilSend(d, 40) == 12);  // RESTART_SETTLE_MS
    assert(d.reason == Reason::Resend && d.fromKbps == 20000 && d.targetKbps == 20000);
    id = sim.send(d);
    assert(sim.answer(id, 20000) == StatusResult::Accepted);
    for (int i = 0; i < 40; i++) {
        assert(!sim.step().send);
    }
}

// Spec 4.5: the wait after ENCODER_FAILED doubles up to 60 s; an applied status resets it
static void testEncoderFailedBackoff()
{
    Sim sim(40000);
    sim.runStart();
    Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
    uint32_t id = sim.send(r);
    const int waits[] = {12, 24, 48, 96, 192, 240, 240};  // 3 s, 6 s, 12 s, 24 s, 48 s, 60 s, 60 s
    for (int wait : waits) {
        assert(sim.c.onStatus(id, StatusEncoderFailed, 20000, 40000, 32000, sim.s.nowMs) == StatusResult::Accepted);
        Decision d;
        assert(sim.stepUntilSend(d, 300) == wait);
        assert(d.reason == Reason::Resend && d.targetKbps == 20000);
        id = sim.send(d);
    }
    assert(sim.answer(id, 20000) == StatusResult::Accepted);

    // The wait starts again at 3 s
    r = sim.c.setCeiling(10000, sim.s.nowMs);
    assert(r.send && r.targetKbps == 10000);
    id = sim.send(r);
    assert(sim.c.onStatus(id, StatusEncoderFailed, 10000, 20000, 16000, sim.s.nowMs) == StatusResult::Accepted);
    Decision d;
    assert(sim.stepUntilSend(d, 300) == 12);
    assert(d.reason == Reason::Resend && d.targetKbps == 10000);
}

// Spec 3.5: after a timeout, the next free tick sends the target again
static void testTimeoutResend()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0 && d.targetKbps == 30000);
    uint32_t id = sim.send(d);
    Decision t;
    int steps = 0;
    do {
        t = sim.step();
        steps++;
    } while (!t.timedOut && steps < 20);
    assert(t.timedOut && !t.send && steps == 12);
    Decision r = sim.step();
    assert(r.send && r.reason == Reason::Resend && r.fromKbps == 30000 && r.targetKbps == 30000);
    uint32_t newId = sim.send(r);
    assert(sim.answer(id, 30000) == StatusResult::Ignored);  // a newer request is pending
    assert(sim.answer(newId, 30000) == StatusResult::Accepted);
}

// Spec 3.5: a late answer before the resend gives the host state; no resend
static void testLateAnswer()
{
    Sim sim(40000);
    sim.runStart();
    Decision d;
    assert(sim.stepUntilSend(d, 20, 1) > 0);
    uint32_t id = sim.send(d);
    Decision t;
    do {
        t = sim.step();
    } while (!t.timedOut);
    assert(sim.answer(id, d.targetKbps) == StatusResult::Accepted);
    assert(!sim.c.pending() && sim.c.encoderKbps() == 24000);
    for (int i = 0; i < 8; i++) {
        assert(!sim.step().send);
    }
}

// Spec 3.5 and D12: a timeout in non-adaptive mode also sends the target again
static void testTimeoutNotAdaptive()
{
    Sim sim(40000, false);
    for (int i = 0; i < 20; i++) {
        assert(!sim.step().send);
    }
    Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
    assert(r.send && r.targetKbps == 20000);
    sim.send(r);
    Decision t;
    int steps = 0;
    do {
        t = sim.step();
        steps++;
    } while (!t.timedOut && steps < 20);
    assert(t.timedOut && steps == 12);
    Decision d = sim.step();
    assert(d.send && d.reason == Reason::Resend && d.targetKbps == 20000);
    uint32_t id = sim.send(d);
    assert(sim.answer(id, 20000) == StatusResult::Accepted);
    for (int i = 0; i < 40; i++) {
        assert(!sim.step(5, 60, 10).send);
    }
}

// Spec 6.2: a request that the session could not send
static void testSendFailed()
{
    {
        // A new ceiling: the controller holds the new target and sends it again after the settle time
        Sim sim(40000);
        sim.runStart();
        Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
        assert(r.send && r.targetKbps == 20000);
        sim.c.sendFailed(r);
        assert(!sim.c.pending() && sim.c.targetKbps() == 20000);
        Decision d;
        assert(sim.stepUntilSend(d, 20) == 12);  // RESTART_SETTLE_MS
        assert(d.reason == Reason::Resend && d.fromKbps == 20000 && d.targetKbps == 20000);
        uint32_t id = sim.send(d);
        assert(sim.answer(id, 20000) == StatusResult::Accepted);
    }
    {
        // A rule decision: the target does not change and the rule decides again
        Sim sim(40000);
        sim.runStart();
        Decision d;
        assert(sim.stepUntilSend(d, 20, 1) > 0 && d.reason == Reason::Loss && d.targetKbps == 30000);
        sim.c.sendFailed(d);
        assert(sim.c.targetKbps() == 40000);
        Decision again = sim.step(1);
        assert(again.send && again.reason == Reason::Loss && again.targetKbps == 30000);
    }
    {
        // The start request: sent again at the next tick
        Sim sim(40000);
        Decision d;
        assert(sim.stepUntilSend(d, 40) > 0 && d.reason == Reason::Start);
        sim.c.sendFailed(d);
        Decision again = sim.step();
        assert(again.send && again.targetKbps == 40000);
    }
    {
        // A stopped controller does nothing
        Sim sim(40000);
        Decision d;
        assert(sim.stepUntilSend(d, 40) > 0);
        uint32_t id = sim.send(d);
        assert(sim.answer(id, 40000, StatusNotSupported) == StatusResult::Stopped);
        sim.c.sendFailed(d);
        assert(!sim.step().send);
    }
}

// stop(): no decision and no request after it, also not for a new ceiling
static void testStop()
{
    Sim sim(40000);
    sim.c.stop();
    assert(sim.c.started() && !sim.c.running());
    for (int i = 0; i < 40; i++) {
        assert(!sim.step(5, 60, 10).send);
    }
    assert(!sim.c.setCeiling(20000, sim.s.nowMs).send);
    sim.c.start(30000, sim.s.nowMs, true);  // a second start does nothing
    assert(!sim.c.running());
}

// Spec 4.5: an unknown status code stops the controller
static void testUnknownStatus()
{
    Sim sim(40000);
    Decision d;
    assert(sim.stepUntilSend(d, 20) > 0);
    uint32_t id = sim.send(d);
    assert(sim.c.onStatus(id, 7, 40000, 40000, 32000, sim.s.nowMs) == StatusResult::Stopped);
    assert(!sim.c.running() && strcmp(statusName(7), "UNKNOWN") == 0);
    for (int i = 0; i < 40; i++) {
        assert(!sim.step(5).send);
    }
}

// The RTT is never known: no delay signal, the loss rule still works
static void testRttZero()
{
    Sim sim(40000);
    sim.s.rttMs = 0;
    Decision d;
    assert(sim.stepUntilSend(d, 20, 0, 0) == 16 && d.reason == Reason::Start);
    sim.sendAndAnswer(d, StatusUnchanged);
    for (int i = 0; i < 80; i++) {
        assert(!sim.step(0, 0).send);
    }
    assert(sim.stepUntilSend(d, 20, 1, 0) == 6);  // full windows: 2 ticks with loss in each of 2 windows
    assert(d.reason == Reason::Loss && d.targetKbps == 30000 && d.rttMs == 0 && d.rttBaselineMs == 0);
}

// Spec D12: the setting is off
static void testNotAdaptive()
{
    Sim sim(40000, false);
    for (int i = 0; i < 80; i++) {
        assert(!sim.step(5, 60, 10).send);
    }
    assert(!sim.c.adaptive());
    Decision r = sim.c.setCeiling(20000, sim.s.nowMs);
    assert(r.send && r.targetKbps == 20000);
}

// Spec R12
static void testCounterWrap()
{
    Sim sim(40000);
    sim.s.framesFinished = 0xFFFFFFF0u;
    sim.s.packetsVideo = 0xFFFFFF00u;
    sim.runStart();
    for (int i = 0; i < 40; i++) {
        assert(!sim.step().send);
    }
}

// Spec 6.5 and 6.7
static void testTexts()
{
    Decision d;
    d.fromKbps = 60000;
    d.targetKbps = 45000;
    d.reason = Reason::Loss;
    d.lossPct[0] = 4.1;
    d.lossPct[1] = 3.2;
    d.rttMs = 38;
    d.rttBaselineMs = 22;
    d.fecPct = 1.2;
    d.measuredMbps = 52.3;
    char text[256];
    formatDecision(d, text, sizeof(text));
    assert(strcmp(text, "Adaptive bitrate: 60000 -> 45000 kbps, reason loss (loss 4.1% 3.2%, rtt 38/22 ms, fec 1.2%, measured 52.3 Mbps)") == 0);
    formatOverlay(true, 42000, 80000, 33400, 31.2, text, sizeof(text));
    assert(strcmp(text, "Bitrate: target 42.0 Mbps (limit 80.0), host encoder 33.4 Mbps, measured 31.2 Mbps\n") == 0);
    formatOverlay(false, 44000, 0, 0, 31.2, text, sizeof(text));
    assert(strcmp(text, "Bitrate: target 44.0 Mbps (fixed), host encoder N/A, measured 31.2 Mbps\n") == 0);
    assert(strcmp(statusName(StatusAppliedRestart), "APPLIED_RESTART") == 0);
    assert(strcmp(reasonName(Reason::FecPressure), "FEC") == 0);
    assert(strcmp(reasonName(Reason::Resend), "resend") == 0);
}

int main()
{
    testCleanNetwork();
    testStartSettle();
    testSustainedLoss();
    testIsolatedBurst();
    testStallAcrossWindows();
    testHeavyLossWithRttRise();
    testDelay();
    testFecPressure();
    testStaticStreamDelayNoDecrease();
    testBusyStreamDelayDecreases();
    testStaticStreamFecNoDecrease();
    testStaticStreamLossDecreases();
    testRecovery();
    testAppLimited();
    testBusyStreamIncreases();
    testStaticStreamNoIncrease();
    testMaxCut();
    testFloorAndCeiling();
    testDeadBand();
    testPendingRequest();
    testHostCap();
    testSetCeiling();
    testRestartMode();
    testStartRequest();
    testStartNoAnswer();
    testSecondStart();
    testSmallWindows();
    testStatusStops();
    testEncoderFailed();
    testEncoderFailedLowerCeiling();
    testEncoderFailedBackoff();
    testTimeoutResend();
    testLateAnswer();
    testTimeoutNotAdaptive();
    testSendFailed();
    testStop();
    testUnknownStatus();
    testRttZero();
    testNotAdaptive();
    testCounterWrap();
    testTexts();
    puts("adaptivebitrate_test: all checks passed");
    return 0;
}

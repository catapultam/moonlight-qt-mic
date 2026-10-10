#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// The network adaptive bitrate controller (spec section 4). It has no SDL, Qt or
// moonlight-common-c dependency, so app/tests/adaptivebitrate_test.cpp can build it
// alone. The session calls it on the main thread with the samples and the time, and
// sends the requests that it decides.
namespace AdaptiveBitrate {

// Status codes of BITRATE_STATUS. The values match LI_BITRATE_STATUS_* in Limelight.h.
enum Status : uint16_t {
    StatusApplied = 0,
    StatusAppliedRestart = 1,
    StatusUnchanged = 2,
    StatusNotSupported = 3,
    StatusInvalid = 4,
    StatusEncoderFailed = 5,
    StatusInputOnly = 6,
};

inline const char* statusName(uint16_t status)
{
    switch (status) {
    case StatusApplied: return "APPLIED";
    case StatusAppliedRestart: return "APPLIED_RESTART";
    case StatusUnchanged: return "UNCHANGED";
    case StatusNotSupported: return "NOT_SUPPORTED";
    case StatusInvalid: return "INVALID";
    case StatusEncoderFailed: return "ENCODER_FAILED";
    case StatusInputOnly: return "INPUT_ONLY";
    default: return "UNKNOWN";
    }
}

// Values of spec section 4.3. "Plan" marks a value that the implementation plan added.
constexpr uint32_t TICK_MS = 250;
constexpr uint32_t START_SETTLE_MS = 4000;
constexpr uint32_t CHANGE_SETTLE_MS = 1000;
constexpr uint32_t RESTART_SETTLE_MS = 3000;
constexpr uint32_t FLOOR_KBPS = 1500;              // the floor is min(FLOOR_KBPS, ceiling)
constexpr uint32_t MIN_CEILING_KBPS = 500;         // the host answers INVALID below this value (spec 5.2, 6.1)
constexpr double LOSS_WINDOW_PCT = 2;              // a 1 s window with this frame loss is "lossy"
constexpr double LOSS_HEAVY_PCT = 10;
constexpr uint32_t LOSS_MIN_FRAMES = 10;           // plan: fewer frames in a window give no loss signal
constexpr uint32_t LOSS_MIN_TICKS = 3;             // plan: sustained loss needs this many ticks with loss in 3 s
constexpr uint32_t FEC_RECOVERED_PCT = 3;          // recovered / video packets over 2 s
constexpr uint32_t FEC_MIN_PACKETS = 200;          // plan: fewer packets in 2 s give no FEC signal
constexpr uint32_t RTT_RISE_MIN_MS = 15;           // delay: RTT > baseline + max(15, baseline / 2)
constexpr uint32_t RTT_BASELINE_MS = 30000;        // the baseline is the lowest RTT in this time
constexpr uint32_t RTT_DELAY_SAMPLES = 3;          // plan: samples of a window above the threshold for "delay"
constexpr double DEC_LOSS = 0.75;
constexpr double DEC_DELAY = 0.90;
constexpr double MEASURED_MARGIN = 0.9;            // rule 2: measured bitrate x 0.9
constexpr double MAX_CUT = 0.5;                    // plan: one decrease keeps at least half
constexpr uint32_t INC_STEP_PCT = 8;
constexpr uint32_t INC_STEP_NEAR_PCT = 3;
constexpr uint32_t STABLE_MS = 4000;
constexpr uint32_t NEAR_FAILURE_PCT = 85;
constexpr uint32_t FAILURE_MEMORY_MS = 60000;
constexpr uint32_t APP_LIMITED_PCT = 70;
constexpr uint32_t DEAD_BAND_PCT = 3;
constexpr uint32_t DEAD_BAND_MIN_KBPS = 250;       // also the smallest increase step (plan)
constexpr uint32_t MIN_DEC_INTERVAL_MS = 1000;
constexpr uint32_t MIN_DEC_INTERVAL_RESTART_MS = 3000;
constexpr uint32_t MIN_INC_INTERVAL_MS = 4000;
constexpr uint32_t MIN_INC_INTERVAL_RESTART_MS = 15000;
constexpr uint32_t REQUEST_TIMEOUT_MS = 3000;
constexpr uint32_t START_ATTEMPTS = 3;             // plan: start requests without an answer before the stop
constexpr uint32_t ENCODER_FAILED_MAX_WAIT_MS = 60000; // the wait after ENCODER_FAILED doubles up to this value
constexpr size_t WINDOW_TICKS = 4;                 // a 1 s window
constexpr size_t WINDOWS = 3;
constexpr size_t HISTORY = WINDOW_TICKS * WINDOWS; // plan: 3 s of deltas
constexpr size_t FEC_TICKS = 8;                    // a 2 s window
constexpr size_t RTT_BUCKETS = RTT_BASELINE_MS / 1000;

// Cumulative values at one tick
struct Sample {
    uint64_t nowMs = 0;
    uint32_t framesFinished = 0;       // LiGetVideoFrameCounters()
    uint32_t framesLost = 0;
    uint32_t packetsVideo = 0;         // RTP_VIDEO_STATS.packetCountVideo
    uint32_t packetsFecRecovered = 0;  // RTP_VIDEO_STATS.packetCountFecRecovered
    uint32_t rttMs = 0;                // 0 when not known
    double measuredMbps = 0;           // received video payload without FEC, last 2.5 s
};

enum class Reason {
    None,
    Start,        // the first request after START_SETTLE_MS (spec 4.4)
    Loss,         // rule 2
    Delay,        // rule 3
    FecPressure,  // rule 3
    Increase,     // rule 5
    Ceiling,      // a new ceiling after a live resize (spec 4.6)
    Resend,       // the same target again after a timeout, a failed change or a failed send (spec 3.5, 4.5, 6.2)
};

inline const char* reasonName(Reason reason)
{
    switch (reason) {
    case Reason::Start: return "start";
    case Reason::Loss: return "loss";
    case Reason::Delay: return "delay";
    case Reason::FecPressure: return "FEC";
    case Reason::Increase: return "increase";
    case Reason::Ceiling: return "new limit";
    case Reason::Resend: return "resend";
    default: return "none";
    }
}

struct Decision {
    bool send = false;              // true: send SET_BITRATE with targetKbps
    uint32_t targetKbps = 0;        // configured kbps
    uint32_t fromKbps = 0;
    Reason reason = Reason::None;
    bool isolated = false;          // rule 4: an isolated burst, no change (D8)
    bool rebased = false;           // the RTT baseline is now the current RTT
    bool timedOut = false;          // the pending request got no answer in REQUEST_TIMEOUT_MS
    uint32_t timedOutRequestId = 0;
    bool stopped = false;           // the start request got no answer START_ATTEMPTS times
    // Signals for the log line
    double lossPct[2] = {0, 0};     // the newest two 1 s windows
    uint32_t rttMs = 0;             // mean RTT of the newest window
    uint32_t rttBaselineMs = 0;
    double fecPct = 0;
    double measuredMbps = 0;
};

enum class StatusResult {
    Ignored,   // an old or repeated answer
    Accepted,
    Stopped,   // NOT_SUPPORTED, INPUT_ONLY, INVALID or an unknown code: the controller stops for the session
};

// Rule 6: a change smaller than max(3 %, 250 kbps) is not sent, except a change to
// the floor or the ceiling.
inline bool passesDeadBand(uint32_t fromKbps, uint32_t toKbps, uint32_t floorKbps, uint32_t ceilingKbps)
{
    if (toKbps == fromKbps) {
        return false;
    }
    if (toKbps == floorKbps || toKbps == ceilingKbps) {
        return true;
    }
    uint32_t diff = toKbps > fromKbps ? toKbps - fromKbps : fromKbps - toKbps;
    uint32_t band = (uint32_t)((uint64_t)fromKbps * DEAD_BAND_PCT / 100);
    if (band < DEAD_BAND_MIN_KBPS) {
        band = DEAD_BAND_MIN_KBPS;
    }
    return diff >= band;
}

// The log line of a decision that sends a request (spec 6.7)
inline int formatDecision(const Decision& d, char* buffer, size_t length)
{
    return snprintf(buffer, length,
                    "Adaptive bitrate: %u -> %u kbps, reason %s (loss %.1f%% %.1f%%, rtt %u/%u ms, fec %.1f%%, measured %.1f Mbps)",
                    d.fromKbps, d.targetKbps, reasonName(d.reason), d.lossPct[0], d.lossPct[1],
                    d.rttMs, d.rttBaselineMs, d.fecPct, d.measuredMbps);
}

// The line of the stats overlay (spec 6.5). running == false shows "fixed".
inline int formatOverlay(bool running, uint32_t targetKbps, uint32_t ceilingKbps, uint32_t encoderKbps,
                         double measuredMbps, char* buffer, size_t length)
{
    char encoder[48];
    if (encoderKbps != 0) {
        snprintf(encoder, sizeof(encoder), "%.1f Mbps", encoderKbps / 1000.0);
    }
    else {
        snprintf(encoder, sizeof(encoder), "N/A");
    }
    if (running) {
        return snprintf(buffer, length, "Bitrate: target %.1f Mbps (limit %.1f), host encoder %s, measured %.1f Mbps\n",
                        targetKbps / 1000.0, ceilingKbps / 1000.0, encoder, measuredMbps);
    }
    return snprintf(buffer, length, "Bitrate: target %.1f Mbps (fixed), host encoder %s, measured %.1f Mbps\n",
                    targetKbps / 1000.0, encoder, measuredMbps);
}

class Controller {
public:
    // Starts the controller at the first decoded frame of the session. A second
    // call does nothing (spec 4.4). adaptive == false: no rule runs. Only setCeiling(),
    // a resend after a timeout and a resend of the ceiling after ENCODER_FAILED
    // make requests (spec D12).
    void start(uint32_t ceilingKbps, uint64_t nowMs, bool adaptive)
    {
        if (m_Started) {
            return;
        }
        m_Started = true;
        m_Adaptive = adaptive;
        m_StartPhase = adaptive;
        m_Ceiling = ceilingKbps;
        m_Target = ceilingKbps;
        m_MinTarget = ceilingKbps;
        m_MaxTarget = ceilingKbps;
        notifySettle(nowMs, START_SETTLE_MS);
    }

    // One tick (TICK_MS). Returns the decision; the caller sends it when send is true
    // and then calls sent().
    Decision tick(const Sample& sample)
    {
        Decision d;
        if (!running()) {
            return d;
        }
        const uint64_t now = sample.nowMs;
        d.fromKbps = m_Target;
        d.targetKbps = m_Target;
        d.measuredMbps = sample.measuredMbps;

        // Deltas of the cumulative counters. Unsigned subtraction handles one wrap (R12).
        Delta delta;
        const bool haveDelta = m_HaveLast;
        if (haveDelta) {
            delta.frames = sample.framesFinished - m_Last.framesFinished;
            delta.lost = sample.framesLost - m_Last.framesLost;
            delta.packets = sample.packetsVideo - m_Last.packetsVideo;
            delta.recovered = sample.packetsFecRecovered - m_Last.packetsFecRecovered;
            delta.rttMs = sample.rttMs;
        }
        m_Last = sample;
        m_HaveLast = true;
        addRtt(now, sample.rttMs);
        m_TargetSum += m_Target;
        m_TargetTicks++;

        // Spec 3.5: 3 s without an answer ends the pending request
        if (m_Pending && now - m_PendingSinceMs >= REQUEST_TIMEOUT_MS) {
            d.timedOut = true;
            d.timedOutRequestId = m_PendingId;
            m_Pending = false;
            if (m_StartPhase && ++m_StartAttempts >= START_ATTEMPTS) {
                m_Stopped = true;
                d.stopped = true;
                return d;
            }
            // The host can run another value now (its watchdog and re-apply, spec 5.3).
            // A later free tick sends the target again, but a late answer comes first.
            if (!m_StartPhase) {
                m_Resend = true;
            }
            return d;
        }

        // Rule 1: a pending request or a settle time. The deltas of this time do
        // not count (an IDR frame or a full queue is not a network signal).
        if (m_Pending || now < m_SettleUntilMs) {
            clearHistory(now);
            return d;
        }

        // Spec 3.5 and 4.5: send the target again. No dead band and no interval:
        // the value does not change.
        if (m_Resend) {
            d.send = true;
            d.reason = Reason::Resend;
            return d;
        }
        if (!m_Adaptive) {
            return d;
        }

        // Spec 4.4: the first request sends the ceiling, also when it equals the
        // start bitrate, so that the host answers with its real values
        if (m_StartPhase) {
            d.send = true;
            d.targetKbps = m_Ceiling;
            d.reason = Reason::Start;
            return d;
        }

        if (!haveDelta) {
            return d;
        }
        push(delta);

        const uint32_t baseline = rttBaseline(now);
        const uint32_t rise = baseline / 2 > RTT_RISE_MIN_MS ? baseline / 2 : RTT_RISE_MIN_MS;
        Window w[WINDOWS];
        int lossyWindows = 0;
        for (size_t i = 0; i < WINDOWS; i++) {
            w[i] = window(i, baseline, rise);
            if (w[i].lossy) {
                lossyWindows++;
            }
        }
        uint32_t lossyTicks = 0;
        for (size_t k = 0; k < m_Count; k++) {
            if (at(k).lost > 0) {
                lossyTicks++;
            }
        }

        bool fecPressure = false;
        if (m_Count >= FEC_TICKS) {
            uint32_t packets = 0, recovered = 0;
            for (size_t k = 0; k < FEC_TICKS; k++) {
                packets += at(k).packets;
                recovered += at(k).recovered;
            }
            if (packets > 0) {
                d.fecPct = 100.0 * recovered / packets;
            }
            fecPressure = packets >= FEC_MIN_PACKETS && (uint64_t)recovered * 100 >= (uint64_t)FEC_RECOVERED_PCT * packets;
        }

        d.lossPct[0] = w[0].lossPct;
        d.lossPct[1] = w[1].lossPct;
        d.rttMs = w[0].rttMeanMs;
        d.rttBaselineMs = baseline;

        if (w[0].lossy || w[0].delay || fecPressure) {
            m_LastBadMs = now;
        }

        const uint32_t floor = floorKbps();
        const bool sustainedLoss = (lossyWindows >= 2 && lossyTicks >= LOSS_MIN_TICKS) || (w[0].heavy && w[0].delay);
        const bool sustainedDelay = w[0].delay && w[1].delay;
        uint32_t newKbps = m_Target;
        Reason reason = Reason::None;

        if (sustainedLoss) {
            // Rule 2
            double cut = m_Target * DEC_LOSS;
            if (m_EncoderKbps > 0 && m_AcceptedKbps > 0 && sample.measuredMbps > 0) {
                double measured = sample.measuredMbps * 1000.0 * m_AcceptedKbps / m_EncoderKbps * MEASURED_MARGIN;
                if (measured < cut) {
                    cut = measured;
                }
            }
            if (cut < m_Target * MAX_CUT) {
                cut = m_Target * MAX_CUT;
            }
            newKbps = cut > floor ? (uint32_t)cut : floor;
            reason = Reason::Loss;
            noteFailure(now);
        }
        else if (sustainedDelay || fecPressure) {
            // Rule 3. A second delay decision while the RTT did not fall after the last
            // delay decrease: the added delay is not a queue that a lower bitrate drains.
            if (sustainedDelay && !fecPressure && m_HaveDelayCut && now - m_LastDelayCutMs < RTT_BASELINE_MS &&
                    w[0].rttMeanMs + rise / 2 >= m_LastDelayCutRttMs) {
                resetRtt(now, w[0].rttMeanMs);
                m_HaveDelayCut = false;
                d.rebased = true;
                d.rttBaselineMs = w[0].rttMeanMs;
                return d;
            }
            double cut = m_Target * DEC_DELAY;
            newKbps = cut > floor ? (uint32_t)cut : floor;
            reason = sustainedDelay ? Reason::Delay : Reason::FecPressure;
            noteFailure(now);
        }
        else if (lossyWindows > 0 || w[0].delay) {
            // Rule 4 (D8)
            d.isolated = true;
            return d;
        }
        else {
            // Rule 5
            const uint64_t cleanSince = m_LastBadMs > m_CleanSinceMs ? m_LastBadMs : m_CleanSinceMs;
            const bool stable = now - cleanSince >= STABLE_MS;
            const bool notAppLimited = m_EncoderKbps > 0 &&
                    sample.measuredMbps * 1000.0 * 100 >= (double)APP_LIMITED_PCT * m_EncoderKbps;
            if (m_Target < m_Ceiling && stable && notAppLimited) {
                const bool nearFailure = m_HaveFailure && now - m_LastFailureMs < FAILURE_MEMORY_MS &&
                        (uint64_t)m_Target * 100 >= (uint64_t)NEAR_FAILURE_PCT * m_LastFailureKbps;
                uint32_t pct = nearFailure ? INC_STEP_NEAR_PCT : INC_STEP_PCT;
                if (m_RestartMode) {
                    pct *= 2;
                }
                uint32_t step = (uint32_t)((uint64_t)m_Target * pct / 100);
                if (step < DEAD_BAND_MIN_KBPS) {
                    step = DEAD_BAND_MIN_KBPS;
                }
                newKbps = m_Ceiling - m_Target < step ? m_Ceiling : m_Target + step;
                reason = Reason::Increase;
            }
        }

        // Rule 6
        if (newKbps == m_Target) {
            return d;
        }
        if (newKbps < m_Target) {
            const uint32_t interval = m_RestartMode ? MIN_DEC_INTERVAL_RESTART_MS : MIN_DEC_INTERVAL_MS;
            if (m_HaveDecrease && now - m_LastDecreaseMs < interval) {
                return d;
            }
        }
        else {
            const uint32_t interval = m_RestartMode ? MIN_INC_INTERVAL_RESTART_MS : MIN_INC_INTERVAL_MS;
            if (m_HaveIncrease && now - m_LastIncreaseMs < interval) {
                return d;
            }
        }
        if (!passesDeadBand(m_Target, newKbps, floor, m_Ceiling)) {
            return d;
        }
        d.send = true;
        d.targetKbps = newKbps;
        d.reason = reason;
        return d;
    }

    // The caller sent the request of decision with this id
    void sent(const Decision& decision, uint32_t requestId, uint64_t nowMs)
    {
        if (!running()) {
            return;
        }
        m_Pending = true;
        m_PendingId = requestId;
        m_PendingSinceMs = nowMs;
        m_LastSentId = requestId;
        m_LastSentHandled = false;
        m_Resend = false;
        switch (decision.reason) {
        case Reason::Loss:
        case Reason::FecPressure:
            m_HaveDecrease = true;
            m_LastDecreaseMs = nowMs;
            break;
        case Reason::Delay:
            m_HaveDecrease = true;
            m_LastDecreaseMs = nowMs;
            m_HaveDelayCut = true;
            m_LastDelayCutMs = nowMs;
            m_LastDelayCutRttMs = decision.rttMs;
            break;
        case Reason::Increase:
            m_HaveIncrease = true;
            m_LastIncreaseMs = nowMs;
            break;
        default:
            break;
        }
        setTarget(decision.targetKbps);
        clearHistory(nowMs);
    }

    // The caller could not send the request of decision (spec 6.2). A target that the
    // controller already holds (start, resend, new ceiling) goes again at the next free
    // tick (reason resend). A rule decision did not change the target: the rules decide again.
    void sendFailed(const Decision& decision)
    {
        if (running() && decision.send && decision.targetKbps == m_Target) {
            m_Resend = true;
        }
    }

    // A BITRATE_STATUS answer (spec 3.5 and 4.5)
    StatusResult onStatus(uint32_t requestId, uint16_t status, uint32_t requestedKbps, uint32_t acceptedKbps,
                          uint32_t encoderKbps, uint64_t nowMs)
    {
        // Only the answer for the newest request counts, and only one time. After a
        // timeout and before a new request, the late answer gives the host state.
        if (!running() || m_LastSentId == 0 || requestId != m_LastSentId || m_LastSentHandled) {
            return StatusResult::Ignored;
        }
        m_LastSentHandled = true;
        m_Pending = false;
        m_StartPhase = false;
        m_Resend = false;

        switch (status) {
        case StatusApplied:
        case StatusAppliedRestart:
        case StatusUnchanged:
            // R13: the host can only lower the value
            if (acceptedKbps > requestedKbps) {
                acceptedKbps = requestedKbps;
            }
            m_AcceptedKbps = acceptedKbps;
            m_EncoderKbps = encoderKbps;
            m_EncoderFailures = 0;
            if (acceptedKbps > 0 && acceptedKbps < requestedKbps) {
                // The host cap (config::video.max_bitrate) is the ceiling for the session
                m_HostCapKbps = acceptedKbps;
                if (m_Ceiling > acceptedKbps) {
                    m_Ceiling = acceptedKbps;
                }
                if (m_Target > m_Ceiling) {
                    setTarget(m_Ceiling);
                }
            }
            if (status == StatusAppliedRestart) {
                m_RestartMode = true;
                notifySettle(nowMs, RESTART_SETTLE_MS);
            }
            else if (status == StatusApplied) {
                notifySettle(nowMs, CHANGE_SETTLE_MS);
            }
            return StatusResult::Accepted;
        case StatusEncoderFailed: {
            // The host runs the old values; the target goes back to them, but not
            // above the ceiling. A running value above the ceiling (a lower ceiling
            // after a live resize) gets the ceiling again after the wait.
            m_AcceptedKbps = acceptedKbps;
            m_EncoderKbps = encoderKbps;
            m_RestartMode = true;
            if (acceptedKbps > 0) {
                setTarget(acceptedKbps < m_Ceiling ? acceptedKbps : m_Ceiling);
            }
            m_Resend = acceptedKbps > m_Ceiling;
            // The wait doubles for each failure in a row: 3 s, 6 s, 12 s ... 60 s
            uint64_t wait = (uint64_t)RESTART_SETTLE_MS << (m_EncoderFailures < 5 ? m_EncoderFailures : 5);
            if (wait > ENCODER_FAILED_MAX_WAIT_MS) {
                wait = ENCODER_FAILED_MAX_WAIT_MS;
            }
            m_EncoderFailures++;
            notifySettle(nowMs, (uint32_t)wait);
            return StatusResult::Accepted;
        }
        default:
            // NOT_SUPPORTED, INPUT_ONLY, INVALID or an unknown code
            m_Stopped = true;
            return StatusResult::Stopped;
        }
    }

    // A new ceiling after a live resize (spec 4.6). The returned decision bypasses
    // rule 1 and the intervals; the caller sends it at once when send is true.
    Decision setCeiling(uint32_t ceilingKbps, uint64_t nowMs)
    {
        Decision d;
        if (!running()) {
            return d;
        }
        if (m_HostCapKbps > 0 && ceilingKbps > m_HostCapKbps) {
            ceilingKbps = m_HostCapKbps;
        }
        const uint32_t old = m_Target;
        if (!m_Adaptive || m_Target >= m_Ceiling) {
            // Not limited by the network: same as a new stream at the configured speed (D9)
            setTarget(ceilingKbps);
        }
        else if (m_Target > ceilingKbps) {
            // The network capacity does not depend on the stream size
            setTarget(ceilingKbps);
        }
        m_Ceiling = ceilingKbps;
        notifySettle(nowMs, RESTART_SETTLE_MS);
        d.fromKbps = old;
        d.targetKbps = m_Target;
        if (m_Target != old) {
            d.send = true;
            d.reason = Reason::Ceiling;
        }
        return d;
    }

    // Stops the controller for the session, for example when the session cannot
    // run the tick timer. No decision and no request after this call.
    void stop()
    {
        m_Stopped = true;
    }

    // No decision until nowMs + durationMs; the windows start again
    void notifySettle(uint64_t nowMs, uint32_t durationMs)
    {
        if (nowMs + durationMs > m_SettleUntilMs) {
            m_SettleUntilMs = nowMs + durationMs;
        }
        clearHistory(nowMs);
    }

    bool started() const { return m_Started; }
    bool running() const { return m_Started && !m_Stopped; }
    bool adaptive() const { return m_Adaptive; }
    bool pending() const { return m_Pending; }
    bool restartMode() const { return m_RestartMode; }
    uint32_t targetKbps() const { return m_Target; }
    uint32_t ceilingKbps() const { return m_Ceiling; }
    uint32_t encoderKbps() const { return m_EncoderKbps; }
    uint32_t floorKbps() const { return m_Ceiling < FLOOR_KBPS ? m_Ceiling : FLOOR_KBPS; }
    uint32_t minTargetKbps() const { return m_MinTarget; }
    uint32_t maxTargetKbps() const { return m_MaxTarget; }
    uint32_t meanTargetKbps() const { return m_TargetTicks ? (uint32_t)(m_TargetSum / m_TargetTicks) : m_Target; }

private:
    struct Delta {
        uint32_t frames = 0;
        uint32_t lost = 0;
        uint32_t packets = 0;
        uint32_t recovered = 0;
        uint32_t rttMs = 0;
    };

    struct Window {
        double lossPct = 0;
        bool lossy = false;
        bool heavy = false;
        bool delay = false;
        uint32_t rttMeanMs = 0;
    };

    struct RttBucket {
        uint64_t second = 0;  // nowMs / 1000 + 1; 0 = empty
        uint32_t minRtt = 0;
    };

    void setTarget(uint32_t kbps)
    {
        m_Target = kbps;
        if (kbps < m_MinTarget) {
            m_MinTarget = kbps;
        }
        if (kbps > m_MaxTarget) {
            m_MaxTarget = kbps;
        }
    }

    void clearHistory(uint64_t nowMs)
    {
        m_Count = 0;
        m_CleanSinceMs = nowMs;
    }

    void push(const Delta& delta)
    {
        m_Head = (m_Head + 1) % HISTORY;
        m_History[m_Head] = delta;
        if (m_Count < HISTORY) {
            m_Count++;
        }
    }

    // k = 0 is the newest delta
    const Delta& at(size_t k) const
    {
        return m_History[(m_Head + HISTORY - k) % HISTORY];
    }

    // Window index 0 is the newest 1 s. A window without 4 deltas gives no signal.
    Window window(size_t index, uint32_t baseline, uint32_t rise) const
    {
        Window w;
        if (m_Count < (index + 1) * WINDOW_TICKS) {
            return w;
        }
        uint32_t frames = 0, lost = 0, delayed = 0, rttSum = 0, rttCount = 0;
        for (size_t k = index * WINDOW_TICKS; k < (index + 1) * WINDOW_TICKS; k++) {
            const Delta& x = at(k);
            frames += x.frames;
            lost += x.lost;
            if (x.rttMs != 0) {
                rttSum += x.rttMs;
                rttCount++;
                if (baseline != 0 && x.rttMs > baseline + rise) {
                    delayed++;
                }
            }
        }
        if (lost > frames) {
            lost = frames;
        }
        if (frames > 0) {
            w.lossPct = 100.0 * lost / frames;
        }
        w.lossy = frames >= LOSS_MIN_FRAMES && w.lossPct >= LOSS_WINDOW_PCT;
        w.heavy = frames >= LOSS_MIN_FRAMES && w.lossPct >= LOSS_HEAVY_PCT;
        w.delay = delayed >= RTT_DELAY_SAMPLES;
        w.rttMeanMs = rttCount ? rttSum / rttCount : 0;
        return w;
    }

    void addRtt(uint64_t nowMs, uint32_t rttMs)
    {
        if (rttMs == 0) {
            return;
        }
        const uint64_t second = nowMs / 1000 + 1;
        RttBucket& bucket = m_Rtt[second % RTT_BUCKETS];
        if (bucket.second != second) {
            bucket.second = second;
            bucket.minRtt = rttMs;
        }
        else if (rttMs < bucket.minRtt) {
            bucket.minRtt = rttMs;
        }
    }

    // The lowest RTT of the last RTT_BASELINE_MS, 0 when not known
    uint32_t rttBaseline(uint64_t nowMs) const
    {
        const uint64_t second = nowMs / 1000 + 1;
        uint32_t best = 0;
        for (const RttBucket& bucket : m_Rtt) {
            if (bucket.second != 0 && bucket.second + RTT_BUCKETS > second && (best == 0 || bucket.minRtt < best)) {
                best = bucket.minRtt;
            }
        }
        return best;
    }

    void resetRtt(uint64_t nowMs, uint32_t rttMs)
    {
        for (RttBucket& bucket : m_Rtt) {
            bucket = RttBucket();
        }
        addRtt(nowMs, rttMs);
    }

    void noteFailure(uint64_t nowMs)
    {
        m_HaveFailure = true;
        m_LastFailureKbps = m_Target;
        m_LastFailureMs = nowMs;
    }

    bool m_Started = false;
    bool m_Stopped = false;
    bool m_Adaptive = false;
    bool m_StartPhase = false;
    uint32_t m_StartAttempts = 0;
    uint32_t m_Ceiling = 0;
    uint32_t m_Target = 0;
    uint32_t m_HostCapKbps = 0;
    uint32_t m_AcceptedKbps = 0;
    uint32_t m_EncoderKbps = 0;
    bool m_RestartMode = false;
    bool m_Resend = false;
    uint32_t m_EncoderFailures = 0;
    uint64_t m_SettleUntilMs = 0;
    uint64_t m_CleanSinceMs = 0;
    uint64_t m_LastBadMs = 0;

    bool m_Pending = false;
    uint32_t m_PendingId = 0;
    uint64_t m_PendingSinceMs = 0;
    uint32_t m_LastSentId = 0;
    bool m_LastSentHandled = false;

    bool m_HaveDecrease = false;
    uint64_t m_LastDecreaseMs = 0;
    bool m_HaveIncrease = false;
    uint64_t m_LastIncreaseMs = 0;
    bool m_HaveFailure = false;
    uint32_t m_LastFailureKbps = 0;
    uint64_t m_LastFailureMs = 0;
    bool m_HaveDelayCut = false;
    uint64_t m_LastDelayCutMs = 0;
    uint32_t m_LastDelayCutRttMs = 0;

    Sample m_Last;
    bool m_HaveLast = false;
    Delta m_History[HISTORY];
    size_t m_Head = 0;
    size_t m_Count = 0;
    RttBucket m_Rtt[RTT_BUCKETS];

    uint32_t m_MinTarget = 0;
    uint32_t m_MaxTarget = 0;
    uint64_t m_TargetSum = 0;
    uint64_t m_TargetTicks = 0;
};

} // namespace AdaptiveBitrate

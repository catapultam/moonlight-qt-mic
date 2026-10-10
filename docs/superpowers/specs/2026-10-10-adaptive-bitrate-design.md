# Adaptive bitrate: design

Date: 2026-10-10
Status: design. No code yet.

Repositories (read-only during the design):

- Client: `~/GitHub/moonlight-qt-mic-lr` (branch with live resize), submodule
  `moonlight-common-c/moonlight-common-c` at `8aa512c`.
- Host: `~/GitHub/apollo-microphone`, branch `live-resize` at `c67f621c`.

Line numbers are from 2026-10-09. Other work changes these files now (live
resize automation), so the numbers can move. Search for the named symbol.

Related: the live resize design
(`docs/superpowers/specs/2026-10-09-live-resize-design.md`). This feature uses
the same protocol style, mail rules and thread rules. Read sections 3, 4.2 and
4.6 of that spec first.

## 1. Goal

The stream bitrate follows two limits at all times:

- (a) The target. This is the bitrate that is correct for the current stream
  size, frame rate and chroma format. The client calculates it again after a
  live resize.
- (b) The available bandwidth. The stream starts at the target. Then the
  client lowers the bitrate when the network shows congestion, and raises it
  again when the network is stable, up to the target.

The host changes the bitrate of the running encoder. With NVENC (the CPLT-4A
host has an RTX 4090), the change has no IDR frame and no encoder restart. With
the other encoders (AMF, Quick Sync, software), the host restarts the encoder
at the new bitrate. This is one feature. All parts ship together.

## 2. Decisions

| # | Decision | Reason |
|---|----------|--------|
| D1 | The client runs the controller. The host only applies a bitrate that the client asks for. | All the network signals are on the client: frame loss (`ControlStream.c:506-545`), FEC recovery (`LiGetRTPVideoStats`, `VideoStream.c:417`), RTT (`LiGetEstimatedRttInfo`, `ControlStream.c:1803`) and the received byte rate (`BandwidthTracker`, `app/streaming/bandwidth.h`). The host does not read the client loss or RTT today (it only logs `IDX_LOSS_STATS`, `src/stream.cpp:1230`). |
| D2 | New control messages `SET_BITRATE` (`0x3102`, client to host) and `BITRATE_STATUS` (`0x3103`, host to client) on the encrypted control stream. | Same path as live resize (`0x3100`/`0x3101`). No new socket. |
| D3 | The value on the wire is the "configured" bitrate in kbps. This is the same unit as the SDP attribute `x-ml-video.configuredBitrateKbps` (`SdpGenerator.c:382-383`). The host converts it to the encoder bitrate with the same chain that it uses at stream start (`src/rtsp.cpp:1066-1154`). One pure function holds the chain, and both paths call it. | Sunshine ignores the 0.8 factor of the client when `configuredBitrateKbps` is not zero (`rtsp.cpp:1066-1070` uses it, `rtsp.cpp:1131-1154` overwrites `config.monitor.bitrate`). If the client sent an encoder value, the host would reduce it a second time. One function stops the two paths from drifting apart. |
| D4 | The host answers every request with `BITRATE_STATUS`. | No frame tells the client that the bitrate changed (live resize uses the new frame size as the answer; a bitrate change has no such sign). The answer also gives the clamped value and the encoder value. |
| D5 | The host advertises support with `a=x-ss-general.dynamicBitrate:1` in the RTSP DESCRIBE reply. | Same method as `a=x-ss-general.liveResize:1` (`src/rtsp.cpp:792-799`). A stock host drops unknown control messages. The client must not run the controller then. |
| D6 | NVENC: `nvEncReconfigureEncoder` with the saved init parameters, new `averageBitRate` and `vbvBufferSize`, `resetEncoder = 0`, `forceIDR = 0`. Other encoders: `encode_run` returns and `capture_async` makes a new encoder at the new bitrate. | NVENC supports a parameter change in a running session except GOP, sync or async mode, maximum size and PTD mode (`nvEncodeAPI.h` SDK 12.1 lines 4130-4142). FFmpeg AMF and Quick Sync read the bitrate only when the encoder opens. The restart path reuses the existing reinit loop of `capture_async` (`src/video.cpp:2398-2463`). |
| D7 | The controller is a pure function with the time as input. It uses decrease on sustained congestion, slow increase when stable, a floor, a ceiling, a minimum interval between changes and a dead band. | A pure function is testable without a network. The rules are in section 4. |
| D8 | An isolated loss or RTT burst (one window only) does not lower the bitrate. | A Wi-Fi stall of about 120 ms every 10 s caused loss bursts at a remote site with 230 Mbit/s of free bandwidth (memory note `moonlight-remote-stream-loss.md`, 2026-09-05). A lower bitrate does not fix such a stall. Capacity loss shows in more than one window or with an RTT rise. |
| D9 | The stream starts at the target. No probe at start. | The user asked for this. The SDP already starts the encoder at the configured bitrate. |
| D10 | Target (a), with `autoAdjustBitrate` on: `getDefaultBitrate(w, h, fps, yuv444)` (`app/settings/streamingpreferences.cpp:645`) for the current stream size and the negotiated chroma format. No new codec or HDR factor. | Same formula that sets the default slider value today. The formula has no codec or HDR term now (`streamingpreferences.cpp:645-711`). A new term would change the meaning of the slider. |
| D11 | Target (a), with a manual bitrate (`autoAdjustBitrate` off): the manual value is the ceiling. The controller lowers from it and goes back up to it. The manual value does not change on a live resize. | The user set this number. The adaptation only protects the stream from congestion. |
| D12 | One new setting: "Adapt bitrate to the network" (`adaptiveBitrate`, default on). It is not the same as `autoAdjustBitrate`. That flag has no check box: a slider move sets it to false and the reset button sets it to true (`SettingsView.qml:706-710, 722-726`). It only says whether the slider value is the default. When off, the stream keeps the start bitrate as today. A live resize then still sends one request with the new target when `autoAdjustBitrate` is on. | The minimum control. Users need a way to measure or stream at a fixed rate. |
| D13 | Live resize no longer keeps the bitrate. This replaces the bitrate part of live resize D4. After the client applies the new stream size, it calculates the target again and sends `SET_BITRATE`. | User request. |
| D14 | Each session has its own bitrate. No refusal with more than one client. | Each session has its own `capture_async` and its own encoder. Only the capture thread is shared (`src/video.cpp:2361-2386`). |
| D15 | Rate limits on both sides. Client: one request in flight, a minimum interval between changes, a dead band. Host: a newer request replaces an unread one, and a minimum interval of 2 s between encoder restarts. | Requests can come faster than they take effect. An encoder restart costs an IDR frame. |
| D16 | The stats overlay shows the target, the ceiling, the host encoder bitrate and the measured bitrate. The client logs each change with the reason. | The user must see that the feature works. |

User decisions (only those that change user-visible behavior). Decided by the user on 2026-10-09: U1 yes, U2 yes, U3 yes (default on), U4 as proposed, U5 yes (drop "Reduce your bitrate" when adaptation is on).

- U1. Manual bitrate is the ceiling and the controller can go below it (D11).
  The other choice: a manual bitrate is fixed and the controller is off. The
  spec takes the ceiling.
- U2. A manual bitrate does not scale on a live resize (D11). The other
  choice: scale it by `getDefaultBitrate(new size) / getDefaultBitrate(start
  size)`.
- U3. New setting "Adapt bitrate to the network", default on (D12).
- U4. Non-NVENC hosts get an encoder restart with an IDR frame on each change
  (D6). The rate limits make this at most one restart each 3 s on a decrease
  and each 15 s on an increase. The other choice: no adaptation for
  non-NVENC encoders (the host does not advertise D5).
- U5. The poor connection overlay text changes when adaptation is on (section
  6.6): "Poor connection to PC" without "Reduce your bitrate".

## 3. Wire protocol

### 3.1 Message ids

Ids in use:

- Client `ControlStream.c:235-251` (`packetTypesGen7Enc`): `0x0302`,
  `0x0307`, `0x0301`, `0x0201`, `0x0204`, `0x0206`, `0x010b`, `0x0109`,
  `0x010e`, `0x5500`-`0x5503`, `0x3100`, `0x3101`. Also `0x0200` (periodic
  ping, `ControlStream.c:1506`) and `SS_FRAME_FEC_PTYPE` `0x5502`
  (`Video.h:57`, client to host, sent each frame by `lossStatsThreadFunc`,
  `ControlStream.c:1484`).
- Host `src/stream.cpp:62-84` (`packetTypes`): the same plus `0x0305`,
  `0x0001`, `0x3000`-`0x3002`.
- `0x5502` has two meanings: the host names it "Set RGB LED" (host to
  client, `stream.cpp:901`), and the client sends FEC status with it (client
  to host). The host has no handler for it. This feature does not touch
  `0x55xx` (upstream Sunshine owns that range) or `0x30xx` (Apollo and
  Artemis own it).

New ids. `grep -rn "0x310[0-9a-f]"` finds only `0x3100`/`0x3101` in both
repositories. Before the implementation, check `0x3102`/`0x3103` again in
Artemis, nonary-vibepollo and logabell-vibepollo (the live resize spec checked
`0x3100`/`0x3101` there).

| Id | Name | Direction | Payload (little endian) |
|----|------|-----------|-------------------------|
| `0x3102` | `SET_BITRATE` | client to host | `uint32 request_id`, `uint32 configured_kbps` (8 bytes) |
| `0x3103` | `BITRATE_STATUS` | host to client | `uint32 request_id`, `uint32 requested_kbps`, `uint32 accepted_kbps`, `uint32 encoder_kbps`, `uint16 status` (18 bytes) |

Fields:

- `request_id`: starts at 1 and increases per request. A separate counter
  from the resize `request_id`.
- `requested_kbps`: the value of the request.
- `accepted_kbps`: the configured value after the host cap
  (`config::video.max_bitrate`). When it is less than `requested_kbps`, the
  client uses it as the new ceiling.
- `encoder_kbps`: the encoder bitrate after the full chain (D3). On a
  refusal (status 3 to 6), `accepted_kbps` and `encoder_kbps` are the values
  that run now, without a change.

Status codes (`adaptive_bitrate::status_e` on the host, `LI_BITRATE_STATUS_*`
in `Limelight.h`):

| Code | Name | Meaning |
|------|------|---------|
| 0 | `APPLIED` | The encoder runs at the new bitrate. No IDR. |
| 1 | `APPLIED_RESTART` | The host restarted the encoder at the new bitrate. One IDR. |
| 2 | `UNCHANGED` | The encoder already runs at this value. Nothing changed. |
| 3 | `NOT_SUPPORTED` | The encoder path cannot change the bitrate (sync path, section 5.5). |
| 4 | `INVALID` | `configured_kbps` is out of limits (section 5.2). |
| 5 | `ENCODER_FAILED` | The restart at the new bitrate failed, or the host refused a value of 0 kbps or less. The encoder runs at the old bitrate. |
| 6 | `INPUT_ONLY` | The session has no video (`config.input_only`). |

### 3.2 Byte order

Both structs are packed and little endian. The client fills them with `LE32` /
`LE16`, as `LiSendResizeRequest` does (`ControlStream.c:908-910`). The host
reads and writes them byte by byte with shifts
(`adaptive_bitrate::decode_request()` and `encode_status()` in
`src/adaptive_bitrate.h`). The bytes on the wire are the same as with
`std::memcpy` and `util::endian::little()` in the resize handler, and the code
does not depend on the byte order of the host CPU.

### 3.3 Encryption path

Client send: `LiSendBitrateRequest(uint32_t configuredKbps, uint32_t*
requestId)` in `ControlStream.c`. Copy `LiSendResizeRequest`
(`ControlStream.c:896-919`): the same guards (`DynamicBitrateSupported`,
`IS_SUNSHINE()`, `encryptedControlStream`, `peer != NULL`, `!stopping`), then
`sendMessageAndForget()` (`ControlStream.c:881`) on `CTRL_CHANNEL_GENERIC`
with `ENET_PACKET_FLAG_RELIABLE`. `sendMessageEnet` takes `enetMutex`, so any
client thread can call it. The client calls it only from the main thread
(section 6.2).

Host receive: the `IDX_ENCRYPTED` handler decrypts and calls
`server->call(type, ...)`. A new `server->map(packetTypes[IDX_SET_BITRATE],
...)` in `controlBroadcastThread` gets the plain payload. The host drops plain
messages on protocol 13 (`stream.cpp:605`).

Host send: `send_bitrate_status()` next to `send_resize_refused()`
(`stream.cpp:978`), with `control_header_v2`, `encode_control()` and
`control_server.send()`. Only `controlBroadcastThread` calls it, because ENet
is not thread safe.

Client receive: add `IDX_BITRATE_STATUS` to `needsAsyncCallback()`
(`ControlStream.c:1090`) and `queueAsyncCallback()` so that the callback runs
on the async callback thread, as for the resize refusal. New callback
`ConnListenerBitrateStatus bitrateStatus` as the last field of
`CONNECTION_LISTENER_CALLBACKS` (after `resizeRefused`, `Limelight.h:510`). A
`NULL` callback is allowed.

New indexes: client `IDX_SET_BITRATE 15`, `IDX_BITRATE_STATUS 16` (after
`IDX_RESIZE_REFUSED 14`, `ControlStream.c:162`), with `-1` in all tables
except `packetTypesGen7Enc`. Host `IDX_SET_BITRATE 21`, `IDX_BITRATE_STATUS
22` (after `IDX_RESIZE_REFUSED 20`, `stream.cpp:60`). Take the next free
numbers at implementation time, because the live resize work can add more.

### 3.4 Support detection

Host: in `cmd_describe` (`src/rtsp.cpp:792`) write
`a=x-ss-general.dynamicBitrate:1` when the chosen encoder has
`PARALLEL_ENCODING`. Use the existing check `video::encoder_supports_live_resize()`
(`src/video.cpp:2465-2468`): it tests exactly this flag. Call it with its
current name and a one-line comment. Do not rename it now: the live resize
work changes this code at the same time. Not Windows only.

Client: in `RtspConnection.c` next to the `x-ss-general.liveResize` parse
(`RtspConnection.c:1154-1159`), parse `x-ss-general.dynamicBitrate` into a new
global `DynamicBitrateSupported`. Expose `bool LiIsDynamicBitrateSupported(void)`.

### 3.5 Idempotency and ordering

- The host compares the encoder value of a request with the value that the
  session tracks (section 5.3). Equal values give `UNCHANGED` and no encoder
  action. Thus a repeated request is safe.
- The host keeps only the newest unapplied request. The mail event holds one
  value, and `raise()` replaces a value that nobody read (live resize spec
  4.2). The host answers for the request that took effect. It does not answer
  for a request that a newer one replaced before it took effect.
- Client rule: only a `BITRATE_STATUS` with `request_id == pending id` ends
  the pending request. One more status is accepted: a late answer for the
  request that timed out, while no other request is pending (it shows the
  real host state). The client accepts each id one time. The client logs and
  ignores each other status: an older id, id 0, a repeated answer, and a
  re-apply status (section 5.3) that has the id of an older request. The host
  sends id 0 for a re-apply of the start values (no answer before).
- Client timeout: 3 s without an answer ends the pending request. The target
  stays at the value of the request. The host can run another value (its
  watchdog and its re-apply, section 5.3). Thus, on the next free tick (rule
  1), the client sends the same target again (reason `resend`), with no dead
  band and no minimum interval. A repeated request is safe (see above), and
  this newer request replaces a host re-apply. If the late answer comes
  before that tick, the client uses it and does not send again. A timeout of
  the start request (section 4.4) sends the start request again instead.

## 4. Controller

### 4.1 Where it runs

`app/streaming/adaptivebitrate.h`: a header-only class
`AdaptiveBitrate::Controller` with no Qt, SDL or common-c calls, as
`app/streaming/liveresize.h` is. The session gives it samples and the time.
It returns a decision.

```
struct Sample {
    uint64_t nowMs;
    uint32_t framesFinished;   // cumulative, from LiGetVideoFrameCounters()
    uint32_t framesLost;       // cumulative
    uint32_t packetsVideo;     // cumulative, RTP_VIDEO_STATS.packetCountVideo
    uint32_t packetsFecRecovered;  // cumulative
    uint32_t rttMs;            // 0 when not known
    double measuredMbps;       // received video payload, without FEC
};

struct Decision {
    bool send;                 // true: send SET_BITRATE with targetKbps
    uint32_t targetKbps;       // configured kbps
    uint32_t fromKbps;
    Reason reason;             // for the log and the overlay
    bool isolated;             // rule 4
    bool rebased;              // the RTT baseline is now the current RTT
    bool timedOut;             // the pending request timed out
    uint32_t timedOutRequestId;
    bool stopped;              // the start request got no answer 3 times
    double lossPct[2];         // log signals (section 6.7)
    uint32_t rttMs;
    uint32_t rttBaselineMs;
    double fecPct;
    double measuredMbps;
};
```

Methods: `start(ceilingKbps, nowMs, adaptive)`, `tick(const Sample&)`,
`sent(decision, requestId, nowMs)`, `setCeiling(kbps, nowMs)` (live resize
or clamp), `onStatus(requestId, status, requestedKbps, acceptedKbps,
encoderKbps, nowMs)`, `notifySettle(nowMs, durationMs)`. `adaptive == false`
(D12): no rule runs. Only `setCeiling()`, a resend after a timeout and a resend
of the ceiling after `ENCODER_FAILED` make requests. A host that never
answers gets one resend each 3.25 s, with one request in flight; a resend of
the same value gets `UNCHANGED` and does not restart the encoder. There is no `onTimeout()`: `tick()` finds the timeout and
reports it in `Decision::timedOut` (section 6.2).

The controller works with deltas of the cumulative counters. It keeps the
last 12 deltas (3 s at the tick rate): rule 2 needs three 1 s windows.

### 4.2 Signals and update rates

| Signal | Source | Writer thread | Update rate |
|--------|--------|---------------|-------------|
| Frames finished and lost | New cumulative counters, changed only in `connectionSawFrame()` (`ControlStream.c`). It reads `lastGoodFrame`, which `connectionReceivedCompleteFrame()` sets as before. Read with new `LiGetVideoFrameCounters()`. One function changes both counters for a frame under one mutex, and the read takes the same mutex, so the pair is consistent and a tick boundary cannot show a false loss | video receive thread | each frame |
| FEC packets recovered and failed, video packets | `RTP_VIDEO_STATS` (`Limelight.h:948-956`, `LiGetRTPVideoStats()`). `RtpVideoQueue.c` did not count recovered video packets; the client now counts them in `reconstructFrame()` | video receive thread | each packet |
| RTT and RTT variance | `LiGetEstimatedRttInfo()` (ENet `roundTripTime`, smoothed) | ENet service | each ACK of a reliable packet; the periodic ping is reliable and goes each 100 ms (`ControlStream.c:332, 1500-1512`) |
| Measured bitrate | New session-owned `BandwidthTracker` (reuse `app/streaming/bandwidth.h`), fed in `Session::drSubmitDecodeUnit` (`session.cpp:630`) with `du->fullLength` | decoder submit thread | each frame; average over the last 2.5 s |

Notes:

- The existing interval counters in `connectionSawFrame` reset each 3 s and
  are static (`ControlStream.c:518-545`). The decoder counter
  `networkDroppedFrames` resets when the decoder is created again
  (`ffmpeg.cpp:2185`), which a live resize does. So the spec adds cumulative
  `uint32_t` counters. Update the "seen" counter before the early returns of
  the first sample period (`ControlStream.c:513-521`). A small mutex
  (`frameCounterMutex`, created in `initializeControlStream()`) protects the
  pair; the video receive thread takes it once for each new frame.
- Known effect: a frame that the depacketizer drops counts as lost, because
  `connectionReceivedCompleteFrame()` does not run for it. This includes the
  frames that the depacketizer drops while it waits for an IDR frame, after
  `requestDecoderRefresh()` (decoder error or recreate) or a decode unit
  queue overflow, or for a reference frame invalidation after a real loss.
  The existing 3 s loss check of common-c counts these frames the same way.
  The controller does not have a special case for them. The rules absorb
  them as follows: after `APPLIED_RESTART` and after a live resize,
  `RESTART_SETTLE_MS` (3 s) blocks decisions (rule 1), and rule 1 clears
  the windows. Thus the frames that the IDR wait dropped during the settle
  time do not count. Frames that it drops after the settle time come in at
  most one window, and one lossy window gives no change (rule 4). A single IDR wait at another time (for example a decoder
  error) usually lasts less than one 1 s window, so it gives one lossy
  window at most, and rule 4 makes no change. An IDR wait that follows a
  real network loss adds lost frames to that loss; this makes the loss
  signal larger, but it does not start a decrease without a real loss in a
  second window.
- The `BandwidthTracker` in `FFmpegVideoDecoder` (`ffmpeg.h:118`) also
  resets with the decoder, and other decoders do not have it. The session
  tracker covers all decoders and lives for the session.
- `du->fullLength` has no FEC. Compare it with `encoder_kbps`, not with
  the configured value.

### 4.3 Control law

Tick: every 250 ms. The session starts an `SDL_AddTimer` that pushes
`SDL_CODE_BITRATE_TICK`. The main thread calls `Controller::tick()`.

Windows: "1 s window" means the sum of the last 4 deltas.

Values (constants in `adaptivebitrate.h`; tests use the same names):

| Name | Value | Meaning |
|------|-------|---------|
| `TICK_MS` | 250 | Tick period |
| `START_SETTLE_MS` | 4000 | No decision after the first decoded frame. Common-c also ignores the first 3 s (`ControlStream.c:513-521`). |
| `CHANGE_SETTLE_MS` | 1000 | No decision after a status `APPLIED`. The queue in the network needs time to drain. |
| `RESTART_SETTLE_MS` | 3000 | No decision after `APPLIED_RESTART`, `ENCODER_FAILED` or a live resize (IDR frame, decoder recreate). |
| `FLOOR_KBPS` | `min(1500, ceiling)` | Lowest target. An absolute value: the network capacity does not depend on the stream size, so (b) wins over (a). |
| `LOSS_WINDOW_PCT` | 2 | A 1 s window with frame loss >= 2 % is "lossy" |
| `LOSS_HEAVY_PCT` | 10 | Heavy loss |
| `FEC_RECOVERED_PCT` | 3 | FEC recovered packets / video packets over 2 s >= 3 % is "FEC pressure" |
| `RTT_RISE_MIN_MS` | 15 | An RTT sample above `baseline + max(RTT_RISE_MIN_MS, baseline / 2)` is "delayed" |
| `RTT_DELAY_SAMPLES` | 3 | A 1 s window shows "delay" when this many of its 4 RTT samples are delayed |
| `RTT_BASELINE_MS` | 30000 | The baseline is the lowest RTT in the last 30 s |
| `DEC_LOSS` | 0.75 | Multiplier on sustained loss |
| `DEC_DELAY` | 0.90 | Multiplier on sustained delay or FEC pressure |
| `INC_STEP_PCT` | 8 | Increase step, percent of the current target |
| `INC_STEP_NEAR_PCT` | 3 | Increase step near the last failure rate |
| `STABLE_MS` | 4000 | Clean time before an increase |
| `NEAR_FAILURE_PCT` | 85 | "Near" means the target is >= 85 % of the last failure rate (memory of 60 s) |
| `APP_LIMITED_PCT` | 70 | Increase only when the measured bitrate is >= 70 % of `encoder_kbps` |
| `DEAD_BAND_PCT`, `DEAD_BAND_MIN_KBPS` | 3, 250 | Changes smaller than `max(3 %, 250 kbps)` are not sent, except a change to the floor or the ceiling. 250 kbps is also the smallest increase step. |
| `MIN_DEC_INTERVAL_MS`, `MIN_DEC_INTERVAL_RESTART_MS` | 1000, 3000 | Between two decreases (in place, restart mode) |
| `MIN_INC_INTERVAL_MS`, `MIN_INC_INTERVAL_RESTART_MS` | 4000, 15000 | Between two increases (in place, restart mode) |
| `REQUEST_TIMEOUT_MS` | 3000 | Pending request timeout |
| `LOSS_MIN_FRAMES` | 10 | A 1 s window with fewer frames gives no loss signal |
| `LOSS_MIN_TICKS` | 3 | Sustained loss also needs this many ticks with loss in the 3 s history |
| `FEC_MIN_PACKETS` | 200 | A 2 s window with fewer video packets gives no FEC signal |
| `MEASURED_MARGIN` | 0.9 | Rule 2: measured bitrate x 0.9 |
| `MAX_CUT` | 0.5 | One decrease keeps at least half of the target |
| `START_ATTEMPTS` | 3 | Start requests with no answer before the controller stops |
| `ENCODER_FAILED_MAX_WAIT_MS` | 60000 | Longest wait after `ENCODER_FAILED` results in a row (section 4.5) |

Rules, in order, at each tick:

1. If a request is pending, or a settle time runs, clear the windows (the
   deltas of this time are not a network signal). Return no decision. On the
   first free tick, a resend (sections 3.5 and 4.5) comes before the other
   rules.
2. Sustained loss: two of the last three 1 s windows are lossy, or one window
   has heavy loss and the RTT shows delay in the same window. Then
   `new = max(FLOOR, min(target * DEC_LOSS, measuredEncoderEquivalent * 0.9))`.
   Store `lastFailureKbps = target` and its time.
   `measuredEncoderEquivalent` converts the measured Mbps to configured kbps
   with the ratio `accepted_kbps / encoder_kbps` of the last status. The
   first request (section 4.4) gets a status before any decision, so this
   ratio is always known. This cut can be larger than `DEC_LOSS` when
   the network carries much less than the target.
3. Sustained delay or FEC pressure: the RTT shows delay in the last two 1 s
   windows, or FEC pressure in the 2 s window. Then
   `new = max(FLOOR, target * DEC_DELAY)`. Store `lastFailureKbps`.
4. An isolated burst (one lossy window or one delay window, and no rule 2 or
   3): no change (D8). Log it at debug level.
5. Increase: the target is below the ceiling, no lossy window, no delay and
   no FEC pressure for `STABLE_MS`, and the measured bitrate is at least
   `APP_LIMITED_PCT` of `encoder_kbps`. Step `INC_STEP_PCT`, or
   `INC_STEP_NEAR_PCT` when the target is near `lastFailureKbps`. Cap at the
   ceiling.
6. Apply the minimum intervals and the dead band. If the decision passes,
   return `send = true`.

Restart mode: after the first `APPLIED_RESTART` or `ENCODER_FAILED`, the
controller uses the restart intervals and doubles the increase steps for the
rest of the session (the host also keeps restart mode on, section 5.3). Thus
there are fewer IDR frames.

App-limited guard (rule 5): with CBR and filler data off
(`insert_filler_data = false`, `src/nvenc/nvenc_config.h:50`), a static
picture uses much less than the target. No loss then proves nothing about the
network. Thus a desktop with no motion stays at its current target. It does
not climb. A decrease does not need the guard.

### 4.4 Start

1. `Session` starts the controller at the first
   `SDL_CODE_FIRST_FRAME_DECODED` (`session.cpp:2441`) with the ceiling
   (section 6.1). The start target is the ceiling. The SDP already started
   the encoder at `m_StreamConfig.bitrate`. This event comes again after
   each decoder recreate (`session.cpp:468`). `start()` and the tick timer
   act only on the first event of the session. Later events do nothing:
   the resize path sets its own settle time (section 4.6).
2. The first tick after `START_SETTLE_MS` always sends one request with the
   ceiling, also when it equals `m_StreamConfig.bitrate`. The host answers
   `UNCHANGED` or `APPLIED` with the real `accepted_kbps` and
   `encoder_kbps`. Rules 2 and 5 need these values, and a `max_bitrate` cap
   shows at once. No other decision comes before this status (rule 1).
3. The RTT baseline starts with the first RTT sample.

### 4.5 Host refusal and limits

- `accepted_kbps < requested_kbps` in an `APPLIED`, `APPLIED_RESTART` or
  `UNCHANGED` status: the host cap is lower. The controller sets the ceiling
  to `accepted_kbps` for the rest of the session. Do not use this rule for
  `ENCODER_FAILED` or another refusal: these statuses give the values that
  run now (section 3.1), thus `accepted_kbps` can be lower than
  `requested_kbps` with no host cap.
- `UNCHANGED`: the controller takes the value as applied.
- `NOT_SUPPORTED` or `INPUT_ONLY`: the controller stops for the session. The
  client logs it once.
- `INVALID`: the client has a bug. Log at error level and stop the
  controller.
- An unknown status code: stop the controller, as for `INVALID`.
- `ENCODER_FAILED`: the target goes back to the value that runs
  (`accepted_kbps`, section 3.1), but not above the ceiling. Restart mode goes
  on. The controller waits, then follows the normal rules. The wait is
  `RESTART_SETTLE_MS` (3 s) and doubles for each `ENCODER_FAILED` in a row
  (3 s, 6 s, 12 s ... at most `ENCODER_FAILED_MAX_WAIT_MS`, 60 s). An
  `APPLIED`, `APPLIED_RESTART` or `UNCHANGED` status resets the wait to 3 s.
  Thus a host that cannot restart its encoder gets few requests.
- `ENCODER_FAILED` with a running value above the ceiling (a live resize
  lowered the ceiling, section 4.6): after the wait, the controller sends the
  ceiling again (reason `resend`).

### 4.6 Live resize

After `Session::applyStreamSize()` (`session.cpp:518`):

1. Calculate the new ceiling (section 6.1). In manual mode it stays.
2. Call `setCeiling(newCeiling, now)`:
   - If the target was at the old ceiling (not limited by the network), the
     target becomes the new ceiling. This is the same as a new stream at the
     configured speed (D9).
   - Else, the network limited the target. The network capacity does not
     depend on the stream size. The target stays, but not above the new
     ceiling.
3. Start `RESTART_SETTLE_MS`.
4. If the target changed, send `SET_BITRATE` now. This bypasses the minimum
   intervals and rule 1: the new request gets a new `request_id` and becomes
   the pending request. A later status for the older request has a smaller
   id, and the client ignores it (section 3.5).

The request goes after the size change, not before. A request before the
resize gives a wrong bitrate when the host refuses the resize. On
non-NVENC hosts, this costs a second encoder restart (one more IDR) after the
resize restart. Accepted.

The host releases a bitrate change only when no resize is busy (section 5.3),
but it checks this only at the release. A resize that starts after the
release can share one encoder restart with the bitrate change. If that
encoder fails, both changes go back: the client gets `RESIZE_REFUSED` and
`ENCODER_FAILED`. Both answers are correct. Accepted.

### 4.7 Poor connection overlay

Common-c raises `CONN_STATUS_POOR` at 30 % loss in one 3 s window, or 15 % in
two (`ControlStream.c:143-145, 523-536`). The controller acts at 2 % loss in
two 1 s windows. Thus the controller normally lowers the bitrate before the
overlay comes on. The overlay stays as it is, with a new text (section 6.6).

## 5. Host changes

### 5.1 Bitrate chain (one function)

New `src/adaptive_bitrate.h`, namespace `adaptive_bitrate`. It is header
only (no `.cpp`) and has no other Sunshine dependency, thus its unit test
builds it alone:

```
struct chain_input_t {
  std::int64_t configured_kbps;
  int max_bitrate;          // config::video.max_bitrate, 0 = no cap
  std::size_t warp_factor;  // 1 when not engaged
  int fec_percentage;       // config::stream.fec_percentage
  bool audio_high_quality;  // config.audio.flags[HIGH_QUALITY]
  int audio_channels;
  bool limit_framerate;     // config::video.limit_framerate
};
struct chain_result_t {
  std::int64_t accepted_kbps;  // after the cap
  std::int64_t encoder_kbps;   // after all steps
};
chain_result_t encoder_bitrate(const chain_input_t &);
```

The function does the steps of `rtsp.cpp:1074-1154` in the same order: cap at
`max_bitrate`, multiply by the warp factor (only when `limit_framerate` is
true and `warp_factor >= 2`, as `rtsp.cpp` does), divide for FEC (when
`fec_percentage <= 80`), subtract audio (at most 20 %), subtract 500 kbps (at
most 10 %). `cmd_announce` (`rtsp.cpp:943`) calls it and logs the same lines as today. The
SDP fallback (`configuredBitrateKbps == 0`, `rtsp.cpp:1068-1070`) stays in
`cmd_announce`: the dynamic path never sees it.

`cmd_announce` stores `chain_input_t` (with the `configured_kbps` of the
stream start, which `session::alloc` uses for the start `accepted_kbps`) in
the launch session config, so the control handler can call the function with the
same audio, FEC and warp values. Add the field `adaptive_bitrate::chain_input_t
bitrate_chain` to `stream::config_t` (`src/stream.h:27-29`), next to
`monitor` and `audio`.

### 5.2 Control handler

`server->map(packetTypes[IDX_SET_BITRATE], ...)` in `controlBroadcastThread`:

1. Size check: `payload.size() < 8` gives a log and return.
2. Convert with `adaptive_bitrate::decode_request()` (byte by byte, section
   3.2).
3. `config.input_only`: answer `INPUT_ONLY`.
4. Encoder not parallel (`!video::encoder_supports_live_resize()`): answer
   `NOT_SUPPORTED`.
5. Limits: `500 <= configured_kbps <= 1000000`. Else answer `INVALID`. (The
   client UI allows up to 500000 with `unlockBitrate`.)
6. `encoder_bitrate()` with the stored chain input.
7. If `encoder_kbps` equals `session->bitrate.encoder_kbps`, `pending` is
   empty and `in_flight` is empty: answer `UNCHANGED` with the values of the
   request (`requested_kbps`, `accepted_kbps` and `encoder_kbps` of the
   chain). The state stores these values as the told values (section 5.3).
   The chain can give one encoder value for two configured values, thus
   `accepted_kbps` can differ from the running value. The answer must not give
   `accepted_kbps < requested_kbps` when the host has no cap.
8. Else store the request in `session->bitrate.pending` (it replaces an
   older pending request) and let the control loop release it (section 5.3).

### 5.3 Session state and mails

New mails in `src/globals.h` next to the resize mails (`globals.h:60-63`).
`mail_raw_t::event<T>` casts with no check, so each raise and each read uses
exactly these types (live resize spec 4.2):

| Mail | Type | Raised by | Read by |
|------|------|-----------|---------|
| `bitrate` | `adaptive_bitrate::change_t` {`uint32 request_id`, `int encoder_kbps`, `uint32 requested_kbps`, `uint32 accepted_kbps`} | control thread | `encode_run` each frame; `capture_async` at the top of its loop |
| `bitrate_result` | `adaptive_bitrate::result_t` {`change_t change`, `uint16 status`} | `encode_run`, `capture_async` | control thread |

New `session_t::bitrate` (`src/stream.cpp:373` struct):

- Persistent handles `change_queue` (`mail::bitrate`) and `result_queue`
  (`mail::bitrate_result`), taken at session start. A raise on a mail with no
  holder is lost (live resize spec 4.2).
- `encoder_kbps`, `accepted_kbps`: the values that run now. Start values
  from `config.monitor.bitrate` and the capped configured value of
  `cmd_announce`.
- `pending` (`std::optional<change_t>`): the newest request that is not
  released.
- `in_flight` (`std::optional<change_t>`) and `in_flight_since`
  (`steady_clock::time_point`): the newest released request with no result
  yet.
- `restart_mode` (`bool`): set after the first `APPLIED_RESTART` or
  `ENCODER_FAILED`.
- `last_restart` (`steady_clock::time_point`).
- `told` (`std::optional<change_t>`): the values of the newest answer that
  the client uses (`UNCHANGED`, or the result of the request in flight).

Control loop, next to the resize queue drain (`stream.cpp:1516-1620`):

1. Drain `result_queue`. For each result: update `encoder_kbps` and
   `accepted_kbps` on `APPLIED`, `APPLIED_RESTART` and `UNCHANGED` (the
   encode thread gives `UNCHANGED` when the value already runs, section 5.4;
   the values are then the same). Set `restart_mode` and `last_restart` on
   `APPLIED_RESTART` and on `ENCODER_FAILED`: a failed restart also costs an
   encoder start, thus the 2 s interval applies after it. `ENCODER_FAILED`
   does not change `encoder_kbps` or `accepted_kbps`. Call
   `send_bitrate_status()`. Clear `in_flight` only when
   `result.change.request_id == in_flight->request_id`, and then store the
   running values as the told values. A result for an older request (a newer
   one was released after it) leaves `in_flight` set.
   Late result: the watchdog (step 3) can clear a request before its result
   comes. The client can then get a later answer (for example `UNCHANGED`)
   with other values. When a result is not for `in_flight`, `pending` and
   `in_flight` are empty, and the new `encoder_kbps` is not the told encoder
   value, the state stores the told values in `pending`. The encoder then
   goes back to the value that the client uses. A newer request replaces
   this re-apply as any pending request (the newest wins). Before the first
   answer, the told values are the start values. The client resends its
   target after its own 3 s timeout (section 3.5), thus a re-apply of an
   older value does not stay: the resend replaces it.
2. Release: if `pending` is set, no live resize is busy (a resize request
   in progress or its display thread runs), and (`!restart_mode` or
   `now - last_restart >= 2 s`), raise `change_queue` with it, move it to `in_flight` and set
   `in_flight_since = now`. A new release
   while `in_flight` is set is allowed: the mail keeps the newest value.
3. Watchdog: if `now - in_flight_since > 5 s`, clear `in_flight` and log a
   warning. (The client has its own 3 s timeout.)

The loop runs at least each 150 ms (`server->iterate(150ms)`,
`stream.cpp:1631`). Thus a release waits at most 150 ms.

On session stop (the `STOPPING` branch, `stream.cpp:1484-1500`), clear
`pending`, `in_flight` and `told`.

### 5.4 Encode thread: apply the bitrate

`encode_run` (`src/video.cpp:1904`) takes `config_t config` by value today.
Change it to `config_t &config`. Its only caller is `capture_async`
(`video.cpp:2448`), which owns the config. Thus a bitrate that `encode_run`
applies stays in the config of `capture_async`, and a later reinit (display
change, live resize) keeps it.

New virtual in `encode_session_t` (`src/video.h:206`): `virtual bool
set_bitrate(int kbps) { return false; }`. `nvenc_encode_session_t` overrides
it (section 5.5). `avcodec_encode_session_t` uses the default.

In the `encode_run` loop, next to the IDR and invalidate checks
(`video.cpp:2002-2015`):

```
if (auto change = bitrate_events->pop(0ms)) {
  if (change->encoder_kbps == config.bitrate) {
    raise result UNCHANGED
  } else if (session->set_bitrate(change->encoder_kbps)) {
    config.bitrate = change->encoder_kbps;
    raise result APPLIED
  } else {
    // restart at the new bitrate
    bitrate_restart = *change;   // local of capture_async, passed by reference
    break;
  }
}
```

`bitrate_events` is `mail->event<adaptive_bitrate::change_t>(mail::bitrate)`,
taken with the other events at `video.cpp:1954`.

In `capture_async` (`video.cpp:2361`):

- At the top of the loop, next to the resize pop (`video.cpp:2409-2415`):
  pop `mail::bitrate` (a request that came while no encoder ran); it
  replaces `bitrate_restart`, because it is newer. If `bitrate_restart` is
  set, `previous_bitrate = config.bitrate; config.bitrate =
  bitrate_restart->encoder_kbps; bitrate_applying = bitrate_restart;
  bitrate_restart.reset()`. Thus the top of the loop clears
  `bitrate_restart` when it takes it.
- `encode_run` gets `bitrate_applying`. At the `resize_done` raise point
  (`video.cpp:1924`), the new encoder runs: when `bitrate_applying` is set,
  raise result `APPLIED_RESTART` there.
- After `encode_run`, next to the resize failure check
  (`video.cpp:2458-2465`): if `encoder_failed` and `bitrate_applying`, set
  `config.bitrate = previous_bitrate` and raise result `ENCODER_FAILED`.
- After `encode_run` returns and after the checks above, always clear
  `bitrate_applying`, as the resize code clears `resize_pending`
  (`video.cpp:2465`). Else a later failure reverts a wrong value. Do not
  clear `bitrate_restart` there: `encode_run` sets it when it breaks for a
  restart, and the next loop pass takes it.
- The live resize revert (`config = last_good_config`, `video.cpp:2462`)
  restores the size. It must keep the newest bitrate: `int keep =
  config.bitrate; config = last_good_config; config.bitrate = keep;`.

An encoder restart in this path does not set `reinit_event`. `capture_async`
loops, takes the same display, and calls `make_encode_device` and
`encode_run` again. The new session starts with an IDR. The capture thread
and the other sessions do not see it.

Known effects and limits:

- On NVENC, a change that the top of the `capture_async` loop takes (it came
  while no encoder ran, for example during a reinit) starts a new encoder
  and gives `APPLIED_RESTART`, not `APPLIED`. The host then keeps restart
  mode on, and later in-place changes wait for the 2 s restart interval.
- A change with `encoder_kbps <= 0` gives result `ENCODER_FAILED` and a
  warning log, in the `encode_run` loop and at the top of the
  `capture_async` loop. The host does not start an encoder at the new bitrate, and the
  encoder keeps its bitrate. The control thread then counts it as a
  failed restart: restart mode goes on and the 2 s interval starts.
- When `make_encode_device` fails in `capture_async`, the function returns
  with no result for a change in progress, and the session stops.

### 5.5 NVENC: change points in `nvenc_base`

Header: the host includes `<ffnvcodec/nvEncodeAPI.h>` from the submodule
`third-party/nv-codec-headers` (not checked out in the local tree). The fields
below are from the NVENC SDK 12.1 header (`NV_ENC_RECONFIGURE_PARAMS` at
lines 2110-2137). Check them against the submodule version at
implementation.

```
typedef struct _NV_ENC_RECONFIGURE_PARAMS {
    uint32_t                 version;            // NV_ENC_RECONFIGURE_PARAMS_VER
    NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
    uint32_t                 resetEncoder :1;    // only with an IDR frame
    uint32_t                 forceIDR     :1;
    uint32_t                 reserved     :30;
} NV_ENC_RECONFIGURE_PARAMS;
```

`maxBitRate` is "used for VBR and ignored for CBR mode" (header line 1484).
The host uses CBR (`nvenc_base.cpp:239`) and never sets it. The change sets
only `averageBitRate` and `vbvBufferSize`.

Changes in `src/nvenc/nvenc_base.h` and `.cpp`:

1. New private members: `NV_ENC_INITIALIZE_PARAMS saved_init_params`,
   `NV_ENC_CONFIG saved_enc_config`, `uint32_t saved_framerate`,
   `int saved_vbv_percentage_increase`, `bool custom_vbv`.
2. `create_encoder()`: `init_params` (`nvenc_base.cpp:142`) and `enc_config`
   (`nvenc_base.cpp:235`) are locals today. After the encoder config is
   complete (`nvenc_base.cpp:382`), copy both into the members, set
   `saved_init_params.encodeConfig = &saved_enc_config`, and pass
   `&saved_init_params` to `nvEncInitializeEncoder` (`nvenc_base.cpp:384`).
   Save `client_config.framerate`, `config.vbv_percentage_increase` and the
   result of `get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)`
   (`nvenc_base.cpp:250`). `nvenc_base` is not copyable
   (`nvenc_base.h:34-35`), so the self pointer is safe.
3. Move the VBV formula of `nvenc_base.cpp:250-253` into a private helper
   `vbv_size(kbps)` and use it in `create_encoder()` and in step 4.
4. New public `bool reconfigure_bitrate(uint32_t kbps)`:
   - Return false when `encoder == nullptr`.
   - Make a copy of `saved_enc_config`. Set
     `rcParams.averageBitRate = kbps * 1000` and, when `custom_vbv`,
     `rcParams.vbvBufferSize = vbv_size(kbps)`.
   - `NV_ENC_RECONFIGURE_PARAMS params =
     {min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER)}`;
     `params.reInitEncodeParams = saved_init_params` with `encodeConfig`
     pointing at the copy; `params.resetEncoder = 0`; `params.forceIDR = 0`.
   - Call `nvenc->nvEncReconfigureEncoder(encoder, &params)`. On failure log
     `last_nvenc_error_string` and return false. On success copy the new
     config into `saved_enc_config` and return true.
   - Log at info level: old and new bitrate and VBV size.
5. Thread: only the encode thread calls it, between two `encode_frame()`
   calls. `encode_frame` waits for the output (`nvEncLockBitstream`,
   `nvenc_base.cpp:532-545`), so no frame is in the encoder at that time.
   With `enablePTD = 1` (`nvenc_base.cpp:218`) and `resetEncoder = 0`, the next
   frame stays a P frame (spike S1 confirms this).

`nvenc_encode_session_t::set_bitrate(int kbps)` (`video.cpp:393-437`) calls
`device->nvenc->reconfigure_bitrate(kbps)`. If it returns false, `encode_run`
uses the restart path. Thus a driver that refuses the reconfigure still gets
the new bitrate, with an IDR.

`nvenc_d3d11` and `nvenc_cuda` derive from `nvenc_base`. They need no
change.

### 5.6 Other encoders

- AMF, Quick Sync, software (Windows) and VAAPI (Linux): avcodec sessions.
  `set_bitrate` returns false. They use the restart path. `make_avcodec_encode_session`
  reads `config.bitrate` for `bit_rate`, `rc_max_rate`, `rc_min_rate` and
  `rc_buffer_size` (`video.cpp:1750-1777`), so the new encoder gets the new
  values with no other change.
- Sync path (encoders without `PARALLEL_ENCODING`, `encode_run_sync`,
  `video.cpp:2182`, started from `capture()` at `video.cpp:2477-2500`): it
  does not read `mail::bitrate`. The host does not
  advertise D5 then and the handler answers `NOT_SUPPORTED`. All Windows
  encoders set `PARALLEL_ENCODING`.

### 5.7 Multiple clients and limits

- Each session has its own mails, its own `capture_async` and its own
  encoder. A change for one session does not touch another.
- `config::video.max_bitrate` (`src/config.h:146`) caps every request through
  the chain (5.1). The answer gives the capped value (`accepted_kbps`).
- No new `crypto::PERM` bit. The session that streams may set its own
  bitrate.

### 5.8 Logs

- Info: each applied change: `Bitrate request <id>: <accepted> kbps ->
  encoder <encoder> kbps (in place|restart)`. `<accepted>` is
  `accepted_kbps` (the configured value after the host cap).
- Info: `UNCHANGED`, `INVALID`, `NOT_SUPPORTED`, `ENCODER_FAILED`.
- Debug: a pending request that a newer one replaces.

## 6. Client changes

### 6.1 Ceiling (target a)

New `Session::calculateBitrateCeiling()`:

- `autoAdjustBitrate` on: `StreamingPreferences::getDefaultBitrate(
  m_ActiveVideoWidth, m_ActiveVideoHeight, m_ActiveVideoFrameRate, yuv444)`.
  `yuv444` is `(m_ActiveVideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0`, from
  `drSetup` (`session.cpp:613-618`). This is the negotiated format, so the
  rule covers the host that has no YUV444 (the start-time swap at
  `session.cpp:1972-1988`).
- `autoAdjustBitrate` off: `m_Preferences->bitrateKbps` (D11).
- Then clamp to the ceiling that the host sent (`accepted_kbps`, section 4.5).

The value is the configured bitrate (D3). The client does not apply the 0.8
factor of `SdpGenerator.c:349`. The host makes all reductions.

`m_StreamConfig.bitrate` keeps the start value. The controller state holds
the current target.

### 6.2 Threads and timers

- The controller runs on the main thread, in the event loop of
  `Session::execInternal`. That loop ends before `LiStopConnection()`
  (`session.cpp:1568-1574`), so every send is between `LiStartConnection` and
  `LiStopConnection`.
- Tick timer: `SDL_AddTimer(250, ...)` pushes `SDL_CODE_BITRATE_TICK`. Start
  it at the first decoded frame of the session only (section 4.4). Remove it
  when the event loop ends.
- Request timeout: the controller checks `REQUEST_TIMEOUT_MS` in its tick. No
  extra timer.
- `clBitrateStatus` (new static callback, last entry of `k_ConnCallbacks`,
  `session.cpp:63-77`) runs on the async callback thread. It pushes
  `SDL_CODE_BITRATE_STATUS` with the fields in a heap struct (`data1`). The
  main thread calls `Controller::onStatus()` and frees the struct.
- New SDL user event codes: take a block from 120 (`SDL_CODE_BITRATE_TICK`
  120, `SDL_CODE_BITRATE_STATUS` 121). 100 to 111 are in use
  (`session.cpp:26-36`, `decoder.h:7-15`). Take the next free numbers at
  implementation time.

### 6.3 moonlight-common-c

- `ControlStream.c`: ids and tables (3.3), `LiSendBitrateRequest()`, async
  callback, cumulative frame counters and `LiGetVideoFrameCounters(uint32_t*
  finishedFrames, uint32_t* lostFrames)`. Reset the counters in the init function of
  the control stream with the other statics.
- `RtspConnection.c`: parse `x-ss-general.dynamicBitrate`.
- `RtpVideoQueue.c`: count recovered video data packets in
  `RTP_VIDEO_STATS::packetCountFecRecovered`.
- `Limelight.h`: `LI_BITRATE_STATUS_*`, callback typedef and field,
  `LiSendBitrateRequest`, `LiIsDynamicBitrateSupported`,
  `LiGetVideoFrameCounters`.

### 6.4 Settings

- `StreamingPreferences`: `bool adaptiveBitrate`, key `adaptivebitrate`,
  default `true` (next to `autoAdjustBitrate`,
  `streamingpreferences.cpp:150-152, 358-360`; `Q_PROPERTY` next to
  `streamingpreferences.h:141-143`).
- `SettingsView.qml`: one check box under the bitrate slider
  (`SettingsView.qml:674-727`): "Adapt bitrate to the network". Tool tip:
  "Lower the bitrate when the network is congested and raise it again up to
  the selected bitrate."
- Command line: `parser.addToggleOption("adaptive-bitrate", "network
  adaptive bitrate")` next to `bitrate` (`cli/commandlineparser.cpp:351`).

### 6.5 Stats overlay

`stringifyVideoStats` (`ffmpeg.cpp:819`) runs on the decoder thread. The
measured bitrate line is inside `#ifdef DISPLAY_BITRATE` (`ffmpeg.cpp:898-919`),
and no build file defines it. Add one new line outside that block:

```
Bitrate: target 42.0 Mbps (limit 80.0), host encoder 33.4 Mbps, measured 31.2 Mbps
```

The session publishes the values with `std::atomic<uint32_t>` members (target,
ceiling, encoder kbps). The measured value comes from the session
`BandwidthTracker` (thread safe, `bandwidth.h:20-21`). When the controller
does not run, the line shows "fixed" after the target.

### 6.6 Poor connection overlay

`clConnectionStatusUpdate` (`session.cpp:202-208`) shows "Slow connection to
PC\nReduce your bitrate" when the bitrate is above 5000 kbps. When the
controller runs, show "Poor connection to PC" (the client already lowers the
bitrate). Else keep the current text. The owner logic
(`m_StatusOverlayOwner`) does not change.

### 6.7 Logs

Info, one line for each decision that sends a request:

```
Adaptive bitrate: 60000 -> 45000 kbps, reason loss (loss 4.1% 3.2%, rtt 38/22 ms, fec 1.2%, measured 52.3 Mbps)
```

Info for each status (status name, accepted, encoder kbps). Debug for an
isolated burst (D8). At the session end, log the lowest, highest and mean
target.

## 7. Build and deploy

### 7.1 Host

- The fork `catapultam/apollo-microphone` builds on GitHub Actions with
  `.github/workflows/build-windows.yml` (live resize spec 6.1, R4). Push the
  branch, run the workflow, and take the NSIS installer from the artifact.
- Install on CPLT-4A with the `ApolloUpdate` scheduled task. That task is in
  work now and has no notes yet. When it is ready, its page in
  `catapultam/homelab-notes` gives the steps. Until then, use the manual
  install of the live resize spec 6.2 (backup of `config\`, keep the old
  installer for rollback).
- After the install, check the RTSP DESCRIBE log or a client log for
  `x-ss-general.dynamicBitrate`.

### 7.2 Client

As the live resize spec 6.3: build in the `moonlight` toolbox. Commit the
common-c change in `Catapultam-GMG/moonlight-common-c-mic` first, then move the
submodule pointer. Clean `app/` after header changes.

## 8. Testing

### 8.1 Controller unit test (client)

`app/tests/adaptivebitrate_test.cpp`, plain `assert`, built with `g++` in the
toolbox as `app/tests/liveresize_test.cpp` is. It feeds `Sample` sequences
with a fake clock:

1. Clean network: the target stays at the ceiling. No request.
2. Start settle: loss in the first 4 s gives no decision.
3. Sustained loss (5 % in three windows): one decrease to 75 %, then no
   second decrease before `MIN_DEC_INTERVAL_MS`.
4. Isolated burst (one window with 12 % loss and an RTT spike, as in the
   Wi-Fi stall): no change.
5. Heavy loss with RTT rise in one window: a decrease.
6. Delay only (RTT baseline + 20 ms for 2 s): decrease to 90 %.
7. FEC pressure only: decrease to 90 %.
8. Recovery: after `STABLE_MS` with no loss and measured >= 70 %: increase
   by 8 %; near `lastFailureKbps`: increase by 3 %.
9. App-limited: no loss, measured 20 % of the target: no increase.
10. Floor and ceiling hold.
11. Dead band: a change below 3 % or 250 kbps is not sent.
12. Pending request: no new decision until the status or the timeout; a
    status with an old id is ignored; a status with a newer id ends the
    pending request.
13. `accepted_kbps < requested_kbps`: the ceiling drops to it.
14. `setCeiling` after a resize: at ceiling, the target follows; network
    limited, the target stays and is capped.
15. Restart mode: longer intervals and double steps.
16. Start: the first tick after `START_SETTLE_MS` sends the ceiling also when
    it equals the start bitrate; no other decision before its status.
17. A second `start()` (decoder recreate) changes nothing.
18. `ENCODER_FAILED`: the target goes back, the rules start again after the
    wait; after a lower ceiling the ceiling is sent again; the wait doubles
    up to 60 s and an applied status resets it.
19. Timeout: the next free tick sends the target again (also with adaptive
    off); a late answer before that tick is accepted and stops the resend.
20. An unknown status code stops the controller. An RTT of 0 at all times
    gives no delay signal, and the loss rule still works.

### 8.2 Protocol tests

- Host googletest `tests/unit/test_adaptive_bitrate.cpp` (as
  `tests/unit/test_live_resize.cpp`): `encoder_bitrate()` gives the same
  values as the old inline code for a table of inputs (FEC 0/20/90, stereo
  and 7.1, high and normal audio, cap on and off, warp 1 and 2); payload
  sizes (`sizeof` request 8, status 18); byte order with
  `util::endian::little`.
- Client common-c: `LiSendBitrateRequest` returns an error with no
  connection and when the host did not advertise D5.

### 8.3 Manual end-to-end test

Setup: CPLT-4A with the new host build, the laptop with the new client,
NVENC HEVC, 2560x1600 at 60 fps, auto bitrate on, adaptive on. Play a video
or a game on the host: a static desktop is app-limited (section 4.3). Turn
on the stats overlay (`Ctrl+Alt+Shift+S`).

Throttle the download of the laptop. `tc ... root tbf` on the laptop
interface shapes only the upload (client to host). The stream is a download,
so use an `ifb` device:

```
sudo modprobe ifb numifbs=1
sudo ip link set ifb0 up
sudo tc qdisc add dev wlp192s0 handle ffff: ingress
sudo tc filter add dev wlp192s0 parent ffff: protocol all u32 match u32 0 0 \
    action mirred egress redirect dev ifb0
sudo tc qdisc add dev ifb0 root tbf rate 30mbit burst 64kb latency 50ms
# later: change the rate
sudo tc qdisc change dev ifb0 root tbf rate 15mbit burst 64kb latency 50ms
# loss or delay only
sudo tc qdisc replace dev ifb0 root netem delay 40ms loss 3%
# clean up
sudo tc qdisc del dev wlp192s0 ingress; sudo tc qdisc del dev ifb0 root
```

On Silverblue, `tc` is in `iproute` on the host. Use the interface of the
stream path (`tailscale0` for a remote test).

Steps:

1. No throttle: the target stays at the ceiling (about 40 Mbps for
   2560x1600 at 60 fps). No request in the logs.
2. `tbf rate 30mbit`: within about 5 s the target goes down below 30 Mbps.
   The host log shows `in place`. No IDR (the client log shows no new key
   frame and no `Recreating renderer`).
3. `tbf rate 15mbit`: the target follows down.
4. Remove the throttle: the target climbs back to the ceiling in steps.
5. `netem loss 3%` alone: check the decrease on loss. `netem delay 40ms`
   with no loss and no rate cap: one decrease on delay at most, then the
   baseline follows (the delay is constant).
6. Live resize (`Ctrl+Alt+Shift+W`) to a smaller window: the ceiling and
   the target go down. Resize back: they go up.
7. Set `max_bitrate = 20000` on the host: the client ceiling drops to the
   accepted value after the first request.
8. Turn off "Adapt bitrate to the network": no request, except one after a
   live resize.
9. Stock host: the client does not run the controller and logs one line.
10. A second client on the same host: each session changes its own bitrate.
11. Non-NVENC test (if a host with AMF or Quick Sync is available, or with
    `encoder = software`): the status is `APPLIED_RESTART` and the client sees
    one IDR per change. Check the intervals.

## 9. Risks and spikes

| # | Risk | How to resolve early |
|---|------|----------------------|
| R1 | `nvEncReconfigureEncoder` on the RTX 4090 refuses a change of `averageBitRate` or `vbvBufferSize` with `resetEncoder = 0`, or makes an IDR frame. | Spike S1: a small test in the host build that calls `reconfigure_bitrate()` each 5 s between 10 and 60 Mbps during a stream. Check `NV_ENC_SUCCESS`, `lock_bitstream.pictureType` of the next frames (no `NV_ENC_PIC_TYPE_IDR`) and the frame size log (`NvEnc: encoded frame sizes in kB`). If the VBV change fails: keep `vbvBufferSize` for the ceiling and change only `averageBitRate`. If all fails: use the restart path for NVENC too (U4 applies to all). |
| R2 | `min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER)` gives a wrong version with the v11 API (H.264 and HEVC use API 11.0, `nvenc_base.cpp:103`). | S1 runs with H.264 and HEVC. Compare the struct version macros of SDK 11 and 12 in the submodule. |
| R3 | Quality drops after a decrease, or the rate control overshoots after an increase (CBR with infinite GOP, `nvenc_base.cpp:237`). | Watch the frame size log after each change in S1. If the overshoot is large, cap the increase step lower. |
| R4 | The controller oscillates between two values. | Dead band, minimum intervals, smaller steps near `lastFailureKbps`, settle times. Unit test 8.1 and manual steps 2 to 4. Log every decision. |
| R5 | The controller lowers the bitrate on Wi-Fi stalls that a lower bitrate does not fix. | D8. Manual test at the remote site of the memory note, or `netem` with bursts. |
| R6 | An IDR frame after a restart or a resize looks like congestion (burst of packets, short loss). | Settle times (4.3). Unit test 2. |
| R7 | The app-limited guard keeps a static desktop at a low target after congestion ends. The next motion then has a low bitrate until the controller climbs. | Accepted. The climb is 8 % each 4 s. If too slow: allow a jump to the last good target when the measured bitrate reaches it. |
| R8 | ENet RTT is smoothed and only updates on ACKs, so the delay signal lags. | The ping goes each 100 ms. The loss rules do not need RTT. Check the lag in S2. |
| R9 | `encode_run` with `config_t &` changes the config of `capture_async` in a place that the live resize code does not expect. | The only field that `encode_run` writes is `bitrate`. The resize revert keeps it (5.4). Host review checks every `config` write. |
| R10 | Restart path: two restarts close together (resize, then bitrate) cost two IDR frames. | Accepted for non-NVENC (4.6). The host 2 s restart interval limits the rate. |
| R11 | The id `0x3102`/`0x3103` collides in another fork (Artemis, vibepollo). | Check before the implementation (3.1). |
| R12 | The cumulative frame counter wraps (`uint32_t`). | The controller uses unsigned deltas, which handle one wrap. At 240 fps, a wrap takes 207 days. |
| R13 | A bad host sends a status with a huge `accepted_kbps`. | The client clamps the ceiling only down, never up. |

Spike S2: the controller with the real signals and no host change. Run the
client with the controller in "log only" mode against the current host, apply
the `tc` steps of 8.3, and check that the decisions are correct before the
host work ends. Spike order: S1 and S2 in parallel. Write the implementation
plan after both have results.

# Live resize: design

Date: 2026-10-09
Status: draft for review. No code exists yet.

## 1. Goal

One hotkey in the Moonlight client makes the stream resolution and the host
virtual display (VDD) resolution equal to the size that the stream window can
show now. The size is the window content area in physical pixels, without
decorations. Odd sizes such as 2537x1391 are valid input. The resize is live:
no reconnect, and the host application keeps running.

We control both ends:

- Client: `~/GitHub/moonlight-qt-mic` (moonlight-qt fork) with the
  `moonlight-common-c` submodule (`Catapultam-GMG/moonlight-common-c-mic`).
- Host: `logabell/apollo-microphone` at commit `0affdaa6`, installed on
  CPLT-4A as release `v2026.3.18-mic.1`. Windows, SudoVDA driver 1.10.9.289,
  `headless_mode = enabled`, `capture = ddx`.

Research clones (read-only) are in `/tmp/live-resize/apollo-microphone`
(checked out at `0affdaa6`) and `/tmp/live-resize/SudoVDA` (driver source,
`a4b09fa`). All host file references below are relative to that clone.

## 2. Decisions

| # | Decision | Reason |
|---|----------|--------|
| D1 | New control message `RESIZE_REQUEST` (client to host) and `RESIZE_REFUSED` (host to client) on the existing encrypted control stream. | Both ends already encrypt and dispatch control messages. No new socket. |
| D2 | The host advertises support with the SDP attribute `a=x-ss-general.liveResize:1` in the RTSP DESCRIBE reply. The client enables the hotkey only when it sees this attribute. | Stock hosts drop unknown control messages silently (`control_server_t::call` logs "type [Unknown]"). The user must get a clear message instead. |
| D3 | The host changes the VDD with remove and re-add of the SudoVDA monitor. A plain mode change is tried first, but it only works for sizes that are already in the monitor mode list. See section 4.3. | The SudoVDA driver builds the mode list once when the monitor arrives. There is no IOCTL that adds a mode. |
| D4 | Size rule: round each dimension down to an even number. Keep the current fps and bitrate. Do nothing when the result equals the current stream size. | 4:2:0 encoders and D3D11 NV12 textures need even sizes. Apollo already masks odd sizes (`src/process.cpp:210`). |
| D5 | The host replies only on refusal, with a reason code. The client shows the reason in the status overlay for 5 seconds. The client also shows a message when no new-size frame arrives in 10 seconds. | A success needs no message: the new frames are the confirmation. A timeout protects against a lost message. |
| D6 | The client recreates its decoder and renderer when a decoded frame arrives at the requested size. It uses the existing recreate path (`SDL_RENDER_DEVICE_RESET` handling in `Session::execInternal`). | All renderers get a clean start at the new size. No per-renderer resize code. |
| D7 | The host refuses the request when more than one client session is active, when the capture display is not a SudoVDA monitor, when a resize is in progress, or when the size is out of limits. | One VDD serves all sessions. A resize would change the picture for the other clients. |
| D8 | The hotkey is `Ctrl+Alt+Shift+R`. It works only while a stream is active. | `R` is free. Used keys: Q, Z, X, S, M, C, D, V, L, E, K (`app/streaming/input/input.cpp:86-138`). |
| D9 | In windowed and borderless full-screen (`SDL_WINDOW_FULLSCREEN_DESKTOP`) mode the target size is `SDL_GetWindowSizeInPixels()`. In exclusive full-screen (`SDL_WINDOW_FULLSCREEN`) it is the desktop mode of `SDL_GetWindowDisplayIndex()`. | Spike S3 (2026-10-09, GNOME 50, monitors at 100 % and 125 %): windowed and borderless full-screen report the real physical size (3840x2160 on the 4K monitor). Exclusive full-screen reports the emulated mode that Moonlight set (1920x1080), so the desktop mode is necessary there. The window display index was wrong once in S3, so exclusive full-screen with two monitors can pick the wrong monitor. This is an accepted limit. |
| D10 | The client does not save the new size to the settings. | The window size is transient. The saved resolution stays the start size. |

Decisions that the user must confirm:

- U1. The remove and re-add path has user-visible effects on the host. See
  section 4.3. The user must accept them, because the driver gives no other way
  to get an arbitrary size.
- U2. With more than one client connected, the hotkey refuses (D7). The
  alternative is to let the other clients get a letterboxed picture. The spec
  takes the refusal.

## 3. Wire protocol

### 3.1 Message ids

Known ids in the control stream:

- Client `moonlight-common-c/src/ControlStream.c:203-216`
  (`packetTypesGen7Enc`): `0x0302`, `0x0307`, `0x0301`, `0x0201`, `0x0204`,
  `0x0206`, `0x010b`, `0x0109`, `0x010e`, `0x5500`-`0x5503`.
- Host `src/stream.cpp:57-77` (`packetTypes`): the same plus `0x0305`,
  `0x0200`, `0x0001`, `0x3000`-`0x3002` (Apollo extensions: server command,
  clipboard, file transfer nonce). Artemis uses the same `0x3000`-`0x3002`.

New ids (free on both sides and in Artemis, nonary-vibepollo and
logabell-vibepollo):

| Id | Name | Direction | Payload |
|----|------|-----------|---------|
| `0x3100` | `RESIZE_REQUEST` | client to host | `uint16 width`, `uint16 height`, `uint32 request_id` (little endian, 8 bytes) |
| `0x3101` | `RESIZE_REFUSED` | host to client | `uint16 width`, `uint16 height`, `uint32 request_id`, `uint16 reason` (little endian, 10 bytes) |

`request_id` starts at 1 and increases per request. The client ignores a
refusal whose `request_id` is not the pending one.

Reason codes:

| Code | Name | Client text |
|------|------|-------------|
| 1 | `BUSY` | "Host is busy with a resize" |
| 2 | `NOT_VIRTUAL_DISPLAY` | "Host does not stream a virtual display" |
| 3 | `MULTIPLE_CLIENTS` | "Another client is connected" |
| 4 | `SIZE_LIMIT` | "Host rejected the size WxH" |
| 5 | `DISPLAY_FAILED` | "Host could not change the display" |
| 6 | `ENCODER_FAILED` | "Host encoder rejected the size" |
| 7 | `NOT_SUPPORTED` | "Host does not support live resize" (used by the client itself when D2 is false) |

### 3.2 Encryption path

Client send: a new `LiSendResizeRequest(uint16_t width, uint16_t height,
uint32_t* requestId)` in `ControlStream.c` calls `sendMessageAndForget()`
(`ControlStream.c:846`). With `controlProtocolType 13` that goes to
`sendMessageEnet()` (`ControlStream.c:692`), which wraps the packet in
`NVCTL_ENCRYPTED_PACKET_HEADER` (type `0x0001`) and AES-GCM. Use
`ENET_PACKET_FLAG_RELIABLE` on channel 0 (the same as `LiRequestIdrFrame`).

Host receive: the `IDX_ENCRYPTED` handler (`src/stream.cpp:1067-1130`)
decrypts and re-dispatches to `server->call(type, ...)`. A new
`server->map(packetTypes[IDX_RESIZE_REQUEST], ...)` in
`controlBroadcastThread` (`src/stream.cpp:942`) gets the plain payload.

Host send: build `control_resize_refused_t` with `control_header_v2`, encode
with `encode_control()` (`src/stream.cpp:446`), send with
`control_server.send()`. This is the same pattern as `send_hdr_mode()`
(`src/stream.cpp:909-937`). Only `controlBroadcastThread` may call `send()`,
because ENet is not thread safe. See section 4.6.

Client receive: in `controlReceiveThread` (`ControlStream.c` near line 1267)
add `IDX_RESIZE_REFUSED` to `needsAsyncCallback()` (`ControlStream.c:1023`)
and `queueAsyncCallback()` so the callback runs on the async callback thread,
like rumble. Add `ConnListenerResizeRefused resizeRefused` as the last field
of `CONNECTION_LISTENER_CALLBACKS` (`Limelight.h:491-505`). A `NULL` callback
is allowed.

### 3.3 Support detection

Host: `cmd_describe` in `src/rtsp.cpp:791` writes
`a=x-ss-general.featureFlags`. Add one line after it:
`a=x-ss-general.liveResize:1`. Write it only on Windows and only when
`proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK`.

Client: `RtspConnection.c:1148` parses `x-ss-general.featureFlags` with
`parseSdpAttributeToUInt()`. Parse `x-ss-general.liveResize` the same way
into a new global `LiveResizeSupported`. Expose `bool LiIsLiveResizeSupported(void)`.
The hotkey handler reads it and shows reason 7 when it is false.

The existing feature-flag bit space (`SS_FF_*` in `Limelight-internal.h`,
`platform_caps` in `src/platform/common.h:278-284`) is left alone, because
upstream Sunshine owns it.

## 4. Host changes (Apollo at 0affdaa6)

### 4.1 How video runs today

- `videoThread` (`src/stream.cpp:2032`) calls `video::capture(session->mail,
  session->config.monitor, session)` with a copy of `config_t`.
- All Windows encoders have `PARALLEL_ENCODING` (`src/video.cpp:523, 730,
  836, 907`), so `capture()` always calls `capture_async()`
  (`src/video.cpp:2354`). The sync path is not used.
- `capture_async` loops: wait while `reinit_event` is set, take the current
  display, `make_encode_device(*display, encoder, config)`, raise
  `touch_port` (`make_port(display, config)`, `src/video.cpp:2054`), raise
  `hdr`, then `encode_run(..., config, display, ...)`.
- `encode_run` (`src/video.cpp:1903`) creates the encoder with
  `config.width`/`config.height` (`src/video.cpp:1564` for avcodec,
  `src/nvenc/nvenc_base.cpp:116` for NVENC). The capture size comes from
  `display->width/height`. When they differ, the host scales and letterboxes
  (`src/platform/windows/display_vram.cpp:586-614`). So the encoder output
  size is always `config.width x config.height`. The resize must change
  `config`, not only the display.
- `encode_run` returns when `reinit_event` is set (`src/video.cpp:1989`),
  then `capture_async` loops and picks up the new display.
- `captureThread` (`src/video.cpp:1141`) owns the display. DXGI returns
  `DXGI_ERROR_ACCESS_LOST` on a mode change or monitor removal
  (`src/platform/windows/display_base.cpp:150, 182`), which becomes
  `capture_e::reinit`. The thread then raises `reinit_event`, waits for all
  display references to drop, and loops `refresh_displays()` +
  `reset_display()` until a display opens (`src/video.cpp:1333-1393`). This
  loop has no time limit, so a short gap with no display is safe.
- `refresh_displays()` (`src/video.cpp:1085`) prefers
  `proc::proc.display_name`, then the mapped `config::video.output_name`,
  then index 0.
- A fresh encoder session starts with an IDR frame. `capture_async` raises a
  new `touch_port`, so absolute mouse scaling (`src/input.cpp:461-483`)
  follows the new size with no extra work.

### 4.2 New state

- `session_t::control` gains `safe::mail_raw_t::event_t<resize_result_t>
  resize_queue` (mail name `resize_refused`), drained in
  `controlBroadcastThread` next to `hdr_queue` (`src/stream.cpp:1189-1194`).
- `session_t` gains `std::atomic<bool> resize_in_progress`.
- New session-local mail `MAIL(resize)` in `src/globals.h` with payload
  `std::pair<int,int>`; `capture_async` consumes it.
- `capture_async` keeps `config_t last_good_config` and a flag
  `resize_pending`.

### 4.3 VDD change

Facts from the driver source (`/tmp/live-resize/SudoVDA/Virtual Display
Driver (HDR)/SudoVDA/Driver.cpp`):

- `SudoVDAParseMonitorDescription` (line 1065) and
  `SudoVDAMonitorQueryModes` (line 1280) build the mode list from
  `s_DefaultModes` (line 64) plus the preferred mode scaled by
  `mode_scale_factors` {100, 50, 75, 125, 150} (line 54) at 1x and 2x
  refresh. The list is fixed when the monitor arrives.
- `IOCTL_ADD_VIRTUAL_DISPLAY` (line 1497) returns the existing monitor when
  the GUID is already known (line 1520-1535). A new preferred mode for the
  same GUID needs `IOCTL_REMOVE_VIRTUAL_DISPLAY` first.
- `freeConnectorSlots` is a queue. A removed slot goes to the back, so the
  re-added monitor gets a different connector index (line 1583 and 862).
- The EDID is `edid_base` plus serial, serial string and product name
  (`edid.h:29`). The physical size bytes do not change.

Procedure, in a worker thread:

1. Read the current mode with `VDISPLAY::getDeviceSettings(name, devMode)`
   (`src/platform/windows/virtual_display.h:25`). Keep width, height,
   frequency for the revert.
2. Try `VDISPLAY::changeDisplaySettings(name, w, h, target_fps)`
   (`virtual_display.cpp:265`). Its return value is not usable: after the
   `ChangeDisplaySettingsExW` calls it always returns
   `changeDisplaySettings2(...)` (`virtual_display.cpp:308`). Read the mode
   back with `getDeviceSettings()` and compare width and height. If they
   match, go to step 6. This works only for sizes in the mode list, for
   example when the user goes back to the start size. Spike S1 confirms
   that `changeDisplaySettings2` has no side effect on a rejected mode.
3. `VDISPLAY::removeVirtualDisplay(proc_t::vdd.guid)`
   (`virtual_display.cpp:691`).
4. `VDISPLAY::createVirtualDisplay(vdd.device_uuid, vdd.device_name, w, h,
   vdd.target_fps, vdd.guid)` (`virtual_display.cpp:656`) with the same
   arguments that `proc_t::execute` used (`src/process.cpp:291-298`).
   `_launch_session` is private (`src/process.h:149`), so `proc_t` gets a
   public struct `vdd {device_uuid, device_name, target_fps, guid}` that
   `execute` fills at launch and `terminate` clears. `createVirtualDisplay`
   polls up to about 640 ms for the device name. If it returns an empty
   name, revert (step 7) and refuse with `DISPLAY_FAILED`.
5. `VDISPLAY::changeDisplaySettings(newName, w, h, target_fps)` and, when
   `config::video.isolated_virtual_display_option` is set,
   `changeDisplaySettings2(..., true)`, as in `src/process.cpp:308-317`.
   Set `proc::proc.display_name = to_utf8(newName)` and
   `config::video.output_name = display_device::map_display_name(...)`
   (`src/process.cpp:321-327`). Also update `launch_session->width/height`.
6. Done. The capture thread sees `DXGI_ERROR_ACCESS_LOST` and reinitializes
   on its own.
7. Revert: create the monitor again with the old width, height, frequency,
   and apply the old mode. Clear the pending size (section 4.4).

`target_fps` follows `src/process.cpp:277-287`: take `launch_session->fps`,
multiply by 1000 when below 1000, double it when
`config::video.double_refreshrate` is set. Keep this value in `proc_t` at
launch and reuse it.

User-visible effects of remove and re-add that the user must accept (U1):

- With `headless_mode` the host has no display for up to about one second.
  Windows moves the application windows to a temporary display and back.
  Apollo already does this at every session end and start, so the host
  applications already survive it.
- Windows may treat the re-added monitor as a new monitor instance because
  the connector index changes. The display scale (DPI) may go back to the
  Windows default for the new size. Window positions may not come back.
- The host application gets no new `APOLLO_CLIENT_WIDTH` environment. Only a
  new launch sees the new size in the environment.

### 4.4 Capture and encoder update

Order matters. `capture_async` reads the new size only at the top of its
loop, which it reaches when `encode_run` exits on `reinit_event`.

1. The control handler validates the request and raises `mail::resize` with
   `{w, h}` on the session mail before it starts the VDD worker.
2. `capture_async`, at the top of the loop and after the reinit wait, pops
   `mail::resize` if present: `last_good_config = config; config.width = w;
   config.height = h; resize_pending = true;`. Then `make_encode_device`,
   `make_port` and `encode_run` all use the new size.
3. If the VDD worker fails before the capture thread reinitializes, the
   worker drains the event with `pop(0ms)` so that the next unrelated reinit
   does not apply the size. If the capture thread already consumed it, the
   worker raises `mail::resize` with the old size and the display revert
   (section 4.3 step 7) triggers the reinit that applies it.
4. Encoder failure: `encode_run` returns at once when
   `make_encode_session` returns null (`src/video.cpp:1914`). Today
   `capture_async` then loops without delay and tries again forever. Add a
   check: when `encode_run` returns and neither `shutdown_event` nor
   `reinit_event` is set and `resize_pending` is true, restore
   `config = last_good_config`, queue `ENCODER_FAILED`, and ask the VDD
   worker to revert the display. Clear `resize_pending` when `encode_run`
   has encoded at least one frame at the new size.
5. The new encoder session starts with an IDR. The client also requests an
   IDR after its decoder recreate, so a second IDR is normal.

Limits checked in the control handler before anything changes:

- Width and height are even, at least 320 x 200, and at most 8192 x 8192.
  With H.264 (`config.videoFormat == 0`) the maximum is 4096 x 4096
  (same rule as the client, `Connection.c:322-326`).
- The host cannot ask the encoder for its maximum from the control thread
  (`NV_ENC_CAPS_WIDTH_MAX` needs an open encoder, `nvenc_base.cpp:191`).
  The encoder-failure revert in step 4 covers the remaining cases.

Padding: the frame header of the host has no size fields. The client learns
the new size from the decoded frame (section 5.4).

### 4.5 Refusal conditions

| Reason | Check |
|--------|-------|
| `BUSY` | `session->resize_in_progress` is set, or `resize_pending` in `capture_async` is still set. |
| `NOT_VIRTUAL_DISPLAY` | `!proc::proc.virtual_display` (`src/process.h:111`) or `vDisplayDriverStatus != OK` (`src/process.cpp:63`). |
| `MULTIPLE_CLIENTS` | `rtsp_stream::session_count() > 1` (`src/rtsp.cpp:653`). |
| `SIZE_LIMIT` | Limits in section 4.4. Also when `config.input_only` is set. |
| `DISPLAY_FAILED` | Step 4 or 5 of section 4.3 failed and the revert ran. |
| `ENCODER_FAILED` | Section 4.4 step 4. |

The handler also drops a request whose size equals the current
`config.width x config.height` without a reply. The client does not send
such a request (D4), but the host must be safe.

Permission: the request needs no new `crypto::PERM` bit. The session that
streams the display may resize it. Reviewers may decide to gate it behind
`PERM::server_cmd`; the spec does not require it.

### 4.6 Thread safety

- The control handler runs on `controlBroadcastThread`. It must not block.
  The VDD work takes up to seconds, so it runs in a detached worker thread,
  as `IDX_EXEC_SERVER_CMD` does (`src/stream.cpp:1029-1046`).
- The worker must not keep the raw `session_t*`. It stores the session uuid
  (`stream::session::uuid(session)`, `src/stream.cpp:2086`) and gets a
  `shared_ptr` back with `rtsp_stream::find_session(uuid)`
  (`src/rtsp.cpp:660`) when it needs to queue a result. If the session is
  gone, the worker only reverts the display and exits.
- `proc::proc.display_name` and `config::video.output_name` are plain
  strings read by the capture thread without a lock. `proc_t::execute`
  already writes them while the capture thread may run. The worker follows
  the same pattern. The spike in section 8 checks that the capture thread
  recovers when the name changes.
- `resize_in_progress` is cleared by the worker when it finishes, and by
  `capture_async` when `resize_pending` clears, whichever is later. Use a
  small counter or two flags; the reviewer should check that a crash in the
  worker cannot leave the session blocked. A watchdog clears the flag after
  15 seconds.
- `session::stop` and `proc_t::terminate` while the worker runs: `terminate`
  removes the VDD by GUID (`src/process.cpp:759-767`). If it runs between
  worker step 3 (remove) and step 4 (create), the worker re-adds a monitor
  that nobody owns. The next launch with the same GUID then gets that stale
  monitor back from `IOCTL_ADD_VIRTUAL_DISPLAY` (Driver.cpp line 1520) with
  the old mode list. Fix: `proc_t` gets a `std::mutex vdd_lock`. The worker
  holds it from step 3 to step 5. `terminate` holds it around its
  `removeVirtualDisplay` call and clears `proc_t::vdd`. After the worker
  takes the lock it checks `proc::proc.running()` and that `vdd.guid` is
  still set; if not, it exits without step 4.

### 4.7 headless_mode interaction

With `headless_mode`, `proc_t::execute` always creates the VDD
(`src/process.cpp:237-243`). `proc::proc.virtual_display` is true, so the
feature is active. Without `headless_mode` and without a virtual display app
flag, the capture display is physical and the host refuses with
`NOT_VIRTUAL_DISPLAY`.

## 5. Client changes (moonlight-qt-mic)

### 5.1 moonlight-common-c

- `ControlStream.c`: add `IDX_RESIZE_REQUEST` and `IDX_RESIZE_REFUSED` to
  the index list and to `packetTypesGen7Enc` only. For the other generations
  put `-1`. Add `LiSendResizeRequest()` and the async callback plumbing
  (section 3.2). Check `encryptedControlStream` and `IS_SUNSHINE()`; return
  an error when the control stream is not encrypted.
- `RtspConnection.c`: parse `x-ss-general.liveResize` (section 3.3).
- `Limelight.h`: new callback typedef, new struct field at the end, new
  functions `LiSendResizeRequest`, `LiIsLiveResizeSupported`.
- The stream config in common-c (`StreamConfig.width/height`) is used only
  at start (`VideoStream.c:325`, SDP). It stays at the start size. Nothing in
  the depacketizer depends on it.

### 5.2 Hotkey

- `app/streaming/input/input.h/.cpp`: add `KeyComboResizeToWindow` with
  `SDLK_r` / `SDL_SCANCODE_R` to `m_SpecialKeyCombos`.
- `app/streaming/input/keyboard.cpp` `performSpecialKeyCombo()`: call
  `Session::get()->requestLiveResize()`.

### 5.3 Size computation (`Session::requestLiveResize`)

1. If `!LiIsLiveResizeSupported()`: show reason 7 and return.
2. If a request is pending: show "Resize in progress" and return.
3. Exclusive full screen (`(SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN`):
   use the desktop mode of the window display,
   `StreamUtils::getNativeDesktopMode(SDL_GetWindowDisplayIndex(m_Window), ...)`
   (used in `session.cpp:1416`). Windowed or borderless full screen:
   `SDL_GetWindowSizeInPixels(m_Window, &w, &h)` (see D9 and spike S3).
4. Apply D4: `w &= ~1; h &= ~1;`. If `w == m_ActiveVideoWidth && h ==
   m_ActiveVideoHeight`, return without a message.
5. Store `m_PendingResize = {w, h, requestId}`, start a 10 second timer
   (`SDL_AddTimer` that pushes an `SDL_USEREVENT`), call
   `LiSendResizeRequest(w, h, &requestId)`.

Note on full screen: `app/main.cpp:753` sets
`SDL_VIDEO_WAYLAND_MODE_SCALING=aspect`, and
`Session::updateOptimalWindowDisplayMode()` calls `SDL_SetWindowDisplayMode`.
Under exclusive `SDL_WINDOW_FULLSCREEN` the pixel size that SDL reports may be
the emulated mode and not the native mode. The native-mode query in step 3
avoids this. Spike S3 confirmed this: exclusive full screen reported 1920x1080 (the
emulated mode), and borderless full screen reported 3840x2160 (the real
size).

### 5.4 Decoder: detect the new size

`FFmpegVideoDecoder` (`app/streaming/video/ffmpeg.cpp:1927-1946`) today
treats any frame that is larger than `m_OriginalVideoWidth/Height` by less
than 64 pixels in both dimensions as encoder padding and crops it. A resize of
+10 pixels would be cropped. New rule, in this order, after
`avcodec_receive_frame()` returns a frame. The free function
`LiveResize::classifyFrameSize()` in `app/streaming/liveresize.h` holds the
rule, and `app/tests/liveresize_test.cpp` tests it.

"Padded size of W x H" means larger than W x H by 0 to 63 pixels in both
dimensions. "Expected size" is the size that the session set with
`IVideoDecoder::setExpectedFrameSize(w, h)` (new virtual, called by
`requestLiveResize`; `0, 0` when no request is pending).

1. If the frame size equals the size that the decoder was created with
   (`m_OriginalVideoWidth/Height`): render the frame as today.
2. Else if an expected size is set and the frame size is a padded size of
   the expected size: this is the new stream size. The new stream size is
   the expected size, not the frame size. Thus the new decoder crops the
   encoder padding with the correct base. Example: a request for 2536x1390
   on an encoder that sends 2560x1392 gives the stream size 2536x1390.
   This rule comes before rule 3, so that a request of +10 pixels is not
   cropped to the old size.
3. Else if the frame size is a padded size of the original size: crop as
   today.
4. Else: this is a new stream size too, and the new stream size is the frame
   size. This covers a host that changed size on its own.

For rules 2 and 4: do not render the frame. Free it. Push `SDL_USEREVENT`
with code `SDL_CODE_STREAM_SIZE_CHANGED` and the new stream size in
`data1/data2`. Set `m_DecoderThreadShouldQuit`, as the consistent-failure
path does (`ffmpeg.cpp:2075-2085`).

`SDL_CODE_STREAM_SIZE_CHANGED` (value 106) is defined in
`app/streaming/video/decoder.h`, not in `session.cpp`, because
`ffmpeg.cpp` pushes it.

Why not render the frame with the old renderer: `SdlRenderer` creates its
texture at the first frame size (`sdlvid.cpp:416-420`), and the VAAPI and
EGL renderers use `frame->width/height` for the source rect but keep other
state at the old size. A clean recreate is simpler and covers PlVk, EGL,
VAAPI direct, DRM and SDL alike.

### 5.5 Session: apply the new size

New `SDL_USEREVENT` codes next to `session.cpp:26-31`:
`SDL_CODE_RESIZE_REFUSED` (107), `SDL_CODE_RESIZE_TIMEOUT` (108) and
`SDL_CODE_RESIZE_STATUS_TIMEOUT` (109). `SDL_CODE_STREAM_SIZE_CHANGED` (106)
is in `decoder.h` (section 5.4).

On `SDL_CODE_STREAM_SIZE_CHANGED` in `Session::execInternal`,
`Session::applyStreamSize(w, h)` does these steps:

1. Clear `m_PendingResize`, stop the timer, and set the expected size of
   the old decoder to `0, 0` (`clearPendingResize()`). If a request was
   pending and `w x h` is not the requested size, log a warning.
2. `m_ActiveVideoWidth/Height = w, h` (these feed `chooseDecoder` in the
   recreate path, `session.cpp:2270-2274`). Also `m_StreamConfig.width/height`
   for the bitrate and window helpers that read it.
3. `m_InputHandler->setStreamSize(w, h)` (new setter for `m_StreamWidth/
   m_StreamHeight`, `input.h:245-246`). The absolute mouse and touch paths
   read these on every event (`mouse.cpp:130, 329, 366`, `abstouch.cpp:70,
   175`, `reltouch.cpp:100`).
4. Push `SDL_RENDER_DEVICE_RESET`. The existing handler
   (`session.cpp:2224-2300`) deletes the decoder, flushes window events,
   calls `chooseDecoder` with the new size, and calls `LiRequestIdrFrame()`.

The new decoder does not need `setExpectedFrameSize()`. It is created with
`w x h`, which is the requested size when the encoder adds padding (section
5.4, rule 2). Thus its original size is the correct base for the padding
rule, and it crops the padding.

Overlays: `completeInitialization` calls
`setOverlayRenderer(m_FrontendRenderer)` (`ffmpeg.cpp:727`), so the debug
and status overlays attach to the new renderer with no extra work. The Pacer
is created with the new size and frame rate in the same function
(`ffmpeg.cpp:499-501`).

Window size and aspect: the window is not resized. In windowed mode the
content area already equals the stream size, so the renderers draw without
bars (`StreamUtils::scaleSourceToDestinationSurface` gives a full-window
rect). In full screen the window stays full screen. `getWindowDimensions()`
(`session.cpp:1371`) runs only at start, so a later full-screen toggle keeps
the current window size.

Fullscreen toggle after a resize: `toggleFullscreen()` (`session.cpp:1527`)
and the `SDL_WINDOWEVENT_SIZE_CHANGED` path recreate the decoder with
`m_ActiveVideoWidth/Height`, which now hold the new size. The stream itself
does not change on a toggle. The user presses the hotkey again when the
window size changed.

### 5.6 Refusal and timeout display

- `Session::clResizeRefused(w, h, requestId, reason)` (new static callback in
  `k_ConnCallbacks`, last entry) pushes `SDL_CODE_RESIZE_REFUSED`.
- The main thread maps the reason to the text in section 3.1, calls
  `m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, text)`
  and `setOverlayState(OverlayStatusUpdate, true)`, and hides the overlay
  after 5 seconds with a timer event. `clConnectionStatusUpdate`
  (`session.cpp:173`) uses the same overlay and leaves it on until the
  connection is OKAY. The resize text must not hide a poor-connection
  warning that is on: skip the resize text when `m_MouseEmulationRefCount >
  0` or when the overlay already shows the connection warning. Simple rule:
  keep a `m_StatusOverlayOwner` enum.
- On `SDL_CODE_RESIZE_TIMEOUT` with a pending request: clear the pending
  state and show "Host did not answer the resize request".

### 5.7 Settings

No new settings. The feature is always on when the host advertises it.

## 6. Build and deploy

### 6.1 Host build

Facts:

- The repository `logabell/apollo-microphone` has no GitHub workflow at
  `0affdaa6`. Commit `da5a4e3e` (2025-07-14, "Remove workflows") deleted
  `ci.yml`, `ci-windows.yml` and the others, and it is an ancestor of
  `0affdaa6`. The tag `v2026.3.18-mic.1` points at `99794062` (also no
  workflows). The GitHub Actions history has runs only since 2026-05-22 and
  only on `feature/microphone-passthrough`. That branch is 785 commits ahead,
  builds a WIX installer named `VibepolloSetup.exe`, and is a different
  product line.
- The installed version string `0.0.0.0affdaa.dirty` means a build without
  `BUILD_VERSION` from a tree with local changes
  (`cmake/prep/build_version.cmake:6-27, 46-59`). The release installer was
  not built by GitHub Actions in this repository.
- The release asset is `Apollo-v2026.3.18-mic.1-windows-installer.exe`
  (NSIS, 17430279 bytes, sha256
  `99b6ab6fe1cc010a74304cb0739b1aa0d8dfe321ba571ef6e1d7da33550ab6c7`).
- The NSIS rules (`cmake/packaging/windows_nsis.cmake:10-30`) install the
  SudoVDA driver from `src_assets/windows/drivers/sudovda` on every install.
  That driver is `1.10.9.289` (`SudoVDA.inf:13`), the same as the one on
  CPLT-4A. An upgrade does not change the driver.

Plan:

1. Fork `logabell/apollo-microphone` to `Catapultam-GMG/apollo-microphone`.
   Branch `live-resize` from `0affdaa6`.
2. Restore `.github/workflows/ci-windows.yml` from `da5a4e3e^` as a
   standalone workflow with `on: workflow_dispatch` and `push` on the
   branch. Keep: checkout with submodules, MSYS2 ucrt64 setup, the
   dependency install step, `cmake` with `-DCMAKE_BUILD_TYPE=RelWithDebInfo
   -DSUNSHINE_ASSETS_DIR=assets`, `ninja`, `cpack -G NSIS`, rename to
   `Apollo-<version>-windows-installer.exe`, `actions/upload-artifact`.
   Remove: tests, coverage, Doxygen, debug-info upload, the `usbmmidd`
   test driver. Set `BUILD_VERSION` and `BRANCH` env from the workflow so
   the binary reports a version such as `2026.10.9-liveresize.1`.
3. First run: build unchanged `0affdaa6` with this workflow. This proves the
   toolchain before any code change. The MSYS2 package set from 2025-07 may
   not match the 2026-03 tree; fix the dependency list in the workflow, not
   the code.
4. The artifact `build-Windows-AMD64` contains the installer.

### 6.2 Host install on CPLT-4A

CPLT-4A: Windows, `10.10.10.232`, reachable through agentbus
(`agentbus:cplt-4a`, see `~/GitHub/homelab-notes/agentbus.md`). Install path
`C:\Program Files\Apollo`, service `ApolloService`, config in
`C:\Program Files\Apollo\config\`.

1. Download the current installer from the release page and keep it next to
   the new one. Check its sha256 against the value above.
2. Back up `C:\Program Files\Apollo\config\` (`sunshine.conf`, `apps.json`,
   state and credentials files) to a dated folder.
3. Stop a running stream. Run the new installer. The NSIS upgrade keeps the
   config folder and reinstalls the service.
4. Check `C:\Program Files\Apollo\sunshine.exe --version` and the web UI
   version string.
5. Rollback: run the kept `Apollo-v2026.3.18-mic.1-windows-installer.exe`,
   then restore the config folder if the upgrade changed it.

### 6.3 Client build

In the `moonlight` toolbox:

```
toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && qmake6 moonlight-qt.pro && make release -j$(nproc)'
```

Changes in `moonlight-common-c` need `make -f Makefile.Release clean` in
`app/` when headers changed, because qmake objects do not depend on the
submodule headers in all cases. Check `ldd app/moonlight | grep placebo` after
a rebuild. The launcher `~/.local/bin/moonlight-mic` runs
`~/GitHub/moonlight-qt-mic/app/moonlight`.

Commit the common-c change in the submodule repository
`Catapultam-GMG/moonlight-common-c-mic` first, then update the submodule
pointer in `moonlight-qt-mic`.

## 7. Testing

### 7.1 Automatic tests on Linux

- Host: `tests/unit/test_stream.cpp` exists (googletest). Add tests for a
  pure function `video::validate_resize(config_t, w, h) -> reason` (even,
  limits per codec, no-op detection) and for the payload struct sizes
  (`sizeof(control_resize_request_t) == 4 + 8`). The host test target builds
  on Linux with the repository `tests/CMakeLists.txt`. The Windows workflow in
  section 6.1 does not run tests; run them locally in a Linux build or add a
  Linux job later.
- Client: `moonlight-qt` has no test target. `app/tests/liveresize_test.cpp`
  is a plain `assert` test of the header-only helpers in
  `app/streaming/liveresize.h`: the size rule (`roundDownEven`), the
  refusal texts, the pending request, and the decoder rule of section 5.4
  (`classifyFrameSize()` and the new stream size that it gives).
- common-c: build with `-DUSE_MBEDTLS` off in the toolbox and check that
  `LiSendResizeRequest` returns an error when no connection exists.

### 7.2 Manual end-to-end test

Setup: CPLT-4A with the new host build, laptop with the new client, Desktop
app, windowed mode, HDR off, absolute mouse mode on.

1. Start the stream at the saved size (for example 2560x1600). Check the
   host log for `Virtual Display created`.
2. Resize the window to an odd size, for example 2537x1391 logical pixels
   with scale 1. Press `Ctrl+Alt+Shift+R`.
3. Expect within about 2 seconds: the host log shows the VDD remove and
   create, `Desktop resolution [2536x1390]`, a new encoder at 2536x1390. The
   client log shows `Recreating renderer` and `Video stream is 2536x1390`.
   The picture fills the window without bars and without blur.
4. Move the mouse to the four window corners. The host cursor reaches the
   display corners. Click a desktop icon in the bottom right corner.
5. Press `Ctrl+Alt+Shift+R` again without a window change. Expect no
   message and no host log entry.
6. Toggle full screen (`Ctrl+Alt+Shift+X`). Press the hotkey. Expect the
   native mode of that display, for example 3840x2160.
7. Stop the stream. Start it again. Expect the saved start size (D10).
8. Refusal test: connect a second client (phone with Artemis or a second
   laptop), press the hotkey. Expect "Another client is connected".
9. Stock host test: stream from a host without the change (or a build with
   the SDP line removed). Expect "Host does not support live resize" and no
   control message in the host log.
10. HDR test: repeat steps 1 to 4 with HDR on (PlVk renderer). Check that the
    HDR state survives the recreate (`setHdrMode` is called in the recreate
    path, `session.cpp:2300`).
11. Microphone test: the mic stream must keep running across the resize.
    Speak during step 3 and check the host.
12. Host application test: run a game in windowed mode on the host, resize,
    and check that the game window is still on the VDD after the re-add.

## 8. Risks and spikes

Each spike is a small throwaway test that runs before the main
implementation.

| # | Risk | How to resolve early |
|---|------|----------------------|
| R1 | Remove and re-add of the SudoVDA monitor with the same GUID and a new preferred mode does not give the odd size, or Windows keeps the old mode. | Spike S1 (2026-10-09, CPLT-4A, SudoVDA 1.10.9.289, not elevated, script `%TEMP%\sudovda-s1\s1.ps1`): ADD of a new GUID at 2536x1390@60000 gave that current mode in 97 ms; the mode list (57 entries) contains the requested size, also odd sizes (2537x1391 accepted). REMOVE + ADD of the SAME GUID at a new size took about 120 ms and kept the GDI name, but Windows applied the mode it saved for that monitor identity (2536x1390), not the new size, for at least 6 s. DPI and the other monitor did not change. The watchdog timeout is 3 s. Decision: after each re-add the host calls `changeDisplaySettings(name, w, h, fps)` (as the launch path does). If the current mode is still wrong after that, the host re-adds with a new GUID and updates `display_guid`. S1b (2026-10-09 14:33, run by the Armor-Console session): after a re-add of the same GUID the saved mode is active; `ChangeDisplaySettingsExW(name, w, h, 60, CDS_UPDATEREGISTRY)` then returns 0 and applies 1922x1078 in 286 ms and the odd size 2537x1391 in 124 ms. A mode change WITHOUT a re-add to a size that is not in the current list returns -2 (`DISP_CHANGE_BADMODE`); the list is rebuilt at each ADD and holds the size of that ADD. A new GUID and serial at 2000x1124 is active at that size 136 ms after ADD, with no mode change. Apollo's display did not change at any time. Odd sizes therefore work end to end in the driver; D4 (round down to even) stays, for the encoders. |
| R2 | The capture thread does not find the re-added display because the device name changed, and `refresh_displays` falls back to a physical display. | Spike S1 also runs during a stream. Check that `Desktop resolution [...]` in the log shows the new VDD. If the fallback picks another display, the worker must set `proc::proc.display_name` before the capture thread reinitializes, or the capture thread must wait for the worker (a `mail::resize_display_ready` event). |
| R3 | Windows resets the display scale for the re-added monitor, so the host desktop looks different after each resize. | Measured in S1. If it happens, keep it as a known effect (U1), or test adding the new monitor before removing the old one (new GUID, update `display_guid`); that keeps a display attached but also changes the identity. |
| R4 | The 2025-07 `ci-windows.yml` cannot build the 2026-03 tree. | Resolved by spike S2 (2026-10-09): fork `catapultam/apollo-microphone`, branch `live-resize` = `0affdaa6` + 4 commits: a build-only `.github/workflows/build-windows.yml` (runner's MSYS2 at `C:\msys64` with `release: false`, because `cmake/targets/common.cmake` finds `npm-cli.js` only there), the `moonlight-common-c` submodule URL set to `logabell/moonlight-common-c` (pinned `6a276a66` is on no branch; GitHub serves it by hash), and `#define DATA_SHARDS_MAX 255` in `src/stream.cpp` (the upstream-based mic `moonlight-common-c` does not include an `rs.h` that defines it; the installed host build was a local dirty build with the same gap). Run `37974002618` passed and produced `Apollo.exe` (NSIS installer) and `Apollo.zip`. |
| R5 | The encoder rejects the new size and `capture_async` spins (pre-existing bug made reachable). | Section 4.4 step 4 adds the revert. Test it by sending a size above the H.264 limit with the limit check disabled. |
| R6 | The decoded frame at the new size reaches a renderer before the recreate and crashes `SdlRenderer`. | Section 5.4 drops the frame. Test with `--video-decoder software` and the SDL renderer path forced. |
| R7 | On GNOME Wayland `SDL_GetWindowSizeInPixels` reports a size that differs from the real buffer with fractional scaling, or the full-screen value is the emulated mode. | Resolved by spike S3 (2026-10-09): windowed and borderless full-screen sizes are physical pixels at 100 % and 125 % scale (window size, pixel size and Vulkan drawable size are equal). Exclusive full screen reports the emulated mode; D9 uses the desktop mode there. |
| R8 | A refusal arrives after the timeout, or two requests interleave. | `request_id` matching (section 3.1) and the pending state machine. Unit test the state transitions. |
| R9 | The control stream is not encrypted (old protocol) and the request goes out in plain text. | `LiSendResizeRequest` returns an error unless `encryptedControlStream` is set. The host already drops plain messages on protocol 13 (`src/stream.cpp:571`). |
| R10 | The mic stream (`MicrophoneStream.c`) or audio resets during the host reinit. | The reinit touches only video. Check in the manual test step 11. The memory note warns that `sdlaud.cpp` debug asserts fire when the renderer reinits with the mic on; run the test with a release build and watch for the assert in a debug build. |
| R11 | `proc_t::terminate` removes the VDD by GUID after a failed worker left no monitor. | `removeVirtualDisplay` on a missing GUID returns `STATUS_NOT_FOUND`, which `terminate` already logs as a warning (`src/process.cpp:763-767`). No crash. |
| R12 | The host `headless_mode` gap triggers `allow_encoder_probing()` or an encoder re-probe. | The reinit path does not probe (`src/video.cpp:1333-1393`). Confirm in S1 that no `Probing encoders` log line appears. |
| R13 | A client disconnect during the worker leaves an orphan VDD that the next launch reuses with a stale mode list. | `vdd_lock` and the running check in section 4.6. Test: disconnect the client 100 ms after the hotkey, then start a new session and check the host log for `Virtual Display created` with the new size. |

Spike order: S2 (CI build of unchanged tree) and S1 (VDD re-add during a
stream) in parallel, then S3 (client size values). Write the implementation
plan after the three spikes have results.

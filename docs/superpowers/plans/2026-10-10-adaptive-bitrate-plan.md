# Adaptive Bitrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The stream bitrate follows (a) the target for the current stream size and (b) the bandwidth that the network has: the client lowers the bitrate on sustained congestion and raises it again up to the target; the host changes the running encoder (NVENC in place without an IDR frame, other encoders with a restart).

**Architecture:** The client runs a pure controller (`app/streaming/adaptivebitrate.h`) on the main thread each 250 ms with frame loss, FEC recovery, RTT and the received byte rate. It sends `SET_BITRATE` (0x3102) on the encrypted control stream. The host converts the configured kbps with the same chain as at stream start (`adaptive_bitrate::encoder_bitrate()`), hands the change to the encode thread through a mail, and answers with `BITRATE_STATUS` (0x3103). NVENC applies it with `nvEncReconfigureEncoder`; other encoders restart through the existing `capture_async` loop.

**Tech Stack:** Client: C++17/Qt6/SDL2 (sdl2-compat) in `~/GitHub/moonlight-qt-mic-abr` (worktree of `Catapultam-GMG/moonlight-qt-mic`), C in the submodule `moonlight-common-c/moonlight-common-c` (`Catapultam-GMG/moonlight-common-c-mic`). Host: C++20 Apollo fork in `~/GitHub/apollo-microphone-abr` (worktree of `catapultam/apollo-microphone`), built only by GitHub Actions. Host unit tests: googletest on Linux in the `moonlight` toolbox. NVENC spike: nv-codec-headers dynamic loader, gcc in the toolbox and MinGW for Windows.

**Spec:** `/var/home/catapultam/src/2026-10-10-adaptive-bitrate-design.md` (Task 1 copies it to `docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md` in the client repository). Read sections 2 to 6 before a task. Research notes: `/var/home/catapultam/src/adaptive-bitrate-research.md`. The live resize spec (`docs/superpowers/specs/2026-10-09-live-resize-design.md`, sections 3, 4.2 and 4.6) gives the mail and thread rules that this plan uses.

## Global Constraints

- Base: `master` of each repository after the live resize merge (and the host `ci-updater` merge). Task 1 stops when `master` does not have them.
- Branch `adaptive-bitrate` in all three repositories. Client worktree: `/var/home/catapultam/GitHub/moonlight-qt-mic-abr`. Submodule in it: `moonlight-common-c/moonlight-common-c` (remote `origin` = `Catapultam-GMG/moonlight-common-c-mic`). Host worktree: `/var/home/catapultam/GitHub/apollo-microphone-abr`.
- Client build: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && qmake6 moonlight-qt.pro && make release -j$(nproc)'`. After a change of a submodule header, first run `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr/app && make -f Makefile.Release clean'`. After each build: `ldd ~/GitHub/moonlight-qt-mic-abr/app/moonlight | grep placebo` must print a line.
- Client tests: plain `assert` programs, `g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/<name>.cpp -o /tmp/<name> && /tmp/<name>` in the toolbox (no `-DNDEBUG`).
- common-c tests: build the library with `cmake -S . -B /tmp/mcc-build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug && cmake --build /tmp/mcc-build -j` in the submodule, then `gcc -O0 -g -I src -I enet/include test/<name>.c /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a -lcrypto -lpthread -o /tmp/<name> && /tmp/<name>` (all in the toolbox).
- Host tests: `g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/<name>.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/<name> && /tmp/<name>` in the toolbox, in the host worktree. The toolbox has no boost, no nlohmann and no system gtest: host logic that a test reads must be in a pure header (`src/adaptive_bitrate.h`). Task 1 builds `/tmp/gtest-build`; rebuild it with the same step when `/tmp` was cleared.
- Host compile check (no Windows toolchain exists): note the newest run id of the branch, `gh workflow run build-windows.yml -R catapultam/apollo-microphone --ref adaptive-bitrate`, poll `gh run list -R catapultam/apollo-microphone -w build-windows.yml -b adaptive-bitrate -L 1 --json databaseId -q '.[0].databaseId'` until it gives a new id (a loop without `sleep`, see Task 4 Step 6), then `gh run watch <id> -R catapultam/apollo-microphone --exit-status` (about 12 minutes). The command harness blocks a foreground `sleep` and stops `&` jobs: wait with Monitor, and start long programs with the Bash option `run_in_background: true`. A push starts the workflow only on `master` and `live-resize`; a manual run also publishes a signed prerelease `build-<n>-<sha>` with the body line `branch: adaptive-bitrate`. The updater on CPLT-4A follows `master` and ignores it.
- Host deploy (Task 10): merge `adaptive-bitrate` into host `master`, push, the workflow publishes a signed release, then the agentbus session `Armor-Console` on `cplt-4a` runs `schtasks /run /tn ApolloUpdate` while no stream runs. Fallback: the manual installer, with the user who approves UAC from a stream. Updater facts: `scripts/updater/README.md` in the host repository.
- Host `min_log_level = 3` (warning) on CPLT-4A: info lines do not show. Verify host behavior through the client log (status code per request, key frame count after an in-place change) and the host warning lines of this plan.
- Wire protocol (spec 3.1): `SET_BITRATE` `0x3102` client to host, payload `uint32 request_id, uint32 configured_kbps` (8 bytes). `BITRATE_STATUS` `0x3103` host to client, payload `uint32 request_id, uint32 requested_kbps, uint32 accepted_kbps, uint32 encoder_kbps, uint16 status` (18 bytes). Little endian. `request_id` starts at 1, separate from the resize counter.
- Status codes: 0 `APPLIED`, 1 `APPLIED_RESTART`, 2 `UNCHANGED`, 3 `NOT_SUPPORTED`, 4 `INVALID`, 5 `ENCODER_FAILED`, 6 `INPUT_ONLY`. On 3 to 6, `accepted_kbps` and `encoder_kbps` are the values that run now.
- SDP attribute: `a=x-ss-general.dynamicBitrate:1` in the DESCRIBE reply when `video::encoder_supports_live_resize()` is true (it tests `PARALLEL_ENCODING`). All platforms.
- Indexes: client `IDX_SET_BITRATE 15`, `IDX_BITRATE_STATUS 16`; host `IDX_SET_BITRATE 21`, `IDX_BITRATE_STATUS 22`; SDL user event codes `SDL_CODE_BITRATE_TICK 120`, `SDL_CODE_BITRATE_STATUS 121`. Task 1 checks that these are free.
- Host limits: `500 <= configured_kbps <= 1000000`, else `INVALID`. Host minimum interval between encoder restarts: 2 s. Host in-flight watchdog: 5 s.
- The value on the wire is the configured bitrate (the unit of `x-ml-video.configuredBitrateKbps`). The client never applies the 0.8 factor; the host makes all reductions.
- Setting (spec 6.4): `bool adaptiveBitrate`, key `adaptivebitrate`, default `true`. Check box text "Adapt bitrate to the network". Tool tip "Lower the bitrate when the network is congested and raise it again up to the selected bitrate." Command line `--adaptive-bitrate` / `--no-adaptive-bitrate` ("network adaptive bitrate").
- Overlay line: `Bitrate: target 42.0 Mbps (limit 80.0), host encoder 33.4 Mbps, measured 31.2 Mbps`; without a running controller: `Bitrate: target 44.0 Mbps (fixed), host encoder N/A, measured 31.2 Mbps`. Poor connection text when the controller runs: `Poor connection to PC` (U5).
- Threads: on the host only `controlBroadcastThread` sends on ENet. On the client only the main thread calls `LiSendBitrateRequest()` and the controller.
- All prose, code comments, log texts and commit messages in ASD-STE100 Simplified Technical English. Commit messages never contain AI attribution (no `Co-Authored-By`, no "Generated with").
- Spec deviations that this plan makes (each has its reason in the task that owns it):
  1. `RtpVideoQueue.c` never counts recovered video packets (`stats.packetCountFecRecovered` stays 0; only `RtpAudioQueue.c` counts). Task 6 adds the count, else the FEC rule cannot fire.
  2. The frame counters are "finished frames" and "lost frames" (`LiGetVideoFrameCounters(finished, lost)`), not "total" and "received". Two counters that two moments update give a false loss of one frame at a tick boundary (3 % of a 1 s window at 30 fps). `Sample` drops `packetsFecFailed` and `rttVarianceMs` (no rule uses them).
  3. The history has 12 deltas (3 s), not 8: rule 2 needs three 1 s windows.
  4. A window shows delay when 3 of its 4 RTT samples are above the threshold. Spec tests 4 (spike: no change) and 5 (rise: decrease) need this difference.
  5. Sustained loss also needs at least 3 ticks with loss in the 3 s history, so a stall that spans a window boundary is not "two lossy windows" (D8).
  6. A window with fewer than 10 frames, and a 2 s FEC window with fewer than 200 video packets, give no signal (a static desktop sends few frames).
  7. One decrease keeps at least half of the target (the measured cut of rule 2 can go far below `DEC_LOSS` on an app-limited picture).
  8. The start request is sent at most 3 times without an answer, then the controller stops for the session.
  9. After a delay decrease, when the RTT does not fall by half the rise, the controller sets the RTT baseline to the current RTT instead of a second decrease. Without this a constant added delay gives a decrease each 3 s for 30 s (spec 8.3 step 5 expects one at most).
  10. The increase step is at least 250 kbps, so the dead band never blocks an increase.
  11. Host `bitrate_result` is a mail queue, not an event: an event keeps one value and a second raise before the control thread reads would lose an answer.
  12. Host payload bytes are written and read by `adaptive_bitrate::encode_status()` / `decode_request()` (explicit shifts), not `util::endian::little()`: the test must build without `utility.h` (it includes nlohmann).
  13. The host does not release a bitrate change while a live resize is in progress or its display thread runs: `capture_async` takes a pending resize size at the top of every loop, and a bitrate restart is a loop without a display change.
  14. The client status callback data goes through a mutex-protected vector, not a heap struct per SDL event: events that are left in the queue at session end would leak.
  15. `--bitrate` on the command line also sets `autoAdjustBitrate = false`, so the manual value is the ceiling (D11).
  16. Spike S2 (the controller in "log only" mode against the old host) is not a separate task: the user wants one feature, and Task 11 runs the same `tc` steps against the full feature. The code reading of the planning step found the S2 risk that mattered (deviation 1).

- Planning checks (2026-10-09, in scratch copies under `/tmp/abr-verify`, nothing committed): the code of Tasks 3, 6 and 7 compiles as written and its tests pass (16 googletests, the common-c test, the controller test); the client with the code of Tasks 6 to 9 builds with no new warning; the spike of Task 2 gave outcome A on the RTX 5070. The host code of Tasks 4 and 5 needs boost and the Windows toolchain and is checked only by CI.

## Review Focus

1. A bitrate restart that the host releases while a live resize is in progress: `capture_async` would take the new size before the display changes. Expected: the host keeps the change pending until the resize ends. Pinned by `AdaptiveBitrateState.ReleaseWaitsForLiveResize` in Task 3.
2. A host that never answers the start request: expected, the client stops after 3 attempts and does not send forever. Pinned by `testStartNoAnswer` in Task 7.
3. Sustained loss on a static picture (the measured bitrate is a small part of the encoder bitrate): expected, one decrease goes to half of the target at most, not to the floor. Pinned by `testMaxCut` in Task 7.
4. A Wi-Fi stall of about 120 ms that falls on two ticks across a window boundary: expected, no decrease (D8). Pinned by `testStallAcrossWindows` in Task 7.
5. A bad host: a short `BITRATE_STATUS` payload, or `accepted_kbps` above `requested_kbps`. Expected: the client drops the short message and never raises its ceiling. Pinned by the parse checks in Task 6 and `testHostCap` in Task 7.

---

## File Structure

Client (`~/GitHub/moonlight-qt-mic-abr`):

- `moonlight-common-c/moonlight-common-c/src/ControlStream.c`: message ids and tables, `LiSendBitrateRequest()`, status parse and async callback, finished and lost frame counters, `LiGetVideoFrameCounters()`.
- `moonlight-common-c/moonlight-common-c/src/RtpVideoQueue.c`: count recovered video packets.
- `moonlight-common-c/moonlight-common-c/src/RtspConnection.c`, `Connection.c`, `Misc.c`, `FakeCallbacks.c`, `Limelight.h`, `Limelight-internal.h`: SDP flag, API, callback.
- `moonlight-common-c/moonlight-common-c/test/bitrate_protocol_test.c` (new): C test.
- `app/streaming/adaptivebitrate.h` (new): the controller and its log and overlay texts. No SDL, Qt or common-c calls.
- `app/tests/adaptivebitrate_test.cpp` (new): controller test.
- `app/settings/streamingpreferences.h`, `.cpp`, `app/gui/SettingsView.qml`, `app/cli/commandlineparser.cpp`: the setting.
- `app/streaming/session.h`, `session.cpp`: timer, status, ceiling, resize hook, overlay, logs.
- `app/streaming/video/ffmpeg.cpp`: overlay line, log buffer size.
- `app/app.pro`: new header.
- `docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md`, `docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md` (copies).

Host (`~/GitHub/apollo-microphone-abr`):

- `src/adaptive_bitrate.h` (new): ids, status codes, payload codec, bitrate chain, request check, session state machine.
- `tests/unit/test_adaptive_bitrate.cpp` (new): googletest.
- `src/globals.h`: mails `bitrate`, `bitrate_result`.
- `src/video.h`, `src/video.cpp`: `encode_session_t::set_bitrate()`, `encode_run(config_t &)`, in-place change and restart path.
- `src/nvenc/nvenc_base.h`, `.cpp`: saved parameters, `vbv_size()`, `reconfigure_bitrate()`.
- `src/stream.h`, `src/stream.cpp`: chain input in `stream::config_t`, ids, session state, handler, status send, control loop.
- `src/rtsp.cpp`: chain call in `cmd_announce`, SDP attribute.
- `cmake/compile_definitions/common.cmake`: new header.

Outside the repositories (not committed): `/var/home/catapultam/src/abr-spike/` (NVENC spike program, network shaping script).

---

### Task 1: Workspace, preconditions and id checks

**Files:**
- Create: worktrees `~/GitHub/moonlight-qt-mic-abr` and `~/GitHub/apollo-microphone-abr`, branch `adaptive-bitrate` in the client, the submodule and the host.
- Create: `~/GitHub/moonlight-qt-mic-abr/docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md`, `~/GitHub/moonlight-qt-mic-abr/docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md` (copies)
- Create: `/tmp/gtest-build/libgtest.a`, `/tmp/gtest-build/libgtest_main.a`

**Interfaces:**
- Consumes: nothing.
- Produces: the three branches and two worktrees that every later task uses; confirmed free ids (Global Constraints); the googletest libraries; the nv-codec-headers submodule in the host worktree (Task 2 uses `third-party/nv-codec-headers/include`).

- [ ] **Step 1: Check that master has the live resize and updater work**

```bash
cd ~/GitHub/moonlight-qt-mic && git fetch origin && git status --short --branch | head -3
git show origin/master:app/streaming/liveresize.h | grep -c "class ResizeController"
git -C moonlight-common-c/moonlight-common-c fetch origin
git -C moonlight-common-c/moonlight-common-c show origin/master:src/Limelight.h | grep -c "LiSendResizeRequest"
cd ~/GitHub/apollo-microphone && git fetch origin
git show origin/master:src/live_resize.h | grep -c "PACKET_TYPE_REQUEST = 0x3100"
git show origin/master:scripts/updater/README.md | grep -c "schtasks /run /tn ApolloUpdate"
for c in 9a02cc63; do git -C ~/GitHub/moonlight-qt-mic merge-base --is-ancestor $c origin/master && echo "client $c in master"; done
for c in d4547e06 e888c7a8; do git merge-base --is-ancestor $c origin/master && echo "host $c in master"; done
```

Expected: each `grep -c` prints `1` or more. The `merge-base` lines are information only (a squash merge changes the SHAs). When a `grep -c` prints `0`, stop and tell the user that the live resize or updater merge is not in `master`.

- [ ] **Step 2: Make the client worktree, the submodule branch and the client branch**

```bash
cd ~/GitHub/moonlight-qt-mic
git worktree add -b adaptive-bitrate ~/GitHub/moonlight-qt-mic-abr origin/master
cd ~/GitHub/moonlight-qt-mic-abr
git submodule update --init --recursive
cd moonlight-common-c/moonlight-common-c
git remote get-url origin
git fetch origin
test "$(git rev-parse HEAD)" = "$(git -C ../.. ls-tree HEAD moonlight-common-c/moonlight-common-c | awk '{print $3}')" && echo "submodule at pointer"
git checkout -b adaptive-bitrate
```

Expected: the remote URL contains `Catapultam-GMG/moonlight-common-c-mic`; `submodule at pointer`; the branch `adaptive-bitrate` exists in the client and in the submodule.

- [ ] **Step 3: Make the host worktree and its submodules**

```bash
cd ~/GitHub/apollo-microphone
git worktree add -b adaptive-bitrate ~/GitHub/apollo-microphone-abr origin/master
cd ~/GitHub/apollo-microphone-abr
git submodule update --init third-party/googletest third-party/nv-codec-headers
ls third-party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h third-party/googletest/googletest/include/gtest/gtest.h
grep -n "define NV_ENC_RECONFIGURE_PARAMS_VER\|define NVENCAPI_MAJOR_VERSION" third-party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h
```

Expected: both files exist. The header says major version 12 and `NV_ENC_RECONFIGURE_PARAMS_VER (NVENCAPI_STRUCT_VERSION(1) | ( 1u<<31 ))`. Struct version 1 is also the SDK 11 value, so `min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER)` needs no override (spec R2). If the version differs, record it; Task 2 and Task 4 then pass it as the v12 override.

- [ ] **Step 4: Check that the ids, indexes and event codes are free**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
grep -rn "0x3102\|0x3103" app moonlight-common-c/moonlight-common-c/src || echo "client ids free"
grep -n "#define IDX_" moonlight-common-c/moonlight-common-c/src/ControlStream.c | tail -3
grep -rn "define SDL_CODE_" app | sort -t' ' -k3 -n | tail -4
cd ~/GitHub/apollo-microphone-abr
grep -rn "0x3102\|0x3103\|dynamicBitrate" src || echo "host ids free"
grep -n "#define IDX_" src/stream.cpp | tail -3
mkdir -p /tmp/abr-ids && cd /tmp/abr-ids
for r in ClassicOldSong/moonlight-android ClassicOldSong/Apollo Nonary/Vibepollo logabell/apollo-microphone; do
  d=$(echo "$r" | tr '/' '_'); [ -d "$d" ] || git clone -q --depth 1 "https://github.com/$r" "$d"
done
grep -rniE "0x310[23]|x-ss-general\.dynamicBitrate" /tmp/abr-ids --include=*.c --include=*.cpp --include=*.h --include=*.java --include=*.kt || echo "fork ids free"
```

Expected: `client ids free`, `host ids free`, `fork ids free`. The last client index is `IDX_RESIZE_REFUSED 14`, the last host index is `IDX_RESIZE_REFUSED 20`, and the highest SDL code is `111`. If one of these differs, stop and ask the user before Task 3; the new values go into Global Constraints.

- [ ] **Step 5: Build googletest for the host tests**

```bash
toolbox run -c moonlight bash -lc 'G=~/GitHub/apollo-microphone-abr/third-party/googletest/googletest; mkdir -p /tmp/gtest-build && cd /tmp/gtest-build && g++ -std=c++20 -O1 -I $G/include -I $G -c $G/src/gtest-all.cc -o gtest-all.o && g++ -std=c++20 -O1 -I $G/include -I $G -c $G/src/gtest_main.cc -o gtest_main.o && ar rcs libgtest.a gtest-all.o && ar rcs libgtest_main.a gtest_main.o && ls -la libgtest.a libgtest_main.a'
toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone-abr && g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/test_live_resize.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/test_live_resize && /tmp/test_live_resize | tail -1'
```

Expected: both libraries exist; the live resize test prints `[  PASSED  ]`.

- [ ] **Step 6: Copy the spec and the plan into the client repository and commit**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
mkdir -p docs/superpowers/specs docs/superpowers/plans
cp /var/home/catapultam/src/2026-10-10-adaptive-bitrate-design.md docs/superpowers/specs/
cp /var/home/catapultam/src/2026-10-10-adaptive-bitrate-plan.md docs/superpowers/plans/
git add docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md
git commit -m "Add the adaptive bitrate spec and implementation plan"
```

Expected: one commit on `adaptive-bitrate`. Later tasks edit the copy in the repository, not the file in `~/src`.

---

### Task 2: Spike S1: NVENC bitrate change without an IDR frame

The spike answers spec R1 before any host code: does `nvEncReconfigureEncoder` change `averageBitRate` (and `vbvBufferSize`) of a running CBR encoder with the Apollo settings, with no IDR frame? It runs on the laptop RTX 5070 (Linux, CUDA) and on the CPLT-4A RTX 4090 (Windows, CUDA). The RTX 4090 result decides. Task 4 also checks the real host path on the RTX 4090 (host warning line and client key frame count), and Task 11 checks it end to end.

**Files:**
- Create: `/var/home/catapultam/src/abr-spike/nvenc_reconfigure_spike.cpp` (not committed)
- Modify: this plan, section "S1 result" at the end of this task (record the outcome)

**Interfaces:**
- Consumes: `~/GitHub/apollo-microphone-abr/third-party/nv-codec-headers/include` (Task 1).
- Produces: the outcome A, B or C. Task 4 Step 5 reads it:
  - A: `averageBitRate` and `vbvBufferSize` change in place, no IDR, frame sizes follow. Task 4 as written.
  - B: the change with `vbvBufferSize` fails or misbehaves, `averageBitRate` alone passes up to the start bitrate. Task 4 sets `RECONFIGURE_VBV = false` (the VBV size stays at the start value, spec R1 "keep vbvBufferSize for the ceiling"). Note: with B, a ceiling above the start bitrate (a live resize to a larger size) gets frames that the start VBV size limits.
  - C: both fail, or an IDR frame follows a change. Task 4 does not add the NVENC override; NVENC uses the restart path (U4 then applies to all encoders).

- [ ] **Step 1: Write the spike program**

Create `/var/home/catapultam/src/abr-spike/nvenc_reconfigure_spike.cpp`:

```cpp
// Spike S1 of the adaptive bitrate design (spec section 9, R1 and R2).
// Question: does NvEncReconfigureEncoder() change the bitrate of a running CBR
// encoder with the encoder settings of Apollo (src/nvenc/nvenc_base.cpp), with
// no IDR frame? Mode "avg+vbv" changes averageBitRate and vbvBufferSize, mode
// "avg" changes only averageBitRate (VBV size of the start). Each segment after a change is compared with
// a new encoder that starts at the same bitrate (the reference): a change in place
// must give the same frame sizes as a new encoder.
// Linux (toolbox):  g++ -std=c++17 -O2 -Wall -I <nv-codec-headers>/include nvenc_reconfigure_spike.cpp -ldl -o /tmp/nvenc_reconfigure_spike
// Windows (MinGW):  x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I <nv-codec-headers>/include nvenc_reconfigure_spike.cpp -o nvenc_reconfigure_spike.exe
#include <ffnvcodec/dynlink_loader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define MAKE_NVENC_VER(major, minor) ((major) | ((minor) << 24))

static const int WIDTH = 2560;
static const int HEIGHT = 1440;
static const int FPS = 60;
static const int SEGMENT_FRAMES = 120;  // 2 s at each bitrate
static const int SETTLE_FRAMES = 30;  // frames after a change that the size mean skips
static const uint32_t RATES_KBPS[] = {40000, 10000, 60000, 20000, 40000};
static const int RATE_COUNT = sizeof(RATES_KBPS) / sizeof(RATES_KBPS[0]);

static uint32_t g_min_api;

// Copy of nvenc_base::min_struct_version()
static uint32_t msv(uint32_t version, uint32_t v11 = 0, uint32_t v12 = 0) {
  version &= ~NVENCAPI_VERSION;
  version |= g_min_api;
  if (v11 || v12) {
    version &= ~(0xFFu << 16);
    version |= (((g_min_api & 0xFF) >= 12) ? v12 : v11) << 16;
  }
  return version;
}

static uint64_t g_rng;

static uint32_t next_random() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (uint32_t) g_rng;
}

// NV12 picture. Each frame replaces 1/16 of the 32x32 blocks with a flat color and
// noise of +-16, so a CBR encoder can use 10 Mbps and 60 Mbps.
struct picture_t {
  std::vector<uint8_t> data = std::vector<uint8_t>((size_t) WIDTH * HEIGHT * 3 / 2, 128);

  void next_frame() {
    const int blocks_x = WIDTH / 32, blocks_y = HEIGHT / 32;
    for (int i = 0; i < blocks_x * blocks_y / 16; i++) {
      const int bx = next_random() % blocks_x, by = next_random() % blocks_y;
      const int base = 32 + next_random() % 192;
      for (int y = 0; y < 32; y++) {
        uint8_t *row = &data[(size_t) (by * 32 + y) * WIDTH + bx * 32];
        for (int x = 0; x < 32; x++) {
          row[x] = (uint8_t) (base + (int) (next_random() % 33) - 16);
        }
      }
      for (int y = 0; y < 16; y++) {
        uint8_t *row = &data[(size_t) WIDTH * HEIGHT + (size_t) (by * 16 + y) * WIDTH + bx * 32];
        for (int x = 0; x < 32; x++) {
          row[x] = (uint8_t) (64 + next_random() % 128);
        }
      }
    }
  }
};

struct segment_result_t {
  uint32_t kbps;
  bool reconfigure_ok;
  int idr_frames;  // IDR frames in the segment (the first frame of the stream is not counted)
  double mean_kb;  // mean frame size after SETTLE_FRAMES
};

struct nvenc_t {
  CudaFunctions *cu = nullptr;
  NvencFunctions *nv = nullptr;
  NV_ENCODE_API_FUNCTION_LIST api {};
  CUcontext ctx = nullptr;
};

static const char *codec_name(int codec) {
  return codec == 0 ? "H.264" : codec == 1 ? "HEVC" : "AV1";
}

// Encodes one segment for each rate and changes the bitrate in place at each segment
// start after the first. The same random seed gives the same pictures in each call.
// Returns false when the encoder could not start (the codec is then skipped).
static bool run_codec(nvenc_t &n, int codec, bool change_vbv, const uint32_t *rates, int rate_count, std::vector<segment_result_t> &results) {
  g_min_api = codec <= 1 ? MAKE_NVENC_VER(11U, 0U) : MAKE_NVENC_VER(12U, 0U);
  g_rng = 88172645463325252ull;

  // One function list for each API version, as nvenc_d3d11::init_library() does
  n.api = {};
  n.api.version = msv(NV_ENCODE_API_FUNCTION_LIST_VER);
  if (n.nv->NvEncodeAPICreateInstance(&n.api) != NV_ENC_SUCCESS) {
    std::printf("SKIP %s: NvEncodeAPICreateInstance failed\n", codec_name(codec));
    return false;
  }

  void *enc = nullptr;
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session_params = {msv(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER)};
  session_params.device = n.ctx;
  session_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  session_params.apiVersion = g_min_api;
  if (n.api.nvEncOpenEncodeSessionEx(&session_params, &enc) != NV_ENC_SUCCESS) {
    std::printf("SKIP %s: nvEncOpenEncodeSessionEx failed\n", codec_name(codec));
    return false;
  }

  NV_ENC_INITIALIZE_PARAMS init_params = {msv(NV_ENC_INITIALIZE_PARAMS_VER)};
  init_params.encodeGUID = codec == 0 ? NV_ENC_CODEC_H264_GUID : codec == 1 ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_AV1_GUID;
  init_params.presetGUID = NV_ENC_PRESET_P1_GUID;
  init_params.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init_params.enablePTD = 1;
  init_params.enableEncodeAsync = 0;
  init_params.encodeWidth = init_params.darWidth = WIDTH;
  init_params.encodeHeight = init_params.darHeight = HEIGHT;
  init_params.frameRateNum = FPS;
  init_params.frameRateDen = 1;

  NV_ENC_PRESET_CONFIG preset_config = {msv(NV_ENC_PRESET_CONFIG_VER), {msv(NV_ENC_CONFIG_VER, 7, 8)}};
  if (n.api.nvEncGetEncodePresetConfigEx(enc, init_params.encodeGUID, init_params.presetGUID, init_params.tuningInfo, &preset_config) != NV_ENC_SUCCESS) {
    std::printf("SKIP %s: nvEncGetEncodePresetConfigEx failed\n", codec_name(codec));
    n.api.nvEncDestroyEncoder(enc);
    return false;
  }

  NV_ENC_CAPS_PARAM caps_param = {msv(NV_ENC_CAPS_PARAM_VER), NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE};
  int custom_vbv = 0;
  n.api.nvEncGetEncodeCaps(enc, init_params.encodeGUID, &caps_param, &custom_vbv);

  // The settings of nvenc_base::create_encoder() with the Apollo defaults
  NV_ENC_CONFIG enc_config = preset_config.presetCfg;
  enc_config.profileGUID = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
  enc_config.gopLength = NVENC_INFINITE_GOPLENGTH;
  enc_config.frameIntervalP = 1;
  enc_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  enc_config.rcParams.zeroReorderDelay = 1;
  enc_config.rcParams.enableLookahead = 0;
  enc_config.rcParams.lowDelayKeyFrameScale = 1;
  enc_config.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
  enc_config.rcParams.averageBitRate = rates[0] * 1000;
  if (custom_vbv) {
    enc_config.rcParams.vbvBufferSize = rates[0] * 1000 / FPS;
  }
  if (codec == 0) {
    enc_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
    enc_config.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    enc_config.encodeCodecConfig.h264Config.sliceMode = 3;
    enc_config.encodeCodecConfig.h264Config.sliceModeData = 1;
  } else if (codec == 1) {
    enc_config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
    enc_config.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
    enc_config.encodeCodecConfig.hevcConfig.sliceMode = 3;
    enc_config.encodeCodecConfig.hevcConfig.sliceModeData = 1;
  } else {
    enc_config.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    enc_config.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  }
  init_params.encodeConfig = &enc_config;

  if (n.api.nvEncInitializeEncoder(enc, &init_params) != NV_ENC_SUCCESS) {
    std::printf("SKIP %s: nvEncInitializeEncoder failed\n", codec_name(codec));
    n.api.nvEncDestroyEncoder(enc);
    return false;
  }

  NV_ENC_CREATE_INPUT_BUFFER input = {msv(NV_ENC_CREATE_INPUT_BUFFER_VER)};
  input.width = WIDTH;
  input.height = HEIGHT;
  input.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  NV_ENC_CREATE_BITSTREAM_BUFFER output = {msv(NV_ENC_CREATE_BITSTREAM_BUFFER_VER)};
  if (n.api.nvEncCreateInputBuffer(enc, &input) != NV_ENC_SUCCESS || n.api.nvEncCreateBitstreamBuffer(enc, &output) != NV_ENC_SUCCESS) {
    std::printf("SKIP %s: buffer allocation failed\n", codec_name(codec));
    n.api.nvEncDestroyEncoder(enc);
    return false;
  }

  picture_t picture;
  uint64_t frame_index = 0;
  bool ok = true;
  for (int segment = 0; segment < rate_count && ok; segment++) {
    segment_result_t result {rates[segment], true, 0, 0};

    if (segment > 0) {
      // The change of nvenc_base::reconfigure_bitrate() (plan Task 4)
      NV_ENC_CONFIG new_config = enc_config;
      new_config.rcParams.averageBitRate = rates[segment] * 1000;
      if (custom_vbv && change_vbv) {
        new_config.rcParams.vbvBufferSize = rates[segment] * 1000 / FPS;
      }
      NV_ENC_RECONFIGURE_PARAMS params = {msv(NV_ENC_RECONFIGURE_PARAMS_VER)};
      params.reInitEncodeParams = init_params;
      params.reInitEncodeParams.encodeConfig = &new_config;
      params.resetEncoder = 0;
      params.forceIDR = 0;
      NVENCSTATUS status = n.api.nvEncReconfigureEncoder(enc, &params);
      result.reconfigure_ok = status == NV_ENC_SUCCESS;
      if (result.reconfigure_ok) {
        enc_config = new_config;
      } else {
        std::printf("%s: nvEncReconfigureEncoder to %u kbps failed with status %d\n", codec_name(codec), rates[segment], (int) status);
      }
    }

    double bytes = 0;
    int counted = 0;
    for (int f = 0; f < SEGMENT_FRAMES; f++, frame_index++) {
      picture.next_frame();
      NV_ENC_LOCK_INPUT_BUFFER lock_input = {msv(NV_ENC_LOCK_INPUT_BUFFER_VER)};
      lock_input.inputBuffer = input.inputBuffer;
      if (n.api.nvEncLockInputBuffer(enc, &lock_input) != NV_ENC_SUCCESS) {
        std::printf("FAIL %s: nvEncLockInputBuffer\n", codec_name(codec));
        ok = false;
        break;
      }
      for (int y = 0; y < HEIGHT * 3 / 2; y++) {
        std::memcpy((uint8_t *) lock_input.bufferDataPtr + (size_t) y * lock_input.pitch, &picture.data[(size_t) y * WIDTH], WIDTH);
      }
      n.api.nvEncUnlockInputBuffer(enc, input.inputBuffer);

      NV_ENC_PIC_PARAMS pic = {msv(NV_ENC_PIC_PARAMS_VER, 4, 6)};
      pic.inputWidth = WIDTH;
      pic.inputHeight = HEIGHT;
      pic.inputPitch = lock_input.pitch;
      pic.inputBuffer = input.inputBuffer;
      pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
      pic.outputBitstream = output.bitstreamBuffer;
      pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
      pic.inputTimeStamp = frame_index;
      if (n.api.nvEncEncodePicture(enc, &pic) != NV_ENC_SUCCESS) {
        std::printf("FAIL %s: nvEncEncodePicture\n", codec_name(codec));
        ok = false;
        break;
      }

      NV_ENC_LOCK_BITSTREAM lock_bitstream = {msv(NV_ENC_LOCK_BITSTREAM_VER, 1, 2)};
      lock_bitstream.outputBitstream = output.bitstreamBuffer;
      if (n.api.nvEncLockBitstream(enc, &lock_bitstream) != NV_ENC_SUCCESS) {
        std::printf("FAIL %s: nvEncLockBitstream\n", codec_name(codec));
        ok = false;
        break;
      }
      const bool idr = lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR || lock_bitstream.pictureType == NV_ENC_PIC_TYPE_I;
      if (idr && frame_index != 0) {
        result.idr_frames++;
      }
      if (f >= SETTLE_FRAMES) {
        bytes += lock_bitstream.bitstreamSizeInBytes;
        counted++;
      }
      n.api.nvEncUnlockBitstream(enc, output.bitstreamBuffer);
    }
    result.mean_kb = counted ? bytes / counted / 1000 : 0;
    results.push_back(result);
  }

  n.api.nvEncDestroyInputBuffer(enc, input.inputBuffer);
  n.api.nvEncDestroyBitstreamBuffer(enc, output.bitstreamBuffer);
  n.api.nvEncDestroyEncoder(enc);
  return ok;
}

int main() {
  nvenc_t n;
  if (cuda_load_functions(&n.cu, nullptr) < 0 || nvenc_load_functions(&n.nv, nullptr) < 0) {
    std::printf("FAIL: cannot load the CUDA or NVENC library\n");
    return 2;
  }
  CUdevice device;
  if (n.cu->cuInit(0) != CUDA_SUCCESS || n.cu->cuDeviceGet(&device, 0) != CUDA_SUCCESS || n.cu->cuCtxCreate(&n.ctx, 0, device) != CUDA_SUCCESS) {
    std::printf("FAIL: CUDA context\n");
    return 2;
  }
  char name[128] = {};
  n.cu->cuDeviceGetName(name, sizeof(name), device);
  uint32_t max_version = 0;
  n.nv->NvEncodeAPIGetMaxSupportedVersion(&max_version);
  std::printf("GPU %s, driver NVENC API %u.%u\n", name, max_version >> 4, max_version & 0xF);

  bool mode_pass[2] = {true, true};  // 0 = avg+vbv, 1 = avg
  for (int codec = 0; codec < 3; codec++) {
    // References: a new encoder at each rate
    double reference_kb[RATE_COUNT] = {};
    bool skip = false;
    for (int i = 1; i < RATE_COUNT && !skip; i++) {
      std::vector<segment_result_t> reference;
      if (!run_codec(n, codec, true, &RATES_KBPS[i], 1, reference)) {
        skip = true;
        break;
      }
      reference_kb[i] = reference[0].mean_kb;
    }
    if (skip) {
      if (codec < 2) {
        mode_pass[0] = mode_pass[1] = false;  // H.264 and HEVC must work; AV1 is optional
      }
      continue;
    }

    for (int mode = 0; mode < 2; mode++) {
      std::vector<segment_result_t> results;
      if (!run_codec(n, codec, mode == 0, RATES_KBPS, RATE_COUNT, results)) {
        if (codec < 2) {
          mode_pass[mode] = false;
        }
        continue;
      }
      for (int i = 1; i < (int) results.size(); i++) {
        const auto &r = results[i];
        const double ratio = reference_kb[i] > 0 ? r.mean_kb / reference_kb[i] : 0;
        bool pass = r.reconfigure_ok && r.idr_frames == 0 && ratio > 0.8 && ratio < 1.25;
        // Mode "avg" keeps the VBV size of the start bitrate (spec R1: the VBV for the
        // ceiling). The controller does not go above the start bitrate, so a change
        // above it is information only in this mode.
        const bool info_only = mode == 1 && r.kbps > RATES_KBPS[0];
        if (!pass && !info_only) {
          mode_pass[mode] = false;
        }
        std::printf("RESULT codec=%s mode=%s change=%u->%u kbps reconfigure=%s idr_frames=%d mean_kB=%.1f reference_kB=%.1f target_kB=%.1f ratio=%.2f %s\n",
                    codec_name(codec), mode == 0 ? "avg+vbv" : "avg", results[i - 1].kbps, r.kbps,
                    r.reconfigure_ok ? "OK" : "FAILED", r.idr_frames, r.mean_kb, reference_kb[i],
                    r.kbps / 8.0 / FPS, ratio, info_only ? "INFO" : pass ? "PASS" : "FAIL");
      }
    }
  }

  const char outcome = mode_pass[0] ? 'A' : mode_pass[1] ? 'B' : 'C';
  std::printf("S1 OUTCOME: %c\n", outcome);
  n.cu->cuCtxDestroy(n.ctx);
  return outcome == 'A' ? 0 : 1;
}
```

- [ ] **Step 2: Build and run it on the laptop (RTX 5070)**

```bash
toolbox run -c moonlight bash -lc 'cd /var/home/catapultam/src/abr-spike && g++ -std=c++17 -O2 -Wall -I ~/GitHub/apollo-microphone-abr/third-party/nv-codec-headers/include nvenc_reconfigure_spike.cpp -ldl -o /tmp/nvenc_reconfigure_spike && /tmp/nvenc_reconfigure_spike | tee /tmp/abr-spike-laptop.txt'
```

Expected: a `GPU NVIDIA GeForce RTX 5070 Laptop GPU` line, 24 `RESULT` lines (3 codecs x 2 modes x 4 changes) and `S1 OUTCOME: A`. The run takes about 1 minute. The planning run of this program (2026-10-09, driver 615.71.09, NVENC API 13.1) gave outcome A: every `avg+vbv` change passed with `idr_frames=0` and a ratio of 0.87 to 1.06 to a new encoder at the same bitrate. In mode `avg` the increase above the start bitrate gave ratio 0.67 to 0.68 (the start VBV size limits the frames); this line shows `INFO`, not `FAIL`. The reference column, not the target column, is the measure: HEVC at 10 Mbps stays near 30 kB also in a new encoder (a floor of this picture content).

- [ ] **Step 3: Build the Windows program**

```bash
toolbox run -c moonlight bash -lc 'sudo dnf install -y mingw64-gcc-c++ mingw64-winpthreads-static && cd /var/home/catapultam/src/abr-spike && x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -I ~/GitHub/apollo-microphone-abr/third-party/nv-codec-headers/include nvenc_reconfigure_spike.cpp -o nvenc_reconfigure_spike.exe && ls -la nvenc_reconfigure_spike.exe && sha256sum nvenc_reconfigure_spike.exe'
```

Expected: `nvenc_reconfigure_spike.exe` (a few MB). Keep the sha256.

- [ ] **Step 4: Publish the program for the host**

The release body has no `branch:` line, so the updater (`ApolloUpdate.ps1`) ignores it.

```bash
cd /var/home/catapultam/src/abr-spike
gh release create spike-s1-nvenc-reconfigure nvenc_reconfigure_spike.exe -R catapultam/apollo-microphone --prerelease \
  --title "Spike S1: NVENC bitrate reconfigure test" \
  --notes "Test program for the adaptive bitrate design (spike S1). Not an Apollo build."
```

Expected: the release URL.

- [ ] **Step 5: Run it on CPLT-4A (RTX 4090) through agentbus**

Find the address of the `Armor-Console` session on `cplt-4a` with ListAgents (it starts with `agentbus:cplt-4a/Armor-Console`). Send it this request and wait for the answer (do not ask the user to relay it). Make sure that no Moonlight stream runs (the encoder sessions of Apollo are then free).

```text
Please run these PowerShell commands one at a time on CPLT-4A and send me the complete output of the last one (it prints about 26 lines and takes about 2 minutes). It is a read-only test program for NVENC; it does not change the system.
New-Item -ItemType Directory -Force -Path "$env:TEMP\abr-spike" | Out-Null
Invoke-WebRequest -Uri 'https://github.com/catapultam/apollo-microphone/releases/download/spike-s1-nvenc-reconfigure/nvenc_reconfigure_spike.exe' -OutFile "$env:TEMP\abr-spike\nvenc_reconfigure_spike.exe"
(Get-FileHash "$env:TEMP\abr-spike\nvenc_reconfigure_spike.exe" -Algorithm SHA256).Hash.ToLower()
Unblock-File "$env:TEMP\abr-spike\nvenc_reconfigure_spike.exe"; & "$env:TEMP\abr-spike\nvenc_reconfigure_spike.exe"; "exit code $LASTEXITCODE"
```

Expected: the hash equals Step 3; a `GPU NVIDIA GeForce RTX 4090` line; 24 `RESULT` lines and `S1 OUTCOME: A`. Save the answer to `/tmp/abr-spike-4090.txt`. When the program prints `FAIL: CUDA context` (the agentbus session can run without access to the GPU), ask the user to run the last command in a PowerShell window from a stream instead.

- [ ] **Step 6: Record the outcome in the plan and commit**

Fill the section below in `~/GitHub/moonlight-qt-mic-abr/docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md` with: the outcome of each GPU, the driver API version lines, and the `RESULT` lines with `FAIL` (if any). The RTX 4090 outcome decides Task 4 Step 5. When the two GPUs differ, use the RTX 4090 outcome and write the difference into the section.

```bash
cd ~/GitHub/moonlight-qt-mic-abr
git add docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md
git commit -m "Record the result of the NVENC reconfigure spike"
```

After Task 11, ask the user if the release `spike-s1-nvenc-reconfigure` can be deleted (`gh release delete spike-s1-nvenc-reconfigure -R catapultam/apollo-microphone --cleanup-tag --yes`).

#### S1 result

- RTX 5070 (Linux): outcome A in the planning run (2026-10-09, driver 615.71.09, NVENC API 13.1). Step 2 result: laptop: outcome A (2026-10-10, `GPU NVIDIA GeForce RTX 5070 Laptop GPU, driver NVENC API 13.1`). 24 RESULT lines, 0 FAIL, all `idr_frames=0`, all reconfigure OK. avg+vbv ratios 0.87 to 1.06. Mode avg 10000->60000: INFO, ratio 0.68 (H.264), 0.67 (HEVC), 1.04 (AV1).
- RTX 4090 (Windows, CPLT-4A, 2026-10-10, run by Armor-Console): outcome A, `GPU NVIDIA GeForce RTX 4090, driver NVENC API 13.1`. 24 RESULT lines, all reconfigure OK, all `idr_frames=0`. All 12 avg+vbv lines PASS (ratios 0.88 to 1.08). Runtime 31 s.
- Failed lines: one, in mode avg (not used): `RESULT codec=AV1 mode=avg change=60000->20000 kbps reconfigure=OK idr_frames=0 mean_kB=33.4 reference_kB=42.5 target_kB=41.7 ratio=0.79 FAIL`. Mode avg 10000->60000: INFO (0.67 H.264, 0.69 HEVC, 1.05 AV1).
- Decision for Task 4 Step 5: A (change averageBitRate, maxBitRate and vbvBufferSize in place with nvEncReconfigureEncoder, resetEncoder=0, forceIDR=0).

---
### Task 3: Host: wire format, bitrate chain and request state (pure header)

**Files:**
- Create: `~/GitHub/apollo-microphone-abr/src/adaptive_bitrate.h`
- Create: `~/GitHub/apollo-microphone-abr/tests/unit/test_adaptive_bitrate.cpp`
- Modify: `~/GitHub/apollo-microphone-abr/cmake/compile_definitions/common.cmake` (add the header after `src/live_resize.cpp`)

**Interfaces:**
- Consumes: nothing (no Sunshine header).
- Produces (Tasks 4 and 5 use these exact names):
  - `adaptive_bitrate::PACKET_TYPE_SET` (`0x3102`), `PACKET_TYPE_STATUS` (`0x3103`), `REQUEST_PAYLOAD_SIZE` (8), `STATUS_PAYLOAD_SIZE` (18), `MIN_CONFIGURED_KBPS` (500), `MAX_CONFIGURED_KBPS` (1000000), `RESTART_INTERVAL` (2 s), `IN_FLIGHT_TIMEOUT` (5 s).
  - `enum class status_e : std::uint16_t { applied, applied_restart, unchanged, not_supported, invalid, encoder_failed, input_only }`, `const char *status_name(status_e)`.
  - `struct request_t { std::uint32_t request_id; std::uint32_t configured_kbps; }`, `std::optional<request_t> decode_request(std::string_view payload)`.
  - `struct status_t { std::uint32_t request_id, requested_kbps, accepted_kbps, encoder_kbps; status_e status; }`, `std::array<std::uint8_t, STATUS_PAYLOAD_SIZE> encode_status(const status_t &)`.
  - `struct chain_input_t { std::int64_t configured_kbps; int max_bitrate; std::size_t warp_factor; int fec_percentage; bool audio_high_quality; int audio_channels; }`, `struct chain_result_t { std::int64_t accepted_kbps; std::int64_t encoder_kbps; }`, `chain_result_t encoder_bitrate(const chain_input_t &)`.
  - `std::optional<status_e> check_request(bool input_only, bool encoder_parallel, std::uint32_t configured_kbps)`.
  - `struct change_t { std::uint32_t request_id; int encoder_kbps; std::uint32_t requested_kbps; std::uint32_t accepted_kbps; }`, `struct result_t { change_t change; status_e status; }`.
  - `struct state_t` with `encoder_kbps`, `accepted_kbps`, `pending`, `in_flight`, `in_flight_since`, `restart_mode`, `last_restart` and `bool on_request(const change_t &, std::optional<change_t> &replaced)`, `bool on_result(const result_t &, time_point now)`, `std::optional<change_t> take_release(time_point now, bool resize_busy)`, `bool watchdog(time_point now)`, `void clear()`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_adaptive_bitrate.cpp`:

```cpp
/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Test src/adaptive_bitrate.h
 *
 * Builds without the rest of Sunshine. In the moonlight toolbox:
 *   g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/test_adaptive_bitrate.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/test_adaptive_bitrate && /tmp/test_adaptive_bitrate
 */
#include <gtest/gtest.h>
#include <src/adaptive_bitrate.h>

#include <algorithm>
#include <string>

using namespace std::chrono_literals;
using adaptive_bitrate::change_t;
using adaptive_bitrate::result_t;
using adaptive_bitrate::state_t;
using adaptive_bitrate::status_e;

namespace {
  // The inline code of cmd_announce (src/rtsp.cpp) before Task 5, without the logs.
  // encoder_bitrate() must give the same values.
  std::pair<std::int64_t, std::int64_t> old_chain(std::int64_t configuredBitrateKbps, int max_bitrate, std::size_t warp_factor, int fec_percentage, bool high_quality, int channels) {
    if (max_bitrate > 0) {
      if (max_bitrate < configuredBitrateKbps) {
        configuredBitrateKbps = max_bitrate;
      }
    }
    const std::int64_t accepted = configuredBitrateKbps;
    if (warp_factor >= 2) {
      configuredBitrateKbps *= warp_factor;
    }
    if (configuredBitrateKbps) {
      if (fec_percentage <= 80) {
        configuredBitrateKbps /= 100.f / (100 - fec_percentage);
      }
      auto audioBitrateAdjustment = (high_quality ? 256 : 96) * channels;
      configuredBitrateKbps -= std::min((std::int64_t) audioBitrateAdjustment, configuredBitrateKbps / 5);
      configuredBitrateKbps -= std::min((std::int64_t) 500, configuredBitrateKbps / 10);
    }
    return {accepted, configuredBitrateKbps};
  }

  change_t make_change(std::uint32_t id, int encoder_kbps) {
    return change_t {id, encoder_kbps, (std::uint32_t) encoder_kbps * 5 / 4, (std::uint32_t) encoder_kbps * 5 / 4};
  }

  state_t started_state() {
    state_t state;
    state.encoder_kbps = 30000;
    state.accepted_kbps = 40000;
    return state;
  }
}  // namespace

TEST(AdaptiveBitrateWire, IdsAndSizesMatchTheSpec) {
  EXPECT_EQ(adaptive_bitrate::PACKET_TYPE_SET, 0x3102);
  EXPECT_EQ(adaptive_bitrate::PACKET_TYPE_STATUS, 0x3103);
  EXPECT_EQ(adaptive_bitrate::REQUEST_PAYLOAD_SIZE, 8u);
  EXPECT_EQ(adaptive_bitrate::STATUS_PAYLOAD_SIZE, 18u);
  EXPECT_EQ((int) status_e::applied, 0);
  EXPECT_EQ((int) status_e::applied_restart, 1);
  EXPECT_EQ((int) status_e::unchanged, 2);
  EXPECT_EQ((int) status_e::not_supported, 3);
  EXPECT_EQ((int) status_e::invalid, 4);
  EXPECT_EQ((int) status_e::encoder_failed, 5);
  EXPECT_EQ((int) status_e::input_only, 6);
  EXPECT_STREQ(adaptive_bitrate::status_name(status_e::applied_restart), "APPLIED_RESTART");
}

TEST(AdaptiveBitrateWire, DecodesALittleEndianRequest) {
  const std::string payload {"\x07\x00\x00\x00\x40\x9c\x00\x00", 8};  // id 7, 40000 kbps
  auto request = adaptive_bitrate::decode_request(payload);
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->request_id, 7u);
  EXPECT_EQ(request->configured_kbps, 40000u);

  // A longer payload is accepted; a shorter one is not
  EXPECT_TRUE(adaptive_bitrate::decode_request(payload + "xx").has_value());
  EXPECT_FALSE(adaptive_bitrate::decode_request(payload.substr(0, 7)).has_value());
  EXPECT_FALSE(adaptive_bitrate::decode_request({}).has_value());
}

TEST(AdaptiveBitrateWire, EncodesALittleEndianStatus) {
  const auto bytes = adaptive_bitrate::encode_status({0x01020304, 40000, 20000, 15500, status_e::applied_restart});
  const std::array<std::uint8_t, 18> expected {
    0x04, 0x03, 0x02, 0x01,  // request_id
    0x40, 0x9c, 0x00, 0x00,  // requested 40000
    0x20, 0x4e, 0x00, 0x00,  // accepted 20000
    0x8c, 0x3c, 0x00, 0x00,  // encoder 15500
    0x01, 0x00,  // APPLIED_RESTART
  };
  EXPECT_EQ(bytes, expected);
}

TEST(AdaptiveBitrateChain, EqualsTheOldInlineCode) {
  for (std::int64_t configured : {500, 1500, 20000, 44000, 150000, 500000}) {
    for (int max_bitrate : {0, 20000}) {
      for (std::size_t warp : {1, 2}) {
        for (int fec : {0, 20, 90}) {
          for (bool high : {false, true}) {
            for (int channels : {2, 8}) {
              adaptive_bitrate::chain_input_t input {configured, max_bitrate, warp, fec, high, channels};
              const auto result = adaptive_bitrate::encoder_bitrate(input);
              const auto old = old_chain(configured, max_bitrate, warp, fec, high, channels);
              EXPECT_EQ(result.accepted_kbps, old.first) << configured << ' ' << max_bitrate << ' ' << warp << ' ' << fec << ' ' << high << ' ' << channels;
              EXPECT_EQ(result.encoder_kbps, old.second) << configured << ' ' << max_bitrate << ' ' << warp << ' ' << fec << ' ' << high << ' ' << channels;
            }
          }
        }
      }
    }
  }
}

TEST(AdaptiveBitrateChain, KnownValues) {
  // 44000 kbps, FEC 20 %, stereo high quality: 44000 / 1.25 = 35200, - 512 = 34688, - 500 = 34188
  auto result = adaptive_bitrate::encoder_bitrate({44000, 0, 1, 20, true, 2});
  EXPECT_EQ(result.accepted_kbps, 44000);
  EXPECT_EQ(result.encoder_kbps, 34188);

  // The host cap gives the accepted value
  result = adaptive_bitrate::encoder_bitrate({44000, 20000, 1, 20, true, 2});
  EXPECT_EQ(result.accepted_kbps, 20000);
  EXPECT_LT(result.encoder_kbps, 20000);
}

TEST(AdaptiveBitrateRequest, Refusals) {
  EXPECT_EQ(adaptive_bitrate::check_request(true, true, 20000), status_e::input_only);
  EXPECT_EQ(adaptive_bitrate::check_request(false, false, 20000), status_e::not_supported);
  EXPECT_EQ(adaptive_bitrate::check_request(false, true, 499), status_e::invalid);
  EXPECT_EQ(adaptive_bitrate::check_request(false, true, 1000001), status_e::invalid);
  EXPECT_FALSE(adaptive_bitrate::check_request(false, true, 500).has_value());
  EXPECT_FALSE(adaptive_bitrate::check_request(false, true, 1000000).has_value());
}

TEST(AdaptiveBitrateState, EqualValueWithNothingWaitingIsUnchanged) {
  auto state = started_state();
  std::optional<change_t> replaced;
  EXPECT_TRUE(state.on_request(make_change(1, 30000), replaced));
  EXPECT_FALSE(state.pending.has_value());
  EXPECT_FALSE(replaced.has_value());
}

TEST(AdaptiveBitrateState, NewerRequestReplacesThePendingOne) {
  auto state = started_state();
  std::optional<change_t> replaced;
  EXPECT_FALSE(state.on_request(make_change(1, 20000), replaced));
  EXPECT_FALSE(replaced.has_value());
  EXPECT_FALSE(state.on_request(make_change(2, 25000), replaced));
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(replaced->request_id, 1u);
  EXPECT_EQ(state.pending->request_id, 2u);

  // An equal value is not UNCHANGED while a change waits: it must replace it
  EXPECT_FALSE(state.on_request(make_change(3, 30000), replaced));
  EXPECT_EQ(state.pending->request_id, 3u);
}

TEST(AdaptiveBitrateState, ReleaseMovesPendingToInFlight) {
  auto state = started_state();
  const auto now = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  auto released = state.take_release(now, false);
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->request_id, 1u);
  EXPECT_FALSE(state.pending.has_value());
  ASSERT_TRUE(state.in_flight.has_value());
  EXPECT_EQ(state.in_flight_since, now);
  EXPECT_FALSE(state.take_release(now, false).has_value());
}

TEST(AdaptiveBitrateState, ReleaseWaitsForLiveResize) {
  // Review Focus 1: capture_async takes a pending resize size at the top of each loop,
  // so a bitrate restart during a resize would apply the size before the display changes
  auto state = started_state();
  const auto now = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  EXPECT_FALSE(state.take_release(now, true).has_value());
  EXPECT_TRUE(state.pending.has_value());
  EXPECT_TRUE(state.take_release(now + 150ms, false).has_value());
}

TEST(AdaptiveBitrateState, RestartModeKeepsTwoSecondsBetweenRestarts) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  state.take_release(t0, false);
  EXPECT_TRUE(state.on_result({make_change(1, 20000), status_e::applied_restart}, t0 + 500ms));
  EXPECT_TRUE(state.restart_mode);
  EXPECT_EQ(state.encoder_kbps, 20000);
  EXPECT_EQ(state.accepted_kbps, 25000u);

  state.on_request(make_change(2, 15000), replaced);
  EXPECT_FALSE(state.take_release(t0 + 2400ms, false).has_value());
  EXPECT_TRUE(state.take_release(t0 + 2500ms, false).has_value());
}

TEST(AdaptiveBitrateState, InPlaceChangesHaveNoInterval) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  state.take_release(t0, false);
  state.on_result({make_change(1, 20000), status_e::applied}, t0 + 20ms);
  EXPECT_FALSE(state.restart_mode);
  state.on_request(make_change(2, 15000), replaced);
  EXPECT_TRUE(state.take_release(t0 + 40ms, false).has_value());
}

TEST(AdaptiveBitrateState, ResultOfAnOlderRequestKeepsInFlight) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  state.take_release(t0, false);
  state.on_request(make_change(2, 15000), replaced);
  state.take_release(t0 + 10ms, false);  // the mail keeps only request 2
  EXPECT_FALSE(state.on_result({make_change(1, 20000), status_e::applied}, t0 + 20ms));
  ASSERT_TRUE(state.in_flight.has_value());
  EXPECT_EQ(state.in_flight->request_id, 2u);
  EXPECT_TRUE(state.on_result({make_change(2, 15000), status_e::applied}, t0 + 30ms));
  EXPECT_FALSE(state.in_flight.has_value());
  EXPECT_EQ(state.encoder_kbps, 15000);
}

TEST(AdaptiveBitrateState, EncoderFailureKeepsTheRunningValues) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 60000), replaced);
  state.take_release(t0, false);
  EXPECT_TRUE(state.on_result({make_change(1, 60000), status_e::encoder_failed}, t0 + 100ms));
  EXPECT_EQ(state.encoder_kbps, 30000);
  EXPECT_EQ(state.accepted_kbps, 40000u);
  // A failed restart is a restart: the interval applies
  EXPECT_TRUE(state.restart_mode);
  EXPECT_EQ(state.last_restart, t0 + 100ms);
}

TEST(AdaptiveBitrateState, WatchdogClearsALateRequest) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  state.take_release(t0, false);
  EXPECT_FALSE(state.watchdog(t0 + 5s));
  EXPECT_TRUE(state.watchdog(t0 + 5001ms));
  EXPECT_FALSE(state.in_flight.has_value());
}

TEST(AdaptiveBitrateState, ClearDropsPendingAndInFlight) {
  auto state = started_state();
  const auto t0 = std::chrono::steady_clock::now();
  std::optional<change_t> replaced;
  state.on_request(make_change(1, 20000), replaced);
  state.take_release(t0, false);
  state.on_request(make_change(2, 15000), replaced);
  state.clear();
  EXPECT_FALSE(state.pending.has_value());
  EXPECT_FALSE(state.in_flight.has_value());
}
```

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone-abr && g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/test_adaptive_bitrate.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/test_adaptive_bitrate && /tmp/test_adaptive_bitrate'`
Expected: FAIL with `src/adaptive_bitrate.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/adaptive_bitrate.h`:

```cpp
/**
 * @file src/adaptive_bitrate.h
 * @brief Wire format, bitrate chain and request state of the adaptive bitrate extension.
 * @details A Moonlight client asks for a new video bitrate with SET_BITRATE on the control
 * stream. The host changes the bitrate of the running encoder and answers each request with
 * BITRATE_STATUS. See docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md in the
 * moonlight-qt-mic repository. This header has no other Sunshine dependency, thus
 * tests/unit/test_adaptive_bitrate.cpp builds it alone.
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace adaptive_bitrate {

  // Control message ids (Apollo adaptive bitrate extension)
  constexpr std::uint16_t PACKET_TYPE_SET = 0x3102;
  constexpr std::uint16_t PACKET_TYPE_STATUS = 0x3103;

  // Payload sizes on the wire. Both payloads are little endian.
  constexpr std::size_t REQUEST_PAYLOAD_SIZE = 8;
  constexpr std::size_t STATUS_PAYLOAD_SIZE = 18;

  // Limits of the configured bitrate of a request. The client UI allows up to 500000.
  constexpr std::int64_t MIN_CONFIGURED_KBPS = 500;
  constexpr std::int64_t MAX_CONFIGURED_KBPS = 1000000;

  // Minimum time between two encoder restarts for a bitrate change
  constexpr auto RESTART_INTERVAL = std::chrono::seconds(2);

  // The control thread clears a released request without a result after this time
  constexpr auto IN_FLIGHT_TIMEOUT = std::chrono::seconds(5);

  enum class status_e : std::uint16_t {
    applied = 0,  ///< The encoder runs at the new bitrate. No IDR frame.
    applied_restart = 1,  ///< The host restarted the encoder at the new bitrate. One IDR frame.
    unchanged = 2,  ///< The encoder already runs at this value
    not_supported = 3,  ///< The encoder path cannot change the bitrate (sync capture path)
    invalid = 4,  ///< The configured bitrate is out of limits
    encoder_failed = 5,  ///< The restart failed. The encoder runs at the old bitrate.
    input_only = 6,  ///< The session has no video
  };

  /**
   * @brief Name of a status for the log.
   */
  constexpr const char *status_name(status_e status) {
    switch (status) {
      case status_e::applied:
        return "APPLIED";
      case status_e::applied_restart:
        return "APPLIED_RESTART";
      case status_e::unchanged:
        return "UNCHANGED";
      case status_e::not_supported:
        return "NOT_SUPPORTED";
      case status_e::invalid:
        return "INVALID";
      case status_e::encoder_failed:
        return "ENCODER_FAILED";
      case status_e::input_only:
        return "INPUT_ONLY";
    }
    return "UNKNOWN";
  }

  /**
   * @brief SET_BITRATE payload.
   */
  struct request_t {
    std::uint32_t request_id;
    std::uint32_t configured_kbps;  ///< Same unit as x-ml-video.configuredBitrateKbps
  };

  /**
   * @brief Read a SET_BITRATE payload.
   * @param payload The plain payload after the control header.
   * @return The request, or no value when the payload is shorter than REQUEST_PAYLOAD_SIZE.
   */
  inline std::optional<request_t> decode_request(std::string_view payload) {
    if (payload.size() < REQUEST_PAYLOAD_SIZE) {
      return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(payload.data());
    auto le32 = [bytes](std::size_t i) {
      return std::uint32_t(bytes[i]) | std::uint32_t(bytes[i + 1]) << 8 | std::uint32_t(bytes[i + 2]) << 16 | std::uint32_t(bytes[i + 3]) << 24;
    };
    return request_t {le32(0), le32(4)};
  }

  /**
   * @brief BITRATE_STATUS payload.
   */
  struct status_t {
    std::uint32_t request_id;
    std::uint32_t requested_kbps;  ///< The value of the request
    std::uint32_t accepted_kbps;  ///< The configured value after the host cap
    std::uint32_t encoder_kbps;  ///< The encoder bitrate after the full chain
    status_e status;
  };

  /**
   * @brief Write a BITRATE_STATUS payload in little endian byte order.
   */
  inline std::array<std::uint8_t, STATUS_PAYLOAD_SIZE> encode_status(const status_t &status) {
    std::array<std::uint8_t, STATUS_PAYLOAD_SIZE> out {};
    auto put32 = [&out](std::size_t i, std::uint32_t value) {
      out[i] = value & 0xFF;
      out[i + 1] = (value >> 8) & 0xFF;
      out[i + 2] = (value >> 16) & 0xFF;
      out[i + 3] = (value >> 24) & 0xFF;
    };
    put32(0, status.request_id);
    put32(4, status.requested_kbps);
    put32(8, status.accepted_kbps);
    put32(12, status.encoder_kbps);
    const auto code = static_cast<std::uint16_t>(status.status);
    out[16] = code & 0xFF;
    out[17] = (code >> 8) & 0xFF;
    return out;
  }

  /**
   * @brief Input of the bitrate chain. cmd_announce fills it at stream start and the
   * control handler uses it again with a new configured_kbps.
   */
  struct chain_input_t {
    std::int64_t configured_kbps = 0;  ///< Client configured bitrate
    int max_bitrate = 0;  ///< config::video.max_bitrate, 0 = no cap
    std::size_t warp_factor = 1;  ///< 1 when warp mode is not engaged
    int fec_percentage = 20;  ///< config::stream.fec_percentage
    bool audio_high_quality = true;  ///< audio::config_t::HIGH_QUALITY
    int audio_channels = 2;
  };

  struct chain_result_t {
    std::int64_t accepted_kbps;  ///< After the host cap
    std::int64_t encoder_kbps;  ///< After all steps
  };

  /**
   * @brief Convert a configured bitrate to the encoder bitrate.
   * @details The steps of cmd_announce in the same order: cap at max_bitrate, multiply by
   * the warp factor, remove the FEC share (when fec_percentage <= 80), subtract the audio
   * bitrate (at most 20 %), subtract 500 kbps for packet overhead (at most 10 %).
   * The stream start and the SET_BITRATE handler both use this function, thus they agree.
   */
  inline chain_result_t encoder_bitrate(const chain_input_t &input) {
    std::int64_t kbps = input.configured_kbps;
    if (input.max_bitrate > 0 && input.max_bitrate < kbps) {
      kbps = input.max_bitrate;
    }
    const std::int64_t accepted = kbps;

    if (input.warp_factor >= 2) {
      kbps *= input.warp_factor;
    }

    // The same float expression as the old code, so the values do not change
    if (input.fec_percentage <= 80) {
      kbps /= 100.f / (100 - input.fec_percentage);
    }

    const auto audio = (std::int64_t) ((input.audio_high_quality ? 256 : 96) * input.audio_channels);
    kbps -= std::min(audio, kbps / 5);
    kbps -= std::min((std::int64_t) 500, kbps / 10);

    return {accepted, kbps};
  }

  /**
   * @brief Check a SET_BITRATE request before the chain.
   * @param input_only True when the session streams no video.
   * @param encoder_parallel True when the encoder has PARALLEL_ENCODING (capture_async path).
   * @param configured_kbps The configured bitrate of the request.
   * @return The refusal status, or no value when the request is valid.
   */
  inline std::optional<status_e> check_request(bool input_only, bool encoder_parallel, std::uint32_t configured_kbps) {
    if (input_only) {
      return status_e::input_only;
    }
    if (!encoder_parallel) {
      return status_e::not_supported;
    }
    if (configured_kbps < MIN_CONFIGURED_KBPS || configured_kbps > MAX_CONFIGURED_KBPS) {
      return status_e::invalid;
    }
    return std::nullopt;
  }

  /**
   * @brief A bitrate change for the encode thread (mail::bitrate).
   */
  struct change_t {
    std::uint32_t request_id = 0;
    int encoder_kbps = 0;  ///< Value for video::config_t::bitrate
    std::uint32_t requested_kbps = 0;
    std::uint32_t accepted_kbps = 0;
  };

  /**
   * @brief The result of a change from the encode thread (mail::bitrate_result).
   */
  struct result_t {
    change_t change;
    status_e status;
  };

  /**
   * @brief Bitrate state of one session. Only the control thread uses it.
   */
  struct state_t {
    int encoder_kbps = 0;  ///< Encoder bitrate that runs now
    std::uint32_t accepted_kbps = 0;  ///< Configured bitrate after the host cap that runs now
    std::optional<change_t> pending;  ///< Newest request that is not released
    std::optional<change_t> in_flight;  ///< Newest released request without a result
    std::chrono::steady_clock::time_point in_flight_since;
    bool restart_mode = false;  ///< Set after the first encoder restart for a change
    std::chrono::steady_clock::time_point last_restart;

    /**
     * @brief Accept a checked request (spec 5.2 steps 7 and 8).
     * @param change The request after the chain.
     * @param replaced Gets the pending request that this request replaces.
     * @return True when the encoder already runs at this value and no change waits:
     * answer UNCHANGED now. False when the request is stored in pending.
     */
    bool on_request(const change_t &change, std::optional<change_t> &replaced) {
      replaced.reset();
      if (change.encoder_kbps == encoder_kbps && !pending && !in_flight) {
        return true;
      }
      replaced = pending;
      pending = change;
      return false;
    }

    /**
     * @brief Apply a result from the encode thread (spec 5.3 step 1).
     * @return True when the result is for the request in flight, which then ends.
     * A result for an older request leaves in_flight set.
     */
    bool on_result(const result_t &result, std::chrono::steady_clock::time_point now) {
      switch (result.status) {
        case status_e::applied_restart:
          restart_mode = true;
          last_restart = now;
          encoder_kbps = result.change.encoder_kbps;
          accepted_kbps = result.change.accepted_kbps;
          break;
        case status_e::applied:
        case status_e::unchanged:
          encoder_kbps = result.change.encoder_kbps;
          accepted_kbps = result.change.accepted_kbps;
          break;
        case status_e::encoder_failed:
          // The encoder runs at the old bitrate. The failed restart counts for the interval.
          restart_mode = true;
          last_restart = now;
          break;
        default:
          break;
      }
      if (in_flight && in_flight->request_id == result.change.request_id) {
        in_flight.reset();
        return true;
      }
      return false;
    }

    /**
     * @brief Release the pending request to the encode thread (spec 5.3 step 2).
     * @param now Current time.
     * @param resize_busy True when a live resize request is in progress or its display
     * thread runs. capture_async takes a pending resize size at the top of each loop, thus
     * a restart for a bitrate change must not start a loop before the display changes.
     * @return The change to raise on mail::bitrate, or no value.
     */
    std::optional<change_t> take_release(std::chrono::steady_clock::time_point now, bool resize_busy) {
      if (!pending || resize_busy) {
        return std::nullopt;
      }
      if (restart_mode && now - last_restart < RESTART_INTERVAL) {
        return std::nullopt;
      }
      const change_t change = *pending;
      pending.reset();
      in_flight = change;
      in_flight_since = now;
      return change;
    }

    /**
     * @brief Clear a released request that got no result (spec 5.3 step 3).
     * @return True when the function cleared it.
     */
    bool watchdog(std::chrono::steady_clock::time_point now) {
      if (in_flight && now - in_flight_since > IN_FLIGHT_TIMEOUT) {
        in_flight.reset();
        return true;
      }
      return false;
    }

    /**
     * @brief Drop the requests at session stop.
     */
    void clear() {
      pending.reset();
      in_flight.reset();
    }
  };

}  // namespace adaptive_bitrate
```

In `cmake/compile_definitions/common.cmake` add one line after `"${CMAKE_SOURCE_DIR}/src/live_resize.cpp"`:

```cmake
        "${CMAKE_SOURCE_DIR}/src/adaptive_bitrate.h"
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone-abr && g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/test_adaptive_bitrate.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/test_adaptive_bitrate && /tmp/test_adaptive_bitrate'`
Expected: `[  PASSED  ] 16 tests.`

- [ ] **Step 5: Commit**

```bash
cd ~/GitHub/apollo-microphone-abr
git add src/adaptive_bitrate.h tests/unit/test_adaptive_bitrate.cpp cmake/compile_definitions/common.cmake
git commit -m "Add the adaptive bitrate wire format, bitrate chain and request state"
```

---

### Task 4: Host: encode thread applies the bitrate (NVENC in place, restart for the others)

**Files:**
- Modify: `~/GitHub/apollo-microphone-abr/src/globals.h` (mails, after `MAIL(encoder_failed);`)
- Modify: `~/GitHub/apollo-microphone-abr/src/video.h` (`struct encode_session_t`)
- Modify: `~/GitHub/apollo-microphone-abr/src/video.cpp` (`nvenc_encode_session_t`, `encode_run`, `capture_async`)
- Modify: `~/GitHub/apollo-microphone-abr/src/nvenc/nvenc_base.h`, `~/GitHub/apollo-microphone-abr/src/nvenc/nvenc_base.cpp` (`create_encoder`, `encode_frame`, new `reconfigure_bitrate`, `vbv_size`)

**Interfaces:**
- Consumes (Task 3): `adaptive_bitrate::change_t`, `result_t`, `status_e`. Task 2: the S1 outcome.
- Produces (Task 5 uses these):
  - Mail `mail::bitrate`, type `adaptive_bitrate::change_t`, an event (`mail->event<adaptive_bitrate::change_t>(mail::bitrate)`): the control thread raises, `encode_run` and `capture_async` pop.
  - Mail `mail::bitrate_result`, type `adaptive_bitrate::result_t`, a queue (`mail->queue<adaptive_bitrate::result_t>(mail::bitrate_result)`): `encode_run` and `capture_async` raise, the control thread pops. Results: `applied` (NVENC in place), `applied_restart` (new encoder started), `unchanged`, `encoder_failed`.
  - `virtual bool video::encode_session_t::set_bitrate(int kbps)` (default `false`).
  - `bool nvenc::nvenc_base::reconfigure_bitrate(uint32_t kbps)`.
  - Host warning lines for the checks in Tasks 10 and 11: `NvEnc: NvEncReconfigureEncoder() failed: ...`, `NvEnc: the first frame after a bitrate change is an IDR frame`, `Bitrate request <id>: encoder failed at <kbps> kbps, back to <kbps> kbps`.

This task has no Linux unit test: `video.cpp` and `nvenc_base.cpp` need boost, FFmpeg and the NVENC runtime. The state machine around it is tested in Task 3. The check is the Windows CI build (Step 6) and the end-to-end test (Task 11).

- [ ] **Step 1: Add the mails**

In `src/globals.h`, after `MAIL(encoder_failed);`:

```cpp
  // Adaptive bitrate: change for the encode thread (event, newest wins),
  // results for the control thread (queue, no result is lost)
  MAIL(bitrate);
  MAIL(bitrate_result);
```

- [ ] **Step 2: Add the session interface**

In `src/video.h`, in `struct encode_session_t`, after `virtual void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;`:

```cpp

    /**
     * @brief Change the bitrate of the running encoder without a new IDR frame.
     * @param kbps New encoder bitrate (video::config_t::bitrate).
     * @return True when the encoder runs at the new bitrate now. False when it cannot
     * change it in place; the caller then makes a new encoder.
     */
    virtual bool set_bitrate(int kbps) {
      return false;
    }
```

- [ ] **Step 3: Add the NVENC change**

In `src/nvenc/nvenc_base.h`, after the declaration of `invalidate_ref_frames(...)` (public part):

```cpp

    /**
     * @brief Change the bitrate of the running encoder without a new IDR frame.
     * @details Calls NvEncReconfigureEncoder() with the saved initialization parameters,
     * a new average bitrate and, when the GPU supports a custom VBV size, a new VBV size.
     * Only the encode thread may call it, between two encode_frame() calls.
     * @param kbps New bitrate in kilobits per second.
     * @return `true` on success. On `false` the encoder runs at the old bitrate.
     */
    bool reconfigure_bitrate(uint32_t kbps);
```

In the `private:` part, before `NV_ENC_OUTPUT_PTR output_bitstream = nullptr;`:

```cpp
    /**
     * @brief VBV size in bits for a bitrate, with the frame rate and the VBV increase of create_encoder().
     */
    uint32_t vbv_size(uint32_t kbps) const;

    // Parameters of create_encoder() for reconfigure_bitrate(). nvenc_base is not copyable,
    // thus saved_init_params.encodeConfig can point to saved_enc_config.
    NV_ENC_INITIALIZE_PARAMS saved_init_params = {};
    NV_ENC_CONFIG saved_enc_config = {};
    uint32_t saved_framerate = 0;
    int saved_vbv_percentage_increase = 0;
    bool custom_vbv = false;
```

In the same file, in `struct { ... } encoder_state;`, after `bool rfi_needs_confirmation = false;`:

```cpp
      bool bitrate_changed = false;  ///< reconfigure_bitrate() succeeded; encode_frame() checks the next frame
```

In `src/nvenc/nvenc_base.cpp`, add after the `MAKE_NVENC_VER` define (near the top):

```cpp

namespace {
  // Spike S1 (adaptive bitrate plan, Task 2): true when NvEncReconfigureEncoder() can also
  // change vbvBufferSize. Outcome B sets it to false.
  constexpr bool RECONFIGURE_VBV = true;
}  // namespace
```

In `create_encoder()`, replace this block:

```cpp
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    if (get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE)) {
      enc_config.rcParams.vbvBufferSize = client_config.bitrate * 1000 / client_config.framerate;
      if (config.vbv_percentage_increase > 0) {
        enc_config.rcParams.vbvBufferSize += enc_config.rcParams.vbvBufferSize * config.vbv_percentage_increase / 100;
      }
    }
```

with:

```cpp
    enc_config.rcParams.averageBitRate = client_config.bitrate * 1000;

    saved_framerate = client_config.framerate;
    saved_vbv_percentage_increase = config.vbv_percentage_increase;
    custom_vbv = get_encoder_cap(NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE);
    if (custom_vbv) {
      enc_config.rcParams.vbvBufferSize = vbv_size(client_config.bitrate);
    }
```

In `create_encoder()`, replace:

```cpp
    init_params.encodeConfig = &enc_config;

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &init_params))) {
```

with:

```cpp
    init_params.encodeConfig = &enc_config;

    // Keep the parameters for reconfigure_bitrate()
    saved_enc_config = enc_config;
    saved_init_params = init_params;
    saved_init_params.encodeConfig = &saved_enc_config;

    if (nvenc_failed(nvenc->nvEncInitializeEncoder(encoder, &saved_init_params))) {
```

In `encode_frame()`, after the `nvenc_encoded_frame encoded_frame {...};` statement and before `if (encoder_state.rfi_needs_confirmation) {`:

```cpp

    if (encoder_state.bitrate_changed) {
      // Spike S1 on the real path: an in-place bitrate change must not make an IDR frame
      encoder_state.bitrate_changed = false;
      if (lock_bitstream.pictureType == NV_ENC_PIC_TYPE_IDR && !force_idr) {
        BOOST_LOG(warning) << "NvEnc: the first frame after a bitrate change is an IDR frame";
      }
    }
```

Add these two functions before `uint32_t nvenc_base::min_struct_version(...)`:

```cpp
  uint32_t nvenc_base::vbv_size(uint32_t kbps) const {
    // The formula of create_encoder(): one frame at the bitrate, plus the VBV increase
    uint32_t size = kbps * 1000 / saved_framerate;
    if (saved_vbv_percentage_increase > 0) {
      size += size * saved_vbv_percentage_increase / 100;
    }
    return size;
  }

  bool nvenc_base::reconfigure_bitrate(uint32_t kbps) {
    if (!encoder || kbps == 0) {
      return false;
    }

    NV_ENC_CONFIG new_config = saved_enc_config;
    const uint32_t old_kbps = new_config.rcParams.averageBitRate / 1000;
    const uint32_t old_vbv = new_config.rcParams.vbvBufferSize;
    new_config.rcParams.averageBitRate = kbps * 1000;
    if (custom_vbv && RECONFIGURE_VBV) {
      new_config.rcParams.vbvBufferSize = vbv_size(kbps);
    }

    // maxBitRate is for VBR only; the encoder uses CBR. No new IDR frame and no reset of the
    // rate control: enablePTD = 1 keeps the next frame a P frame.
    NV_ENC_RECONFIGURE_PARAMS params = {min_struct_version(NV_ENC_RECONFIGURE_PARAMS_VER)};
    params.reInitEncodeParams = saved_init_params;
    params.reInitEncodeParams.encodeConfig = &new_config;
    params.resetEncoder = 0;
    params.forceIDR = 0;

    if (nvenc_failed(nvenc->nvEncReconfigureEncoder(encoder, &params))) {
      BOOST_LOG(warning) << "NvEnc: NvEncReconfigureEncoder() failed: " << last_nvenc_error_string;
      return false;
    }

    saved_enc_config = new_config;
    encoder_state.bitrate_changed = true;
    BOOST_LOG(info) << "NvEnc: bitrate " << old_kbps << " -> " << kbps << " kbps, VBV " << old_vbv << " -> " << new_config.rcParams.vbvBufferSize << " bits";
    return true;
  }

```

In `src/video.cpp`, in `class nvenc_encode_session_t`, after `invalidate_ref_frames(...) override { ... }`:

```cpp

    bool set_bitrate(int kbps) override {
      if (!device || !device->nvenc || kbps <= 0) {
        return false;
      }
      return device->nvenc->reconfigure_bitrate((uint32_t) kbps);
    }
```

- [ ] **Step 4: Apply the change in the encode thread and restart for the other encoders**

In `src/video.cpp`, add `#include "adaptive_bitrate.h"` after `#include "live_resize.h"`.

Change the signature of `encode_run` from:

```cpp
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data
  ) {
```

to:

```cpp
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t &config,  // capture_async owns it; a bitrate applied in place stays for the next encoder
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data,
    const std::optional<adaptive_bitrate::change_t> &bitrate_applying,  // the change that this encoder start applies
    std::optional<adaptive_bitrate::change_t> &bitrate_restart  // set here: restart the encoder at this bitrate
  ) {
```

In `encode_run`, replace:

```cpp
    // Live resize: tell the control thread that an encoder runs at config.width x config.height.
    // The control thread ignores this when no resize is in progress.
    mail->event<bool>(mail::resize_done)->raise(true);
```

with:

```cpp
    // Live resize: tell the control thread that an encoder runs at config.width x config.height.
    // The control thread ignores this when no resize is in progress.
    mail->event<bool>(mail::resize_done)->raise(true);

    // Adaptive bitrate: a restart for a bitrate change is done when the new encoder runs
    auto bitrate_results = mail->queue<adaptive_bitrate::result_t>(mail::bitrate_result);
    if (bitrate_applying) {
      bitrate_results->raise(adaptive_bitrate::result_t {*bitrate_applying, adaptive_bitrate::status_e::applied_restart});
      BOOST_LOG(info) << "Bitrate request "sv << bitrate_applying->request_id << ": "sv << bitrate_applying->accepted_kbps
                      << " kbps -> encoder "sv << config.bitrate << " kbps (restart)"sv;
    }
```

In `encode_run`, after `auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);` add:

```cpp
    auto bitrate_events = mail->event<adaptive_bitrate::change_t>(mail::bitrate);
```

In the `while (true)` loop of `encode_run`, after the block `if (requested_idr_frame) { session->request_idr_frame(); }` add:

```cpp

      // Adaptive bitrate: change the running encoder, else restart it at the new bitrate
      if (auto change = bitrate_events->pop(0ms)) {
        if (change->encoder_kbps == config.bitrate) {
          bitrate_results->raise(adaptive_bitrate::result_t {*change, adaptive_bitrate::status_e::unchanged});
        } else if (session->set_bitrate(change->encoder_kbps)) {
          BOOST_LOG(info) << "Bitrate request "sv << change->request_id << ": "sv << change->accepted_kbps
                          << " kbps -> encoder "sv << change->encoder_kbps << " kbps (in place)"sv;
          config.bitrate = change->encoder_kbps;
          bitrate_results->raise(adaptive_bitrate::result_t {*change, adaptive_bitrate::status_e::applied});
        } else {
          // capture_async makes a new encoder at the new bitrate
          bitrate_restart = *change;
          break;
        }
      }
```

In `capture_async`, after the line `bool resize_pending = false;` add:

```cpp

    // Adaptive bitrate state (see src/adaptive_bitrate.h)
    auto bitrate_event = mail->event<adaptive_bitrate::change_t>(mail::bitrate);
    auto bitrate_results = mail->queue<adaptive_bitrate::result_t>(mail::bitrate_result);
    std::optional<adaptive_bitrate::change_t> bitrate_restart;  // set by encode_run
    std::optional<adaptive_bitrate::change_t> bitrate_applying;  // the change of the next encoder start
    int previous_bitrate = config.bitrate;
```

In `capture_async`, after the live resize block that ends with `BOOST_LOG(info) << "Live resize: encoder size set to "sv << ...; }` and before `auto &encoder = *chosen_encoder;` add:

```cpp

      // A bitrate restart from encode_run, or a change that came while no encoder ran.
      // A change in the mail is newer than the restart request, thus it wins.
      if (auto change = bitrate_event->pop(0ms)) {
        bitrate_restart = *change;
      }
      if (bitrate_restart) {
        previous_bitrate = config.bitrate;
        config.bitrate = bitrate_restart->encoder_kbps;
        bitrate_applying = bitrate_restart;
        bitrate_restart.reset();
      }
```

In `capture_async`, change the `encode_run(...)` call to pass the two new arguments:

```cpp
      encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        ref->reinit_event,
        *ref->encoder_p,
        channel_data,
        bitrate_applying,
        bitrate_restart
      );
```

In `capture_async`, replace the block after `encode_run`:

```cpp
      // Clear a stale failure flag, then check it only for a resize
      bool encoder_failed = encoder_failed_event->pop(0ms);
      if (resize_pending && encoder_failed) {
        BOOST_LOG(warning) << "Live resize: encoder rejected "sv << config.width << 'x' << config.height
                           << ", back to "sv << last_good_config.width << 'x' << last_good_config.height;
        config = last_good_config;
        resize_refused_event->raise((std::uint16_t) live_resize::reason_e::encoder_failed);
      }
      resize_pending = false;
```

with:

```cpp
      // Clear a stale failure flag, then check it only for a resize or a bitrate restart
      bool encoder_failed = encoder_failed_event->pop(0ms);
      if (resize_pending && encoder_failed) {
        BOOST_LOG(warning) << "Live resize: encoder rejected "sv << config.width << 'x' << config.height
                           << ", back to "sv << last_good_config.width << 'x' << last_good_config.height;
        // The revert is for the size only; keep the newest bitrate
        const int keep_bitrate = config.bitrate;
        config = last_good_config;
        config.bitrate = keep_bitrate;
        resize_refused_event->raise((std::uint16_t) live_resize::reason_e::encoder_failed);
      }
      if (bitrate_applying && encoder_failed) {
        BOOST_LOG(warning) << "Bitrate request "sv << bitrate_applying->request_id << ": encoder failed at "sv << config.bitrate
                           << " kbps, back to "sv << previous_bitrate << " kbps"sv;
        config.bitrate = previous_bitrate;
        bitrate_results->raise(adaptive_bitrate::result_t {*bitrate_applying, adaptive_bitrate::status_e::encoder_failed});
      }
      resize_pending = false;
      bitrate_applying.reset();
```

Check that `encode_run` has no other caller and that nothing else writes `config` in `encode_run`:

```bash
cd ~/GitHub/apollo-microphone-abr
grep -n "encode_run(" src/*.cpp src/*.h
grep -n "config\.[a-zA-Z_]* *=[^=]" src/video.cpp | sed -n 1,60p
```

Expected: one definition and one call of `encode_run(` (in `capture_async`); `encode_run_sync(` is a different function. In `encode_run` the only write to `config` is `config.bitrate = change->encoder_kbps;` (spec R9).

- [ ] **Step 5: Apply the S1 outcome**

Read "S1 result" in Task 2.

- Outcome A: no change.
- Outcome B: in `src/nvenc/nvenc_base.cpp` set `constexpr bool RECONFIGURE_VBV = false;` and change its comment to `// Spike S1 outcome B: NvEncReconfigureEncoder() refused a new vbvBufferSize; keep the start VBV size`.
- Outcome C: remove the `set_bitrate` override from `nvenc_encode_session_t` (NVENC then uses the restart path). Keep `reconfigure_bitrate()` out too: remove the declaration, `vbv_size()`, the saved members, `RECONFIGURE_VBV`, `bitrate_changed` and the check in `encode_frame()`, and restore the original VBV code of `create_encoder()`. Write in the commit message: "NVENC uses the restart path (spike S1 outcome C)". Tell the user that U4 now applies to NVENC too.

- [ ] **Step 6: Build on Windows CI**

```bash
cd ~/GitHub/apollo-microphone-abr
git add src/globals.h src/video.h src/video.cpp src/nvenc/nvenc_base.h src/nvenc/nvenc_base.cpp
git commit -m "Apply a bitrate change in the encode thread, in place for NVENC"
git push -u origin adaptive-bitrate
OLD=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b adaptive-bitrate -L 1 --json databaseId -q '.[0].databaseId')
gh workflow run build-windows.yml -R catapultam/apollo-microphone --ref adaptive-bitrate
for i in $(seq 1 60); do RUN=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b adaptive-bitrate -L 1 --json databaseId -q '.[0].databaseId'); [ -n "$RUN" ] && [ "$RUN" != "$OLD" ] && break; done; echo "$RUN"
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
```

Expected: the run passes. On a compile error: `gh run view "$RUN" -R catapultam/apollo-microphone --log-failed | grep -E "error|Error" | head -30`, fix, commit, push and run again. The Linux googletest of Task 3 must still pass.

---

### Task 5: Host: bitrate chain at stream start, SDP flag and control handler

**Files:**
- Modify: `~/GitHub/apollo-microphone-abr/src/stream.h` (`struct config_t`, include)
- Modify: `~/GitHub/apollo-microphone-abr/src/rtsp.cpp` (`cmd_describe`, `cmd_announce`)
- Modify: `~/GitHub/apollo-microphone-abr/src/stream.cpp` (ids, `session_t`, `send_bitrate_status`, handler, control loop, `session::alloc`)

**Interfaces:**
- Consumes (Task 3): `adaptive_bitrate::chain_input_t`, `encoder_bitrate()`, `check_request()`, `decode_request()`, `encode_status()`, `status_t`, `state_t`, `change_t`, `result_t`, `status_name()`. (Task 4): mails `mail::bitrate` (event) and `mail::bitrate_result` (queue); `video::encoder_supports_live_resize()`.
- Produces: `stream::config_t::bitrate_chain` (`adaptive_bitrate::chain_input_t`); the SDP line `a=x-ss-general.dynamicBitrate:1`; the wire behavior of spec 3 (the client tasks rely on it).

- [ ] **Step 1: Store the chain input in the stream config**

In `src/stream.h`, add `#include "adaptive_bitrate.h"` after `#include "audio.h"`. In `struct config_t`, after `video::config_t monitor;`:

```cpp
    adaptive_bitrate::chain_input_t bitrate_chain;  ///< Bitrate chain of cmd_announce; SET_BITRATE uses it again
```

- [ ] **Step 2: Use the chain function in cmd_announce**

In `src/rtsp.cpp` `cmd_announce`, inside the `try` block, replace:

```cpp
      BOOST_LOG(info) << "Client Requested bitrate is [" << configuredBitrateKbps << "kbps]";

      if (config::video.max_bitrate > 0) {
        if (config::video.max_bitrate < configuredBitrateKbps) {
          configuredBitrateKbps = config::video.max_bitrate;
        }
      }

      BOOST_LOG(info) << "Host Streaming bitrate is [" << configuredBitrateKbps << "kbps]";

      // Hack: Restore bitrate for warp mode
      size_t warp_factor = std::round((float)config.monitor.framerate * 1000 / session.fps);
      if (config::video.limit_framerate && warp_factor >= 2) {
        configuredBitrateKbps *= warp_factor;
        BOOST_LOG(info) << "Warp factor [" << warp_factor << "] engaged";
      }
```

with:

```cpp
      BOOST_LOG(info) << "Client Requested bitrate is [" << configuredBitrateKbps << "kbps]";

      // Hack: Restore bitrate for warp mode. adaptive_bitrate::encoder_bitrate() applies it.
      size_t warp_factor = std::round((float)config.monitor.framerate * 1000 / session.fps);
      config.bitrate_chain.warp_factor = (config::video.limit_framerate && warp_factor >= 2) ? warp_factor : 1;
```

Below, replace the whole block that starts with the comment `// If the client sent a configured bitrate, we will choose the actual bitrate ourselves` and ends with `config.monitor.bitrate = configuredBitrateKbps;\n    }` with:

```cpp
    // If the client sent a configured bitrate, we will choose the actual bitrate ourselves
    // by using FEC percentage and audio quality settings. If the calculated bitrate ends up
    // too low, we'll allow it to exceed the limits rather than reducing the encoding bitrate
    // down to nearly nothing. The control stream uses the same chain for SET_BITRATE
    // (adaptive_bitrate::encoder_bitrate()), thus the audio flags must be final here.
    config.bitrate_chain.configured_kbps = configuredBitrateKbps;
    config.bitrate_chain.max_bitrate = config::video.max_bitrate;
    config.bitrate_chain.fec_percentage = config::stream.fec_percentage;
    config.bitrate_chain.audio_high_quality = config.audio.flags[audio::config_t::HIGH_QUALITY];
    config.bitrate_chain.audio_channels = config.audio.channels;
    {
      const auto chain = adaptive_bitrate::encoder_bitrate(config.bitrate_chain);
      BOOST_LOG(info) << "Host Streaming bitrate is [" << chain.accepted_kbps << "kbps]";
      if (config.bitrate_chain.warp_factor >= 2) {
        BOOST_LOG(info) << "Warp factor [" << config.bitrate_chain.warp_factor << "] engaged";
      }
      if (configuredBitrateKbps) {
        BOOST_LOG(debug) << "Client configured bitrate is "sv << chain.accepted_kbps * (std::int64_t) config.bitrate_chain.warp_factor << " Kbps"sv;
        BOOST_LOG(debug) << "Final adjusted video encoding bitrate is "sv << chain.encoder_kbps << " Kbps"sv;
        config.monitor.bitrate = (int) chain.encoder_kbps;
      }
    }
```

The log lines "Host Streaming bitrate" and "Warp factor" now come after the audio fix-up (a small change of the log order, no change of the values). Check that `configuredBitrateKbps` has no other use:

```bash
cd ~/GitHub/apollo-microphone-abr && grep -n "configuredBitrateKbps" src/rtsp.cpp
```

Expected: only the declaration, the `args.try_emplace`, the parse with the fallback to `config.monitor.bitrate`, the "Client Requested bitrate" log, `config.bitrate_chain.configured_kbps = configuredBitrateKbps;` and `if (configuredBitrateKbps)`.

- [ ] **Step 3: Advertise the feature**

In `src/rtsp.cpp` `cmd_describe`, after the `#endif` of the live resize block (`a=x-ss-general.liveResize:1`):

```cpp

    // Adaptive bitrate (Apollo extension). Only capture_async reads a bitrate change, and
    // only an encoder with PARALLEL_ENCODING uses capture_async. encoder_supports_live_resize()
    // tests exactly this flag.
    if (video::encoder_supports_live_resize()) {
      ss << "a=x-ss-general.dynamicBitrate:1"sv << std::endl;
    }
```

- [ ] **Step 4: Add the ids, the session state and the status send**

In `src/stream.cpp`, add `#include "adaptive_bitrate.h"` before `#include "live_resize.h"`. After `#define IDX_RESIZE_REFUSED 20`:

```cpp
#define IDX_SET_BITRATE 21
#define IDX_BITRATE_STATUS 22
```

In `packetTypes[]`, after `0x3101,  // Resize refused (Apollo live resize extension)`:

```cpp
  0x3102,  // Set bitrate (Apollo adaptive bitrate extension)
  0x3103,  // Bitrate status (Apollo adaptive bitrate extension)
```

After `struct control_resize_refused_t { ... };` (inside the `#pragma pack(push, 1)` area):

```cpp

  struct control_bitrate_status_t {
    control_header_v2 header;

    std::array<std::uint8_t, adaptive_bitrate::STATUS_PAYLOAD_SIZE> payload;  ///< adaptive_bitrate::encode_status()
  };
```

In `struct session_t`, after the live resize `} resize;` member:

```cpp

    // Adaptive bitrate state (see src/adaptive_bitrate.h). Only the control thread uses it.
    struct {
      adaptive_bitrate::state_t state;
      safe::mail_raw_t::event_t<adaptive_bitrate::change_t> change_queue;  ///< Change for encode_run or capture_async
      safe::mail_raw_t::queue_t<adaptive_bitrate::result_t> result_queue;  ///< Results of encode_run and capture_async
    } bitrate;
```

After the function `send_resize_refused(...)`:

```cpp

  /**
   * @brief Send BITRATE_STATUS to the client.
   * @details Only the control thread may call this, because ENet is not thread safe.
   */
  int send_bitrate_status(session_t *session, const adaptive_bitrate::status_t &status) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Could not send bitrate status, still waiting for PING from Moonlight"sv;
      return -1;
    }

    control_bitrate_status_t plaintext {};
    plaintext.header.type = packetTypes[IDX_BITRATE_STATUS];
    plaintext.header.payloadLength = adaptive_bitrate::STATUS_PAYLOAD_SIZE;
    plaintext.payload = adaptive_bitrate::encode_status(status);

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Could not send bitrate status to ["sv << addr << ':' << port << ']';
      return -1;
    }
    return 0;
  }

  /**
   * @brief Answer a SET_BITRATE request.
   * @details APPLIED, APPLIED_RESTART and UNCHANGED give the values of the change. The other
   * codes give the values that run now. Only the control thread may call this.
   */
  static void answer_bitrate(session_t *session, const adaptive_bitrate::change_t &change, adaptive_bitrate::status_e status) {
    using adaptive_bitrate::status_e;
    const bool runs = status == status_e::applied || status == status_e::applied_restart || status == status_e::unchanged;
    const auto &state = session->bitrate.state;
    const adaptive_bitrate::status_t message {
      change.request_id,
      change.requested_kbps,
      runs ? change.accepted_kbps : state.accepted_kbps,
      (std::uint32_t) (runs ? change.encoder_kbps : state.encoder_kbps),
      status,
    };
    send_bitrate_status(session, message);
    BOOST_LOG(info) << "Bitrate request "sv << change.request_id << ": "sv << adaptive_bitrate::status_name(status)
                    << ", requested "sv << message.requested_kbps << " kbps, accepted "sv << message.accepted_kbps
                    << " kbps, encoder "sv << message.encoder_kbps << " kbps"sv;
  }
```

- [ ] **Step 5: Add the handler**

In `controlBroadcastThread`, after the `server->map(packetTypes[IDX_RESIZE_REQUEST], ...)` handler and before `server->map(packetTypes[IDX_ENCRYPTED], ...)`:

```cpp
    server->map(packetTypes[IDX_SET_BITRATE], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_SET_BITRATE]"sv;

      auto request = adaptive_bitrate::decode_request(payload);
      if (!request) {
        BOOST_LOG(warning) << "Bitrate request: runt payload of "sv << payload.size() << " bytes"sv;
        return;
      }

      adaptive_bitrate::change_t change;
      change.request_id = request->request_id;
      change.requested_kbps = request->configured_kbps;

      // Only capture_async reads a change; encoder_supports_live_resize() tests PARALLEL_ENCODING
      if (auto refusal = adaptive_bitrate::check_request(session->config.monitor.input_only, video::encoder_supports_live_resize(), request->configured_kbps)) {
        answer_bitrate(session, change, *refusal);
        return;
      }

      auto chain = session->config.bitrate_chain;
      chain.configured_kbps = request->configured_kbps;
      const auto result = adaptive_bitrate::encoder_bitrate(chain);
      change.accepted_kbps = (std::uint32_t) result.accepted_kbps;
      change.encoder_kbps = (int) result.encoder_kbps;

      std::optional<adaptive_bitrate::change_t> replaced;
      if (session->bitrate.state.on_request(change, replaced)) {
        answer_bitrate(session, change, adaptive_bitrate::status_e::unchanged);
        return;
      }
      if (replaced) {
        BOOST_LOG(debug) << "Bitrate request "sv << change.request_id << " replaces pending request "sv << replaced->request_id;
      }
    });

```

- [ ] **Step 6: Drain the results and release the next change in the control loop**

In the control loop, in the `STOPPING` branch, after `session->resize.pending.clear();`:

```cpp
            session->bitrate.state.clear();
```

In the `else` branch (a peer is connected), after the closing `}` of `switch (live_resize::take_pending(...)) { ... }`:

```cpp

            // Adaptive bitrate: answer the results, then release the next change. The state is
            // in src/adaptive_bitrate.h.
            auto &bitrate = session->bitrate;
            while (session->control.peer && bitrate.result_queue->peek()) {
              auto result = bitrate.result_queue->pop();
              if (!result) {
                continue;
              }
              bitrate.state.on_result(*result, now);
              answer_bitrate(session, result->change, result->status);
            }
            if (bitrate.state.watchdog(now)) {
              BOOST_LOG(warning) << "Bitrate request got no encoder result in 5 s, state cleared"sv;
            }
            // A live resize in progress takes the size at the next capture_async loop. A bitrate
            // restart must not start that loop early (adaptive_bitrate::state_t::take_release()).
            const bool resize_busy = resize.in_progress || resize.worker_id.load(std::memory_order_acquire) != 0;
            if (auto change = bitrate.state.take_release(now, resize_busy)) {
              bitrate.change_queue->raise(*change);
            }
```

- [ ] **Step 7: Start values in session::alloc**

In `session::alloc`, after `session->resize.done_queue = mail->event<bool>(mail::resize_done);`:

```cpp

      // The encoder starts at config.monitor.bitrate; the chain gives the accepted value of the start
      session->bitrate.state = {};
      session->bitrate.state.encoder_kbps = config.monitor.bitrate;
      session->bitrate.state.accepted_kbps = (std::uint32_t) adaptive_bitrate::encoder_bitrate(config.bitrate_chain).accepted_kbps;
      session->bitrate.change_queue = mail->event<adaptive_bitrate::change_t>(mail::bitrate);
      session->bitrate.result_queue = mail->queue<adaptive_bitrate::result_t>(mail::bitrate_result);
```

- [ ] **Step 8: Check the places that the reviewer must see**

```bash
cd ~/GitHub/apollo-microphone-abr
grep -n "IDX_SET_BITRATE\|IDX_BITRATE_STATUS\|0x3102\|0x3103" src/stream.cpp
grep -n "control_server.send" src/stream.cpp
grep -n "dynamicBitrate\|bitrate_chain" src/rtsp.cpp src/stream.h src/stream.cpp
```

Expected: the two defines, the two `packetTypes` entries, the handler map and the use in `send_bitrate_status`; every `control_server.send` call is in a function that only `controlBroadcastThread` calls; the SDP line, the chain fields in `cmd_announce`, the config field, the handler and `alloc`.

- [ ] **Step 9: Run the host tests and build on Windows CI**

```bash
toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone-abr && g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/test_adaptive_bitrate.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/test_adaptive_bitrate && /tmp/test_adaptive_bitrate | tail -1'
cd ~/GitHub/apollo-microphone-abr
git add src/stream.h src/stream.cpp src/rtsp.cpp
git commit -m "Add the SET_BITRATE handler, BITRATE_STATUS and the dynamicBitrate SDP flag"
git push
OLD=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b adaptive-bitrate -L 1 --json databaseId -q '.[0].databaseId')
gh workflow run build-windows.yml -R catapultam/apollo-microphone --ref adaptive-bitrate
for i in $(seq 1 60); do RUN=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b adaptive-bitrate -L 1 --json databaseId -q '.[0].databaseId'); [ -n "$RUN" ] && [ "$RUN" != "$OLD" ] && break; done; echo "$RUN"
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
```

Expected: `[  PASSED  ] 16 tests.` and the run passes. Fix compile errors as in Task 4 Step 6.

---

### Task 6: moonlight-common-c: bitrate request, status callback, SDP flag and network counters

All paths in this task are relative to `~/GitHub/moonlight-qt-mic-abr/moonlight-common-c/moonlight-common-c` (branch `adaptive-bitrate`).

**Files:**
- Create: `test/bitrate_protocol_test.c`
- Modify: `src/Limelight.h` (status codes, callback typedef and field, API)
- Modify: `src/Limelight-internal.h` (`DynamicBitrateSupported`, `BITRATE_STATUS_PAYLOAD`, `parseBitrateStatusPayload()`)
- Modify: `src/ControlStream.c` (ids, tables, request, status parse and callback, frame counters)
- Modify: `src/RtpVideoQueue.c` (`reconstructFrame()`: count recovered packets)
- Modify: `src/RtspConnection.c` (parse `x-ss-general.dynamicBitrate`)
- Modify: `src/Connection.c`, `src/Misc.c`, `src/FakeCallbacks.c`

**Interfaces:**
- Consumes: the wire protocol of Global Constraints.
- Produces (Task 9 uses these):
  - `#define LI_BITRATE_STATUS_APPLIED 0` ... `LI_BITRATE_STATUS_INPUT_ONLY 6` in `Limelight.h`.
  - `typedef void(*ConnListenerBitrateStatus)(uint32_t requestId, uint32_t requestedKbps, uint32_t acceptedKbps, uint32_t encoderKbps, uint16_t status);` and the field `bitrateStatus` as the last field of `CONNECTION_LISTENER_CALLBACKS`.
  - `bool LiIsDynamicBitrateSupported(void);`
  - `int LiSendBitrateRequest(uint32_t configuredKbps, uint32_t* requestId);` (0 = sent, -1 = not sent)
  - `void LiGetVideoFrameCounters(uint32_t* finishedFrames, uint32_t* lostFrames);` (cumulative, from the session start)
  - `RTP_VIDEO_STATS::packetCountFecRecovered` now counts recovered video data packets.
  - Internal: `bool parseBitrateStatusPayload(const char* payload, int length, PBITRATE_STATUS_PAYLOAD status);`

- [ ] **Step 1: Write the failing test**

Create `test/bitrate_protocol_test.c`:

```c
// Test of the adaptive bitrate API without a connection, the SDP parse, the
// BITRATE_STATUS parse and the frame counters.
//
// Build the library once in the moonlight toolbox:
//   cmake -S . -B /tmp/mcc-build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug
//   cmake --build /tmp/mcc-build -j
// Then:
//   gcc -O0 -g -I src -I enet/include test/bitrate_protocol_test.c \
//       /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a \
//       -lcrypto -lpthread -o /tmp/bitrate_protocol_test && /tmp/bitrate_protocol_test
#include "Limelight-internal.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool condition, const char* what) {
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

int main(void) {
    uint32_t requestId = 77;
    uint32_t value = 5;
    uint32_t finished = 1, lost = 1;
    BITRATE_STATUS_PAYLOAD status;

    // Without a connection the host cannot have advertised the feature
    check(!LiIsDynamicBitrateSupported(), "LiIsDynamicBitrateSupported() is true before a connection");

    // Without a connection the request must fail and leave the id alone
    check(LiSendBitrateRequest(20000, &requestId) == -1, "LiSendBitrateRequest() did not fail without a connection");
    check(requestId == 77, "LiSendBitrateRequest() changed the id on failure");

    // The SDP attribute parse used by the DESCRIBE handler
    {
        const char* sdpWith = "a=x-ss-general.featureFlags:3\r\na=x-ss-general.dynamicBitrate:1\r\na=rtpmap:96 opus/48000/1\r\n";
        const char* sdpWithout = "a=x-ss-general.featureFlags:3\r\na=x-ss-general.liveResize:1\r\n";
        check(parseSdpAttributeToUInt(sdpWith, "x-ss-general.dynamicBitrate", &value) && value == 1,
              "dynamicBitrate attribute not parsed as 1");
        value = 5;
        check(!parseSdpAttributeToUInt(sdpWithout, "x-ss-general.dynamicBitrate", &value) && value == 5,
              "missing dynamicBitrate attribute reported as present");
    }

    // BITRATE_STATUS payload: id 1, requested 10000, accepted 10000, encoder 6000, APPLIED_RESTART
    {
        const char payload[20] = { 0x01, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
                                   0x70, 0x17, 0x00, 0x00, 0x01, 0x00, 0x55, 0x55 };
        memset(&status, 0, sizeof(status));
        check(parseBitrateStatusPayload(payload, 18, &status), "an 18 byte status was not parsed");
        check(status.requestId == 1 && status.requestedKbps == 10000 && status.acceptedKbps == 10000 &&
              status.encoderKbps == 6000 && status.status == LI_BITRATE_STATUS_APPLIED_RESTART,
              "status fields are wrong");
        check(parseBitrateStatusPayload(payload, 20, &status), "a longer status was not parsed");

        // Review Focus 5: a short payload must be dropped
        check(!parseBitrateStatusPayload(payload, 17, &status), "a 17 byte status was parsed");
        check(!parseBitrateStatusPayload(payload, 0, &status), "an empty status was parsed");
    }

    // Finished and lost frames. Frame 3 never arrives, frame 4 starts but does not complete.
    {
        LiGetVideoFrameCounters(&finished, &lost);
        check(finished == 0 && lost == 0, "frame counters do not start at 0");

        connectionSawFrame(1);
        connectionReceivedCompleteFrame(1, false);
        connectionSawFrame(2);
        connectionReceivedCompleteFrame(2, false);
        LiGetVideoFrameCounters(&finished, &lost);
        check(finished == 1 && lost == 0, "frame 1 is not counted as finished when frame 2 starts");

        connectionSawFrame(4);
        connectionSawFrame(5);
        LiGetVideoFrameCounters(&finished, &lost);
        check(finished == 4 && lost == 2, "frames 3 and 4 are not counted as lost");

        // The same frame again counts nothing
        connectionSawFrame(5);
        LiGetVideoFrameCounters(&finished, &lost);
        check(finished == 4 && lost == 2, "a repeated frame index changed the counters");
    }

    // A new connection clears the value of the previous connection. The start
    // fails at the audio configuration check, before any network operation.
    {
        SERVER_INFORMATION serverInfo;
        STREAM_CONFIGURATION streamConfig;

        LiInitializeServerInformation(&serverInfo);
        serverInfo.address = "127.0.0.1";
        serverInfo.serverInfoAppVersion = "7.1.431.-1";
        serverInfo.serverCodecModeSupport = SCM_H264;
        LiInitializeStreamConfiguration(&streamConfig);
        streamConfig.audioConfiguration = 0;

        DynamicBitrateSupported = true;
        check(LiStartConnection(&serverInfo, &streamConfig, NULL, NULL, NULL, NULL, 0, NULL, 0) != 0,
              "LiStartConnection() did not fail with an incorrect audio configuration");
        check(!LiIsDynamicBitrateSupported(), "LiStartConnection() did not clear the dynamic bitrate value");
    }

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }

    printf("bitrate_protocol_test: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr/moonlight-common-c/moonlight-common-c && cmake -S . -B /tmp/mcc-build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug >/dev/null && cmake --build /tmp/mcc-build -j >/dev/null && gcc -O0 -g -I src -I enet/include test/bitrate_protocol_test.c /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a -lcrypto -lpthread -o /tmp/bitrate_protocol_test && /tmp/bitrate_protocol_test'`
Expected: FAIL to compile with `unknown type name 'BITRATE_STATUS_PAYLOAD'` (and other missing names).

- [ ] **Step 3: Add the public API**

In `src/Limelight.h`, after the typedef `ConnListenerResizeRefused`:

```c

// This callback is invoked when the host answers a bitrate request that was sent
// with LiSendBitrateRequest(). The status is one of the LI_BITRATE_STATUS_* values.
// acceptedKbps is the configured bitrate after the host cap, and encoderKbps is the
// bitrate of the host encoder. For a refusal (status 3 to 6) both are the values
// that run now.
typedef void(*ConnListenerBitrateStatus)(uint32_t requestId, uint32_t requestedKbps, uint32_t acceptedKbps,
                                         uint32_t encoderKbps, uint16_t status);
```

In `CONNECTION_LISTENER_CALLBACKS`, after `ConnListenerResizeRefused resizeRefused;`:

```c
    ConnListenerBitrateStatus bitrateStatus;
```

After the declaration `int LiSendResizeRequest(uint16_t width, uint16_t height, uint32_t* requestId);`:

```c

// Status codes for ConnListenerBitrateStatus()
#define LI_BITRATE_STATUS_APPLIED 0
#define LI_BITRATE_STATUS_APPLIED_RESTART 1
#define LI_BITRATE_STATUS_UNCHANGED 2
#define LI_BITRATE_STATUS_NOT_SUPPORTED 3
#define LI_BITRATE_STATUS_INVALID 4
#define LI_BITRATE_STATUS_ENCODER_FAILED 5
#define LI_BITRATE_STATUS_INPUT_ONLY 6

// Returns true when the host advertised bitrate changes in the RTSP DESCRIBE reply
// (SDP attribute x-ss-general.dynamicBitrate). The value is valid while a connection
// is active. It is false before the RTSP handshake of the current connection.
bool LiIsDynamicBitrateSupported(void);

// Asks the host to change the video bitrate to configuredKbps. The unit is the
// configured bitrate (the same as StreamConfig.bitrate); the host makes the FEC,
// audio and overhead reductions. The host answers each request with
// ConnListenerBitrateStatus(). On success, *requestId receives the id of this request.
// Returns 0 when the request was sent, or -1 when the host does not support it, the
// control stream is not encrypted, the control stream is stopped or configuredKbps is 0.
// This function may only be called between LiStartConnection() and LiStopConnection().
// Do not call it at the same time as LiStopConnection(). Only one thread may call it.
int LiSendBitrateRequest(uint32_t configuredKbps, uint32_t* requestId);

// Writes the number of video frames that finished (completed or lost) and the number
// of lost frames since the start of the connection. A frame is finished when the
// next frame starts. The values wrap at 2^32. Any thread may call this function; a
// value can be one frame old.
void LiGetVideoFrameCounters(uint32_t* finishedFrames, uint32_t* lostFrames);
```

In `src/Limelight-internal.h`, after `extern bool LiveResizeSupported;`:

```c
extern bool DynamicBitrateSupported;
```

After the declaration `bool parseSdpAttributeToUInt(const char* payload, const char* name, uint32_t* val);`:

```c

// ControlStream.c: payload of BITRATE_STATUS (Apollo adaptive bitrate extension)
#define BITRATE_STATUS_PAYLOAD_LENGTH 18
typedef struct _BITRATE_STATUS_PAYLOAD {
    uint32_t requestId;
    uint32_t requestedKbps;
    uint32_t acceptedKbps;
    uint32_t encoderKbps;
    uint16_t status;
} BITRATE_STATUS_PAYLOAD, *PBITRATE_STATUS_PAYLOAD;

// Reads a little endian BITRATE_STATUS payload. Returns false when length is less than
// BITRATE_STATUS_PAYLOAD_LENGTH; the caller then drops the message.
bool parseBitrateStatusPayload(const char* payload, int length, PBITRATE_STATUS_PAYLOAD status);
```

In `src/Connection.c`, after `bool LiveResizeSupported;`:

```c
bool DynamicBitrateSupported;
```

and in `LiStartConnection`, after `LiveResizeSupported = false;`:

```c
    DynamicBitrateSupported = false;
```

In `src/Misc.c`, after `LiIsLiveResizeSupported()`:

```c

bool LiIsDynamicBitrateSupported(void) {
    return DynamicBitrateSupported;
}
```

In `src/FakeCallbacks.c`, after `static void fakeClResizeRefused(...) {}`:

```c
static void fakeClBitrateStatus(uint32_t requestId, uint32_t requestedKbps, uint32_t acceptedKbps, uint32_t encoderKbps, uint16_t status) {}
```

in `fakeClCallbacks`, after `.resizeRefused = fakeClResizeRefused,`:

```c
    .bitrateStatus = fakeClBitrateStatus,
```

and in `fixupMissingCallbacks`, after the `resizeRefused == NULL` block:

```c
        if ((*clCallbacks)->bitrateStatus == NULL) {
            (*clCallbacks)->bitrateStatus = fakeClBitrateStatus;
        }
```

In `src/RtspConnection.c`, after the live resize attribute block in `performRtspHandshake`:

```c

        // Look for the adaptive bitrate attribute (Apollo extension)
        {
            uint32_t dynamicBitrate;
            if (parseSdpAttributeToUInt(response.payload, "x-ss-general.dynamicBitrate", &dynamicBitrate)) {
                DynamicBitrateSupported = (dynamicBitrate != 0);
            }
            else {
                DynamicBitrateSupported = false;
            }
        }
```

- [ ] **Step 4: Add the request, the status callback and the frame counters**

In `src/ControlStream.c`:

After the `SS_RESIZE_REQUEST` struct (inside the same `#pragma pack(push, 1)` area, before `#pragma pack(pop)`):

```c

// Payload of SET_BITRATE (Apollo adaptive bitrate extension), little endian on the wire
typedef struct _SS_BITRATE_REQUEST {
    uint32_t requestId;
    uint32_t configuredKbps;
} SS_BITRATE_REQUEST, *PSS_BITRATE_REQUEST;
```

In the `data` union of `QUEUED_ASYNC_CALLBACK`, after the `resizeRefused` struct:

```c
        BITRATE_STATUS_PAYLOAD bitrateStatus;
```

After `static uint32_t nextResizeRequestId;`:

```c
static uint32_t nextBitrateRequestId;

// Cumulative video frame counters for the adaptive bitrate controller. Only the video
// receive thread writes them; LiGetVideoFrameCounters() reads them with no lock.
static uint32_t finishedFrameCount;
static uint32_t lostFrameCount;
```

After `#define IDX_RESIZE_REFUSED 14`:

```c
#define IDX_SET_BITRATE 15
#define IDX_BITRATE_STATUS 16
```

In `packetTypesGen3`, `packetTypesGen4`, `packetTypesGen5` and `packetTypesGen7`, after the line `-1,     // Resize refused (unused)`:

```c
    -1,     // Set bitrate (unused)
    -1,     // Bitrate status (unused)
```

In `packetTypesGen7Enc`, after `0x3101, // Resize refused (Apollo live resize extension)`:

```c
    0x3102, // Set bitrate (Apollo adaptive bitrate extension)
    0x3103, // Bitrate status (Apollo adaptive bitrate extension)
```

In `initializeControlStream()`, after `nextResizeRequestId = 1;`:

```c
    nextBitrateRequestId = 1;
```

and after `lastSeenFrame = 0;`:

```c
    finishedFrameCount = 0;
    lostFrameCount = 0;
```

In `connectionReceivedCompleteFrame()` nothing changes (it sets `lastGoodFrame`). In `connectionSawFrame()`, replace:

```c
    LC_ASSERT_VT(!isBefore16(frameIndex, lastSeenFrame));

    uint64_t now = PltGetMillis();
```

with:

```c
    LC_ASSERT_VT(!isBefore16(frameIndex, lastSeenFrame));

    // Count before the early returns below. The frames from lastSeenFrame to frameIndex - 1
    // are finished now. A finished frame is lost unless connectionReceivedCompleteFrame()
    // reported it; frames after lastSeenFrame were never seen, thus lost.
    if (lastSeenFrame != 0 && frameIndex != lastSeenFrame) {
        uint32_t finished = frameIndex - lastSeenFrame;
        finishedFrameCount += finished;
        lostFrameCount += finished - (lastGoodFrame == lastSeenFrame ? 1 : 0);
    }

    uint64_t now = PltGetMillis();
```

After the function `LiSendResizeRequest(...)`:

```c

int LiSendBitrateRequest(uint32_t configuredKbps, uint32_t* requestId) {
    SS_BITRATE_REQUEST request;

    // The request goes only to a Sunshine-type host over the encrypted control stream
    if (!DynamicBitrateSupported || !IS_SUNSHINE() || !encryptedControlStream || configuredKbps == 0) {
        return -1;
    }

    if (packetTypes[IDX_SET_BITRATE] == -1 || peer == NULL || stopping) {
        return -1;
    }

    request.requestId = LE32(nextBitrateRequestId);
    request.configuredKbps = LE32(configuredKbps);

    if (!sendMessageAndForget(packetTypes[IDX_SET_BITRATE], sizeof(request), &request,
                              CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false)) {
        return -1;
    }

    *requestId = nextBitrateRequestId++;
    return 0;
}

bool parseBitrateStatusPayload(const char* payload, int length, PBITRATE_STATUS_PAYLOAD status) {
    BYTE_BUFFER bb;

    if (length < BITRATE_STATUS_PAYLOAD_LENGTH) {
        return false;
    }

    BbInitializeWrappedBuffer(&bb, (char*)payload, 0, length, BYTE_ORDER_LITTLE);
    return BbGet32(&bb, &status->requestId) &&
           BbGet32(&bb, &status->requestedKbps) &&
           BbGet32(&bb, &status->acceptedKbps) &&
           BbGet32(&bb, &status->encoderKbps) &&
           BbGet16(&bb, &status->status);
}

void LiGetVideoFrameCounters(uint32_t* finishedFrames, uint32_t* lostFrames) {
    // No lock: a torn or old value is acceptable, as in LiGetEstimatedRttInfo()
    *finishedFrames = finishedFrameCount;
    *lostFrames = lostFrameCount;
}
```

In `asyncCallbackThreadFunc()`, after the `case IDX_RESIZE_REFUSED: ... break;` block:

```c
        case IDX_BITRATE_STATUS:
            // Each request gets one answer; answers are not batched
            ListenerCallbacks.bitrateStatus(queuedCb->data.bitrateStatus.requestId,
                                            queuedCb->data.bitrateStatus.requestedKbps,
                                            queuedCb->data.bitrateStatus.acceptedKbps,
                                            queuedCb->data.bitrateStatus.encoderKbps,
                                            queuedCb->data.bitrateStatus.status);
            break;
```

In `needsAsyncCallback()`, replace `packetType == packetTypes[IDX_RESIZE_REFUSED];` with:

```c
           packetType == packetTypes[IDX_RESIZE_REFUSED] ||
           packetType == packetTypes[IDX_BITRATE_STATUS];
```

In `queueAsyncCallback()`, after the `else if (ctlHdr->type == packetTypes[IDX_RESIZE_REFUSED]) { ... }` block:

```c
    else if (ctlHdr->type == packetTypes[IDX_BITRATE_STATUS]) {
        // A short payload would leave fields unset. Drop it (Review Focus 5).
        if (!parseBitrateStatusPayload((char*)(ctlHdr + 1), packetLength - (int)sizeof(*ctlHdr),
                                       &queuedCb->data.bitrateStatus)) {
            Limelog("Dropping a short bitrate status message (%d bytes)\n", packetLength - (int)sizeof(*ctlHdr));
            free(queuedCb);
            return;
        }

        queuedCb->typeIndex = IDX_BITRATE_STATUS;
    }
```

- [ ] **Step 5: Count recovered video packets**

In `src/RtpVideoQueue.c` `reconstructFrame()`, replace:

```c
    if (queue->bufferDataPackets != queue->receivedDataPackets) {
#ifdef FEC_VERBOSE
```

with:

```c
    if (queue->bufferDataPackets != queue->receivedDataPackets) {
        // The adaptive bitrate controller reads this with LiGetRTPVideoStats(). Before this
        // change only the audio queue counted recovered packets.
        if (ret == 0) {
            queue->stats.packetCountFecRecovered += queue->bufferDataPackets - queue->receivedDataPackets;
        }

#ifdef FEC_VERBOSE
```

- [ ] **Step 6: Run the tests to see them pass**

```bash
toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr/moonlight-common-c/moonlight-common-c && cmake --build /tmp/mcc-build -j >/dev/null && for t in bitrate_protocol_test resize_protocol_test mic_crypto_test; do gcc -O0 -g -I src -I enet/include test/$t.c /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a -lcrypto -lpthread -o /tmp/$t && /tmp/$t || echo "$t FAILED"; done'
```

Expected: `bitrate_protocol_test: all checks passed`, `resize_protocol_test: all checks passed`, and the mic test pass line; no `FAILED`.

- [ ] **Step 7: Commit and push the submodule branch**

```bash
cd ~/GitHub/moonlight-qt-mic-abr/moonlight-common-c/moonlight-common-c
git add src/Limelight.h src/Limelight-internal.h src/ControlStream.c src/RtpVideoQueue.c src/RtspConnection.c src/Connection.c src/Misc.c src/FakeCallbacks.c test/bitrate_protocol_test.c
git commit -m "Add the bitrate request and status messages and the network counters"
git push -u origin adaptive-bitrate
```

Expected: the branch is on `Catapultam-GMG/moonlight-common-c-mic`. Task 9 moves the client pointer to this commit. At the merge (Task 11, Step 9) the submodule branch goes into the submodule `master` first.

---

### Task 7: Client: the adaptive bitrate controller

**Files:**
- Create: `~/GitHub/moonlight-qt-mic-abr/app/streaming/adaptivebitrate.h`
- Create: `~/GitHub/moonlight-qt-mic-abr/app/tests/adaptivebitrate_test.cpp`
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/app.pro` (HEADERS, after `streaming/liveresize.h \`)

**Interfaces:**
- Consumes: nothing (the status values equal `LI_BITRATE_STATUS_*` of Task 6 by definition).
- Produces (Task 9 uses these exact names):
  - `namespace AdaptiveBitrate`: `enum Status : uint16_t { StatusApplied = 0, StatusAppliedRestart, StatusUnchanged, StatusNotSupported, StatusInvalid, StatusEncoderFailed, StatusInputOnly }`, `const char* statusName(uint16_t)`, constants (`TICK_MS` and the others below).
  - `struct Sample { uint64_t nowMs; uint32_t framesFinished; uint32_t framesLost; uint32_t packetsVideo; uint32_t packetsFecRecovered; uint32_t rttMs; double measuredMbps; }`.
  - `enum class Reason { None, Start, Loss, Delay, FecPressure, Increase, Ceiling }`, `const char* reasonName(Reason)`.
  - `struct Decision { bool send; uint32_t targetKbps; uint32_t fromKbps; Reason reason; bool isolated; bool rebased; bool timedOut; uint32_t timedOutRequestId; bool stopped; double lossPct[2]; uint32_t rttMs; uint32_t rttBaselineMs; double fecPct; double measuredMbps; }`.
  - `enum class StatusResult { Ignored, Accepted, Stopped }`.
  - `bool passesDeadBand(uint32_t fromKbps, uint32_t toKbps, uint32_t floorKbps, uint32_t ceilingKbps)`.
  - `int formatDecision(const Decision&, char* buffer, size_t length)`, `int formatOverlay(bool running, uint32_t targetKbps, uint32_t ceilingKbps, uint32_t encoderKbps, double measuredMbps, char* buffer, size_t length)`.
  - `class Controller`: `void start(uint32_t ceilingKbps, uint64_t nowMs, bool adaptive)`, `Decision tick(const Sample&)`, `void sent(const Decision&, uint32_t requestId, uint64_t nowMs)`, `StatusResult onStatus(uint32_t requestId, uint16_t status, uint32_t requestedKbps, uint32_t acceptedKbps, uint32_t encoderKbps, uint64_t nowMs)`, `Decision setCeiling(uint32_t ceilingKbps, uint64_t nowMs)`, `void notifySettle(uint64_t nowMs, uint32_t durationMs)`, and `started()`, `running()`, `adaptive()`, `pending()`, `restartMode()`, `targetKbps()`, `ceilingKbps()`, `encoderKbps()`, `floorKbps()`, `minTargetKbps()`, `maxTargetKbps()`, `meanTargetKbps()`.
  - Spec 4.1 named a method `onTimeout(requestId)`: here `tick()` detects the timeout and reports it in `Decision::timedOut` (spec 6.2: "the controller checks REQUEST_TIMEOUT_MS in its tick").

- [ ] **Step 1: Write the failing test**

Create `app/tests/adaptivebitrate_test.cpp`:

```cpp
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
    assert(sim.c.targetKbps() == 40000);
    for (int i = 0; i < 12; i++) {
        assert(!sim.step(1).send);  // RESTART_SETTLE_MS
    }
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
    testRecovery();
    testAppLimited();
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
    testNotAdaptive();
    testCounterWrap();
    testTexts();
    puts("adaptivebitrate_test: all checks passed");
    return 0;
}
```

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/adaptivebitrate_test.cpp -o /tmp/adaptivebitrate_test && /tmp/adaptivebitrate_test'`
Expected: FAIL with `streaming/adaptivebitrate.h: No such file or directory`.

- [ ] **Step 3: Write the controller**

Create `app/streaming/adaptivebitrate.h`:

```cpp
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
    Stopped,   // NOT_SUPPORTED, INPUT_ONLY or INVALID: the controller stops for the session
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
    // call does nothing (spec 4.4). adaptive == false: no rule runs, and only
    // setCeiling() makes requests (spec D12).
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
        }

        // Rule 1: a pending request or a settle time. The deltas of this time do
        // not count (an IDR frame or a full queue is not a network signal).
        if (m_Pending || now < m_SettleUntilMs) {
            clearHistory(now);
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

    // A BITRATE_STATUS answer (spec 3.5 and 4.5)
    StatusResult onStatus(uint32_t requestId, uint16_t status, uint32_t requestedKbps, uint32_t acceptedKbps,
                          uint32_t encoderKbps, uint64_t nowMs)
    {
        // Only the answer for the newest request counts, and only one time
        if (!running() || m_LastSentId == 0 || requestId != m_LastSentId || m_LastSentHandled) {
            return StatusResult::Ignored;
        }
        m_LastSentHandled = true;
        m_Pending = false;
        m_StartPhase = false;

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
        case StatusEncoderFailed:
            // The host runs the old values; the target goes back to them
            m_AcceptedKbps = acceptedKbps;
            m_EncoderKbps = encoderKbps;
            if (acceptedKbps > 0) {
                setTarget(acceptedKbps < m_Ceiling ? acceptedKbps : m_Ceiling);
            }
            notifySettle(nowMs, RESTART_SETTLE_MS);
            return StatusResult::Accepted;
        default:
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
```

In `app/app.pro`, after `    streaming/liveresize.h \` add:

```
    streaming/adaptivebitrate.h \
```

- [ ] **Step 4: Run the test to see it pass**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/adaptivebitrate_test.cpp -o /tmp/adaptivebitrate_test && /tmp/adaptivebitrate_test && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test && /tmp/liveresize_test'`
Expected: `adaptivebitrate_test: all checks passed` and `liveresize_test: all checks passed`.

When an assert fails, find the cause in the controller, not in the test. The test values follow from the spec rules and the plan deviations; the reviewer checks any change of a test value against the spec.

- [ ] **Step 5: Commit**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
git add app/streaming/adaptivebitrate.h app/tests/adaptivebitrate_test.cpp app/app.pro
git commit -m "Add the adaptive bitrate controller and its test"
```

---

### Task 8: Client: the "Adapt bitrate to the network" setting

**Files:**
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/settings/streamingpreferences.h` (`Q_PROPERTY`, member, signal)
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/settings/streamingpreferences.cpp` (key, `reload()`, `save()`)
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/gui/SettingsView.qml` (check box under the bitrate slider)
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/cli/commandlineparser.cpp` (toggle option, `--bitrate` fix)

**Interfaces:**
- Consumes: nothing.
- Produces (Task 9 uses it): `bool StreamingPreferences::adaptiveBitrate` (default `true`), signal `adaptiveBitrateChanged()`, QML property `StreamingPreferences.adaptiveBitrate`. `--bitrate <kbps>` now also sets `autoAdjustBitrate = false`.

The app has no test target for Qt code. The check is the build, the QML load and the command line help (Step 5).

- [ ] **Step 1: Add the preference**

In `app/settings/streamingpreferences.h`, after `Q_PROPERTY(bool autoAdjustBitrate MEMBER autoAdjustBitrate NOTIFY autoAdjustBitrateChanged)`:

```cpp
    Q_PROPERTY(bool adaptiveBitrate MEMBER adaptiveBitrate NOTIFY adaptiveBitrateChanged)
```

after the member `bool autoAdjustBitrate;`:

```cpp
    // Lower the bitrate on network congestion and raise it again up to the
    // target (adaptive bitrate spec D12). Not the same as autoAdjustBitrate.
    bool adaptiveBitrate;
```

and after the signal `void autoAdjustBitrateChanged();`:

```cpp
    void adaptiveBitrateChanged();
```

In `app/settings/streamingpreferences.cpp`, after `#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"`:

```cpp
#define SER_ADAPTIVEBITRATE "adaptivebitrate"
```

in `reload()`, after `autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();`:

```cpp
    adaptiveBitrate = settings.value(SER_ADAPTIVEBITRATE, true).toBool();
```

and in `save()`, after `settings.setValue(SER_AUTOADJUSTBITRATE, autoAdjustBitrate);`:

```cpp
    settings.setValue(SER_ADAPTIVEBITRATE, adaptiveBitrate);
```

- [ ] **Step 2: Add the check box**

In `app/gui/SettingsView.qml`, after the `Row { ... }` that holds `Slider { id: slider ... }` and `Button { id: resetBitrateButton ... }`, and before `Label { ... id: windowModeTitle ... }`:

```qml
                CheckBox {
                    id: adaptiveBitrateCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Adapt bitrate to the network")
                    font.pointSize: 12
                    checked: StreamingPreferences.adaptiveBitrate
                    onCheckedChanged: {
                        StreamingPreferences.adaptiveBitrate = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Lower the bitrate when the network is congested and raise it again up to the selected bitrate.")
                }

```

- [ ] **Step 3: Add the command line option and fix --bitrate**

In `app/cli/commandlineparser.cpp`, after `parser.addValueOption("bitrate", "bitrate in Kbps");`:

```cpp
    parser.addToggleOption("adaptive-bitrate", "network adaptive bitrate");
```

Replace:

```cpp
    if (parser.isSet("bitrate")) {
        preferences->bitrateKbps = parser.getIntOption("bitrate");
```

with:

```cpp
    if (parser.isSet("bitrate")) {
        preferences->bitrateKbps = parser.getIntOption("bitrate");
        // A bitrate from the command line is a manual value: it is the ceiling of the
        // adaptive bitrate (spec D11), not the default for the stream size
        preferences->autoAdjustBitrate = false;
```

After the line `preferences->muteOnFocusLoss = parser.getToggleOptionValue("mute-on-focus-loss", preferences->muteOnFocusLoss);` add:

```cpp

    // Resolve --adaptive-bitrate and --no-adaptive-bitrate options
    preferences->adaptiveBitrate = parser.getToggleOptionValue("adaptive-bitrate", preferences->adaptiveBitrate);
```

- [ ] **Step 4: Build**

Task 6 changed `Limelight.h`, so clean `app/` first (first build of this worktree: the clean step only prints that no makefile exists, which is fine).

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && qmake6 moonlight-qt.pro && (cd app && make -f Makefile.Release clean >/dev/null 2>&1; true) && make release -j$(nproc) 2>&1 | grep -E "error" ; ls -la app/moonlight && ldd app/moonlight | grep placebo'`
Expected: no `error` line; the binary exists; one `libplacebo` line.

- [ ] **Step 5: Check the option and the settings page**

```bash
toolbox run -c moonlight bash -lc 'QT_QPA_PLATFORM=offscreen ~/GitHub/moonlight-qt-mic-abr/app/moonlight stream --help 2>&1 | grep -i -A1 "adaptive"'
```

Expected: `--adaptive-bitrate  Use network adaptive bitrate.` and `--no-adaptive-bitrate  Do not use network adaptive bitrate.` (the options of an action show only in `<action> --help`).

Start the app (`toolbox run -c moonlight env QT_QPA_PLATFORM=wayland ~/GitHub/moonlight-qt-mic-abr/app/moonlight`), open Settings: the check box "Adapt bitrate to the network" is under the bitrate slider and is checked; the tool tip shows the text of Step 2; uncheck it, close and open the app again: it stays unchecked; check it again. The QML log on stderr has no `TypeError` or `ReferenceError` line.

- [ ] **Step 6: Commit**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
git add app/settings/streamingpreferences.h app/settings/streamingpreferences.cpp app/gui/SettingsView.qml app/cli/commandlineparser.cpp
git commit -m "Add the adapt bitrate to the network setting"
```

---

### Task 9: Client: session integration, overlay and logs

**Files:**
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/streaming/session.h`
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/streaming/session.cpp`
- Modify: `~/GitHub/moonlight-qt-mic-abr/app/streaming/video/ffmpeg.cpp` (`stringifyVideoStats`, `logVideoStats`)
- Modify: `~/GitHub/moonlight-qt-mic-abr/moonlight-common-c/moonlight-common-c` (submodule pointer, to the commit of Task 6)

**Interfaces:**
- Consumes: Task 6 (`LiIsDynamicBitrateSupported()`, `LiSendBitrateRequest()`, `LiGetVideoFrameCounters()`, `LiGetRTPVideoStats()`, `ConnListenerBitrateStatus`, `LI_BITRATE_STATUS_*`), Task 7 (`AdaptiveBitrate::Controller`, `Sample`, `Decision`, `StatusResult`, `formatDecision()`, `formatOverlay()`, `statusName()`, `TICK_MS`, `START_ATTEMPTS`), Task 8 (`StreamingPreferences::adaptiveBitrate`). Existing: `StreamingPreferences::getDefaultBitrate()`, `BandwidthTracker` (`app/streaming/bandwidth.h`), `Session::applyStreamSize()`, `SDL_CODE_FIRST_FRAME_DECODED`.
- Produces: `int Session::formatBitrateStats(char* output, int length)` (any thread); the client log lines that Task 11 reads:
  - `Adaptive bitrate: <on|off (a live resize still sets the bitrate)>, limit <kbps> kbps, stream start <kbps> kbps`
  - `Adaptive bitrate: the host does not support bitrate changes; the bitrate stays at <kbps> kbps`
  - `Adaptive bitrate: <from> -> <to> kbps, reason <reason> (loss ..., rtt .../... ms, fec ...%, measured ... Mbps), request <id>`
  - `Adaptive bitrate: request <id> <STATUS>, accepted <kbps> kbps, host encoder <kbps> kbps`
  - `Adaptive bitrate: no key frame after the in-place change of request <id>` (warning `... <n> key frames in about 1 s after the in-place change of request <id>`)
  - `Adaptive bitrate: the limit for <w>x<h> is <kbps> kbps`
  - `Adaptive bitrate: target lowest <kbps>, highest <kbps>, mean <kbps> kbps`

This task has no unit test: the logic is in the controller (Task 7). The checks are the build and Task 11.

- [ ] **Step 1: Declare the session parts**

In `app/streaming/session.h`, after `#include "liveresize.h"`:

```cpp
#include "adaptivebitrate.h"
#include "bandwidth.h"

#include <atomic>
#include <mutex>
#include <vector>
```

In the public part, after `void requestLiveResize();`:

```cpp

    // Writes the bitrate line of the stats overlay (adaptive bitrate spec 6.5).
    // Any thread may call it. Returns the snprintf() result.
    int formatBitrateStats(char* output, int length);
```

In the private part, after the declaration of `clResizeRefused(...)`:

```cpp

    static
    void clBitrateStatus(uint32_t requestId, uint32_t requestedKbps, uint32_t acceptedKbps, uint32_t encoderKbps, uint16_t status);

    static
    Uint32 bitrateTickTimerCallback(Uint32 interval, void* param);

    // Adaptive bitrate (spec section 6). All run on the main thread.
    // The target for the current stream size (spec 6.1)
    uint32_t calculateBitrateCeiling();
    // Starts the controller at the first decoded frame of the session (spec 4.4)
    void startAdaptiveBitrate();
    // One controller tick
    void bitrateTick();
    // Sends the request of a decision and logs it
    void sendBitrateRequest(const AdaptiveBitrate::Decision& decision);
    // Handles the answers that clBitrateStatus() queued
    void handleBitrateStatus();
    // A new ceiling after a live resize (spec 4.6)
    void updateBitrateCeiling();
    // Copies the controller state for the overlay and the connection warning
    void publishBitrateState();
```

After the member `uint32_t m_StatusOverlayGeneration;`:

```cpp

    // Adaptive bitrate (spec section 6). The controller and the timer belong to the main thread.
    AdaptiveBitrate::Controller m_BitrateController;
    SDL_TimerID m_BitrateTickTimer;
    bool m_BitrateUnsupportedLogged;
    // Received video bytes of the session; all decoders feed it in drSubmitDecodeUnit()
    BandwidthTracker m_BitrateTracker;
    // Copies of the controller state for other threads (overlay, connection warning)
    std::atomic<uint32_t> m_BitrateTargetKbps;
    std::atomic<uint32_t> m_BitrateCeilingKbps;
    std::atomic<uint32_t> m_BitrateEncoderKbps;
    std::atomic<bool> m_BitrateRunning;
    // Key frames that arrived (spike S1 check on the real path: an in-place change must not make one)
    std::atomic<uint32_t> m_KeyFrameCount;
    uint32_t m_KeyFrameMark;
    uint32_t m_KeyFrameMarkRequestId;
    uint64_t m_KeyFrameCheckMs;
    uint32_t m_KeyFrameCheckRequestId;
    // Answers from clBitrateStatus() (async callback thread) for the main thread. A vector
    // and not a heap object per SDL event: events left in the queue at the end would leak.
    struct BitrateStatusMessage {
        uint32_t requestId;
        uint32_t requestedKbps;
        uint32_t acceptedKbps;
        uint32_t encoderKbps;
        uint16_t status;
    };
    std::mutex m_BitrateStatusLock;
    std::vector<BitrateStatusMessage> m_BitrateStatusQueue;
```

- [ ] **Step 2: Event codes, callback table, constructor and the connection warning**

In `app/streaming/session.cpp`, after the line `// SDL_CODE_FIRST_FRAME_DECODED is 111 (decoder.h)`:

```cpp
// Adaptive bitrate (adaptive bitrate spec 6.2)
#define SDL_CODE_BITRATE_TICK 120
#define SDL_CODE_BITRATE_STATUS 121
```

In `k_ConnCallbacks`, replace the last entry `Session::clResizeRefused` with:

```cpp
    Session::clResizeRefused,
    Session::clBitrateStatus
```

In the constructor initializer list, replace `m_StatusOverlayGeneration(0)` with:

```cpp
      m_StatusOverlayGeneration(0),
      m_BitrateTickTimer(0),
      m_BitrateUnsupportedLogged(false),
      m_BitrateTracker(10, 250),
      m_BitrateTargetKbps(0),
      m_BitrateCeilingKbps(0),
      m_BitrateEncoderKbps(0),
      m_BitrateRunning(false),
      m_KeyFrameCount(0),
      m_KeyFrameMark(0),
      m_KeyFrameMarkRequestId(0),
      m_KeyFrameCheckMs(0),
      m_KeyFrameCheckRequestId(0)
```

In `clConnectionStatusUpdate`, replace:

```cpp
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
```

with:

```cpp
        // Spec 6.6 (U5): when the controller runs, the client lowers the bitrate itself
        s_ActiveSession->m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate,
                                                            (!s_ActiveSession->m_BitrateRunning.load() && s_ActiveSession->m_StreamConfig.bitrate > 5000) ?
                                                                "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC");
```

- [ ] **Step 3: The callback, the timer and the controller functions**

After the function `Session::clResizeRefused(...)`:

```cpp

void Session::clBitrateStatus(uint32_t requestId, uint32_t requestedKbps, uint32_t acceptedKbps, uint32_t encoderKbps, uint16_t status)
{
    // Runs on the async callback thread. The main thread owns the controller.
    {
        std::lock_guard<std::mutex> lock(s_ActiveSession->m_BitrateStatusLock);
        s_ActiveSession->m_BitrateStatusQueue.push_back({requestId, requestedKbps, acceptedKbps, encoderKbps, status});
    }

    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_BITRATE_STATUS;
    SDL_PushEvent(&event);
}

Uint32 Session::bitrateTickTimerCallback(Uint32 interval, void*)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_BITRATE_TICK;
    SDL_PushEvent(&event);

    // Repeats until the session removes it
    return interval;
}
```

After the function `Session::applyStreamSize(...)`:

```cpp

uint32_t Session::calculateBitrateCeiling()
{
    // Spec 6.1 and D11: a manual bitrate is the ceiling and does not change on a live resize
    if (!m_Preferences->autoAdjustBitrate) {
        return (uint32_t)m_Preferences->bitrateKbps;
    }

    // The default for the current stream size and the negotiated chroma format (D10)
    bool yuv444 = (m_ActiveVideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    return (uint32_t)StreamingPreferences::getDefaultBitrate(m_ActiveVideoWidth, m_ActiveVideoHeight,
                                                             m_ActiveVideoFrameRate, yuv444);
}

void Session::startAdaptiveBitrate()
{
    // Each new decoder sends SDL_CODE_FIRST_FRAME_DECODED. Only the first one of the
    // session starts the controller (spec 4.4); a live resize sets its own settle time.
    if (m_BitrateController.started()) {
        return;
    }

    if (!LiIsDynamicBitrateSupported()) {
        if (!m_BitrateUnsupportedLogged) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive bitrate: the host does not support bitrate changes; the bitrate stays at %d kbps",
                        m_StreamConfig.bitrate);
            m_BitrateUnsupportedLogged = true;
        }
        return;
    }

    uint32_t ceiling = calculateBitrateCeiling();
    m_BitrateController.start(ceiling, SDL_GetTicks64(), m_Preferences->adaptiveBitrate);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Adaptive bitrate: %s, limit %u kbps, stream start %d kbps",
                m_Preferences->adaptiveBitrate ? "on" : "off (a live resize still sets the bitrate)",
                ceiling, m_StreamConfig.bitrate);
    publishBitrateState();

    m_BitrateTickTimer = SDL_AddTimer(AdaptiveBitrate::TICK_MS, bitrateTickTimerCallback, nullptr);
}

void Session::bitrateTick()
{
    if (!m_BitrateController.running()) {
        return;
    }

    uint64_t now = SDL_GetTicks64();
    AdaptiveBitrate::Sample sample;
    sample.nowMs = now;
    LiGetVideoFrameCounters(&sample.framesFinished, &sample.framesLost);
    const RTP_VIDEO_STATS* rtpStats = LiGetRTPVideoStats();
    sample.packetsVideo = rtpStats->packetCountVideo;
    sample.packetsFecRecovered = rtpStats->packetCountFecRecovered;
    uint32_t rtt, rttVariance;
    sample.rttMs = LiGetEstimatedRttInfo(&rtt, &rttVariance) ? rtt : 0;
    sample.measuredMbps = m_BitrateTracker.GetAverageMbps();

    // Spike S1 on the real path: count the key frames in about 1 s after an in-place change
    if (m_KeyFrameCheckRequestId != 0 && now >= m_KeyFrameCheckMs) {
        uint32_t keyFrames = m_KeyFrameCount.load() - m_KeyFrameMark;
        if (keyFrames != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive bitrate: %u key frames in about 1 s after the in-place change of request %u",
                        keyFrames, m_KeyFrameCheckRequestId);
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive bitrate: no key frame after the in-place change of request %u",
                        m_KeyFrameCheckRequestId);
        }
        m_KeyFrameCheckRequestId = 0;
    }

    AdaptiveBitrate::Decision decision = m_BitrateController.tick(sample);
    if (decision.timedOut) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Adaptive bitrate: request %u got no answer in %u ms",
                    decision.timedOutRequestId, AdaptiveBitrate::REQUEST_TIMEOUT_MS);
    }
    if (decision.stopped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Adaptive bitrate: the host did not answer the first request %u times; adaptation is off for this session",
                    AdaptiveBitrate::START_ATTEMPTS);
    }
    if (decision.rebased) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Adaptive bitrate: the RTT stays at %u ms after a decrease; this is now the RTT baseline",
                    decision.rttBaselineMs);
    }
    if (decision.isolated) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "Adaptive bitrate: isolated loss or delay, no change (loss %.1f%% %.1f%%, rtt %u/%u ms)",
                     decision.lossPct[0], decision.lossPct[1], decision.rttMs, decision.rttBaselineMs);
    }
    if (decision.send) {
        sendBitrateRequest(decision);
    }
    publishBitrateState();
}

void Session::sendBitrateRequest(const AdaptiveBitrate::Decision& decision)
{
    uint32_t requestId;
    if (LiSendBitrateRequest(decision.targetKbps, &requestId) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Adaptive bitrate: could not send the request for %u kbps", decision.targetKbps);
        return;
    }
    m_BitrateController.sent(decision, requestId, SDL_GetTicks64());

    // A key frame of an in-place change can arrive before the answer: count from here
    m_KeyFrameMark = m_KeyFrameCount.load();
    m_KeyFrameMarkRequestId = requestId;

    char line[256];
    AdaptiveBitrate::formatDecision(decision, line, sizeof(line));
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s, request %u", line, requestId);
}

void Session::handleBitrateStatus()
{
    std::vector<BitrateStatusMessage> messages;
    {
        std::lock_guard<std::mutex> lock(m_BitrateStatusLock);
        messages.swap(m_BitrateStatusQueue);
    }

    for (const BitrateStatusMessage& message : messages) {
        uint64_t now = SDL_GetTicks64();
        const char* name = AdaptiveBitrate::statusName(message.status);
        switch (m_BitrateController.onStatus(message.requestId, message.status, message.requestedKbps,
                                             message.acceptedKbps, message.encoderKbps, now)) {
        case AdaptiveBitrate::StatusResult::Ignored:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive bitrate: old answer %s for request %u ignored", name, message.requestId);
            break;
        case AdaptiveBitrate::StatusResult::Accepted:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Adaptive bitrate: request %u %s, accepted %u kbps, host encoder %u kbps",
                        message.requestId, name, message.acceptedKbps, message.encoderKbps);
            if (message.status == AdaptiveBitrate::StatusApplied && message.requestId == m_KeyFrameMarkRequestId) {
                m_KeyFrameCheckMs = now + 1000;
                m_KeyFrameCheckRequestId = message.requestId;
            }
            break;
        case AdaptiveBitrate::StatusResult::Stopped:
            if (message.status == AdaptiveBitrate::StatusInvalid) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Adaptive bitrate: the host answered INVALID for request %u (%u kbps); adaptation is off for this session",
                             message.requestId, message.requestedKbps);
            }
            else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Adaptive bitrate: the host answered %s; adaptation is off for this session", name);
            }
            break;
        }
    }
    publishBitrateState();
}

void Session::updateBitrateCeiling()
{
    if (!m_BitrateController.running()) {
        return;
    }

    AdaptiveBitrate::Decision decision = m_BitrateController.setCeiling(calculateBitrateCeiling(), SDL_GetTicks64());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Adaptive bitrate: the limit for %dx%d is %u kbps",
                m_ActiveVideoWidth, m_ActiveVideoHeight, m_BitrateController.ceilingKbps());
    if (decision.send) {
        // Spec 4.6: after the size change, not before; this bypasses rule 1 and the intervals
        sendBitrateRequest(decision);
    }
    publishBitrateState();
}

void Session::publishBitrateState()
{
    m_BitrateTargetKbps = m_BitrateController.targetKbps();
    m_BitrateCeilingKbps = m_BitrateController.ceilingKbps();
    m_BitrateEncoderKbps = m_BitrateController.encoderKbps();
    m_BitrateRunning = m_BitrateController.running() && m_BitrateController.adaptive();
}

int Session::formatBitrateStats(char* output, int length)
{
    uint32_t target = m_BitrateTargetKbps.load();
    if (target == 0) {
        // No controller: the stream keeps its start bitrate
        target = (uint32_t)m_StreamConfig.bitrate;
    }
    return AdaptiveBitrate::formatOverlay(m_BitrateRunning.load(), target, m_BitrateCeilingKbps.load(),
                                          m_BitrateEncoderKbps.load(), m_BitrateTracker.GetAverageMbps(),
                                          output, (size_t)length);
}
```

- [ ] **Step 4: Hook the events, the decoder data and the resize**

In `Session::applyStreamSize(...)`, after `m_InputHandler->setStreamSize(width, height);`:

```cpp

    // Adaptive bitrate: the target for the new size (spec 4.6, D13)
    updateBitrateCeiling();
```

In `Session::drSubmitDecodeUnit(PDECODE_UNIT du)`, as the first statements of the function:

```cpp
    // Adaptive bitrate: the measured bitrate and the key frame count cover all decoders
    s_ActiveSession->m_BitrateTracker.AddBytes(du->fullLength);
    if (du->frameType == FRAME_TYPE_IDR) {
        s_ActiveSession->m_KeyFrameCount++;
    }

```

In the event loop `switch (event.user.code)`, in `case SDL_CODE_FIRST_FRAME_DECODED:`, after `pumpLiveResize();` and before `break;`:

```cpp
                startAdaptiveBitrate();
```

Before `default:` of the same switch:

```cpp
            case SDL_CODE_BITRATE_TICK:
                bitrateTick();
                break;
            case SDL_CODE_BITRATE_STATUS:
                handleBitrateStatus();
                break;
```

At `DispatchDeferredCleanup:`, after the block that removes `m_StatusOverlayTimer`:

```cpp

    // Stop the adaptive bitrate timer before this object can go away
    if (m_BitrateTickTimer != 0) {
        SDL_RemoveTimer(m_BitrateTickTimer);
        m_BitrateTickTimer = 0;
    }
    if (m_BitrateController.started()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Adaptive bitrate: target lowest %u, highest %u, mean %u kbps",
                    m_BitrateController.minTargetKbps(), m_BitrateController.maxTargetKbps(),
                    m_BitrateController.meanTargetKbps());
    }
```

- [ ] **Step 5: The overlay line**

In `app/streaming/video/ffmpeg.cpp` `stringifyVideoStats`, inside `if (m_VideoDecoderCtx != nullptr) { ... }`, after the existing `offset += ret;` of the "Video stream" line:

```cpp

            // Adaptive bitrate line (spec 6.5), outside DISPLAY_BITRATE
            Session* session = Session::get();
            if (session != nullptr) {
                ret = session->formatBitrateStats(&output[offset], length - offset);
                if (ret < 0 || ret >= length - offset) {
                    SDL_assert(false);
                    return;
                }
                offset += ret;
            }
```

In `logVideoStats`, change `char videoStatsStr[512];` to `char videoStatsStr[1024];` (the stats text is near 500 characters already; the overlay buffer is 1024, `overlaymanager.h`).

- [ ] **Step 6: Move the submodule pointer, build and check**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
git -C moonlight-common-c/moonlight-common-c log --oneline -1
toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && qmake6 moonlight-qt.pro && (cd app && make -f Makefile.Release clean >/dev/null 2>&1; true) && make release -j$(nproc) 2>&1 | grep -E "error|warning: .*(adaptive|bitrate|Bitrate)" ; ls -la app/moonlight && ldd app/moonlight | grep placebo'
grep -n "SDL_CODE_BITRATE\|startAdaptiveBitrate\|updateBitrateCeiling\|handleBitrateStatus\|bitrateTick()" app/streaming/session.cpp
toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic-abr && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/adaptivebitrate_test.cpp -o /tmp/adaptivebitrate_test && /tmp/adaptivebitrate_test && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test && /tmp/liveresize_test'
```

Expected: the submodule log shows the commit of Task 6; no `error` line and no warning about the new code; one `libplacebo` line; the grep shows the two defines, the event cases, the calls in `SDL_CODE_FIRST_FRAME_DECODED` and `applyStreamSize`; both tests pass.

- [ ] **Step 7: Commit**

```bash
cd ~/GitHub/moonlight-qt-mic-abr
git add app/streaming/session.h app/streaming/session.cpp app/streaming/video/ffmpeg.cpp moonlight-common-c/moonlight-common-c
git commit -m "Run the adaptive bitrate controller in the session"
git push -u origin adaptive-bitrate
```

---

### Task 10: Host deploy on CPLT-4A through master and ApolloUpdate

Do this task only after the whole-branch review of the host branch passed and the user said yes to the merge into host `master` (Step 1). The host change is backward compatible: a client without the feature never sends `SET_BITRATE`, and the SDP attribute is new.

**Files:**
- None in the repositories (a merge and a push). Notes: `~/GitHub/homelab-notes/apollo-cplt-4a.md`.

**Interfaces:**
- Consumes: branch `adaptive-bitrate` of `catapultam/apollo-microphone` (Tasks 3 to 5); the updater (`scripts/updater/README.md`: task `ApolloUpdate`, channel `master`, log `C:\ApolloBackup\update\ApolloUpdate.log`, it skips when a stream is active); the agentbus session `Armor-Console` on `cplt-4a`.
- Produces: the new host running on CPLT-4A (`sunshine.exe` ProductVersion ends with the short sha of the `master` commit); the line "Current build" in the notes.

- [ ] **Step 1: Ask the user for the merge**

Tell the user: "The host branch adaptive-bitrate passed the review and the Windows build. The deploy path is: merge into master, push, CI publishes a signed release, ApolloUpdate installs it. Can I merge adaptive-bitrate into host master and push?" Wait for the answer. If the answer is no, use the fallback of Step 5 with the prerelease of the last `workflow_dispatch` run of the branch.

- [ ] **Step 2: Merge and push**

```bash
cd ~/GitHub/apollo-microphone-abr
git fetch origin
git status --short | head -5
if git merge-base --is-ancestor origin/master HEAD; then echo "fast-forward"; else git merge --no-ff origin/master -m "Merge master into adaptive-bitrate"; fi
toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone-abr && for t in test_adaptive_bitrate test_live_resize; do g++ -std=c++20 -Wall -Werror -I . -I third-party/googletest/googletest/include tests/unit/$t.cpp -L /tmp/gtest-build -lgtest -lgtest_main -pthread -o /tmp/$t && /tmp/$t | tail -1; done'
git push origin adaptive-bitrate
OLD=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b master -L 1 --json databaseId -q '.[0].databaseId'); echo "$OLD" > /tmp/abr-master-old-run
git push origin HEAD:master
git rev-parse --short=7 HEAD
```

Expected: a clean status; both tests `[  PASSED  ]`; both pushes succeed. When the merge of `origin/master` had conflicts or brought new commits, run the `workflow_dispatch` build of Task 5 Step 9 before the push to `master`. Record the short sha (7 characters).

- [ ] **Step 3: Wait for the signed release**

```bash
OLD=$(cat /tmp/abr-master-old-run)
for i in $(seq 1 60); do RUN=$(gh run list -R catapultam/apollo-microphone -w build-windows.yml -b master -L 1 --json databaseId -q '.[0].databaseId'); [ -n "$RUN" ] && [ "$RUN" != "$OLD" ] && break; done; echo "$RUN"
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
gh release list -R catapultam/apollo-microphone -L 3
gh release view "$(gh release list -R catapultam/apollo-microphone -L 1 --json tagName -q '.[0].tagName')" -R catapultam/apollo-microphone --json body,assets -q '.body, (.assets[].name)'
```

Expected: the run passes (build and publish jobs); the newest release `build-<n>-<short sha>` has `branch: master`, `commit: <full sha of HEAD>` and the assets `Apollo.exe`, `SHA256SUMS`, `SHA256SUMS.sig`.

- [ ] **Step 4: Run the updater on CPLT-4A**

Make sure that no Moonlight stream runs (the updater skips an active stream and exits with 0 without an install). Send this to the `Armor-Console` session (address from ListAgents, `agentbus:cplt-4a/Armor-Console...`), and wait for each answer:

```text
Please run this on CPLT-4A and send me the output. It starts the Apollo updater task (it runs as SYSTEM, no UAC):
schtasks /run /tn ApolloUpdate
```

Wait about 4 minutes with Monitor (an until-loop on the time; not a question to the user), then send:

```text
Please run these and send me the complete output:
schtasks /query /tn ApolloUpdate /v /fo list | findstr /C:"Last Result" /C:"Status" /C:"Last Run Time"
Get-Content C:\ApolloBackup\update\ApolloUpdate.log -Tail 30
(Get-Item 'C:\Program Files\Apollo\sunshine.exe').VersionInfo.ProductVersion
Get-Service ApolloService | Select-Object Status
```

Expected: `Last Result: 0`, the log shows the download, the signature and hash checks, the install and the service start of the new tag; the ProductVersion ends with the short sha of Step 2 (for example `0.0.0.<sha>.dirty`); the service is `Running`. When `Status` is `Running`, wait 2 more minutes and ask again. When the log says that a stream is active, ask the user to end the stream, then run Step 4 again. When `Last Result` is 1, 2 or 3, read the log lines with `ERROR` and go to Step 5.

- [ ] **Step 5: Fallback: manual install from a stream**

Only when Step 4 failed. Tell the user: "The automatic update did not install. Please start a stream to CPLT-4A (moonlight-mic, Desktop) and tell me when you see the desktop." Then send to `Armor-Console`:

```text
Please run these one at a time and send me the output:
$tag = '<tag of Step 3>'
New-Item -ItemType Directory -Force -Path "$env:TEMP\abr-install" | Out-Null
Invoke-WebRequest -Uri "https://github.com/catapultam/apollo-microphone/releases/download/$tag/Apollo.exe" -OutFile "$env:TEMP\abr-install\Apollo.exe"
Invoke-WebRequest -Uri "https://github.com/catapultam/apollo-microphone/releases/download/$tag/SHA256SUMS" -OutFile "$env:TEMP\abr-install\SHA256SUMS"
(Get-FileHash "$env:TEMP\abr-install\Apollo.exe" -Algorithm SHA256).Hash.ToLower(); Get-Content "$env:TEMP\abr-install\SHA256SUMS"
```

When the two hashes are equal, send:

```text
Please run this. A UAC prompt comes up on the desktop; the user approves it from the stream. The stream ends when the service restarts.
Start-Process -FilePath "$env:TEMP\abr-install\Apollo.exe" -ArgumentList '/S' -Verb RunAs
```

Tell the user to approve the UAC prompt in the stream and to reconnect after about one minute. Then run the version and service commands of Step 4. Rollback, if the new host does not stream: `scripts/updater/README.md`, "Manual rollback" (newest `C:\ApolloBackup\auto-<timestamp>.zip`).

- [ ] **Step 6: Check the feature flag with the new client**

Start the client with the Bash option `run_in_background: true`:

```bash
toolbox run -c moonlight env QT_QPA_PLATFORM=wayland SDL_AUDIO_DRIVER=pulseaudio ~/GitHub/moonlight-qt-mic-abr/app/moonlight stream 10.10.10.232 Desktop --resolution 2560x1600 --fps 60 --video-codec HEVC --display-mode windowed > /tmp/abr-check.log 2>&1
```

Watch `/tmp/abr-check.log` with Monitor until two `Adaptive bitrate` lines are there (at most 60 s), then `grep -E "Adaptive bitrate" /tmp/abr-check.log | head -5`.

Expected: `Adaptive bitrate: on, limit <kbps> kbps, stream start <kbps> kbps` and, about 4 s later, a request with `reason start` and an answer `UNCHANGED` or `APPLIED`. Not the line "the host does not support bitrate changes". End the stream (close the window).

- [ ] **Step 7: Record the build in the notes**

In `~/GitHub/homelab-notes/apollo-cplt-4a.md`, replace the line `- **Current build: ...**` with the release tag, the short sha, the date and "branch master (live resize, updater, adaptive bitrate)". Add the line `- Install: push to master, CI publishes a signed release, then schtasks /run /tn ApolloUpdate (agentbus Armor-Console). See scripts/updater/README.md.` under "Install procedure", if the page does not have it yet.

```bash
cd ~/GitHub/homelab-notes
git add apollo-cplt-4a.md
git commit -m "Record the Apollo adaptive bitrate build on CPLT-4A"
git push
```

---

### Task 11: End-to-end test with a throttled network, notes and finish

**Files:**
- Create: `/var/home/catapultam/src/abr-spike/abr-shape` (not committed), installed as `/usr/local/sbin/abr-shape`
- Create: `/usr/local/sbin/tc` (from the `iproute-tc` package; the host has no `tc`)
- Modify: `~/GitHub/homelab-notes/apollo-cplt-4a.md` (section "Adaptive bitrate")
- Modify: `~/GitHub/moonlight-qt-mic-abr/docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md` (status line), `docs/superpowers/specs/2026-10-09-live-resize-design.md` (row D4), `docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md` (test results)

**Interfaces:**
- Consumes: the client of Task 9 (`~/GitHub/moonlight-qt-mic-abr/app/moonlight`), the host of Task 10, the client log lines of Task 9 Interfaces.
- Produces: the results of spec 8.3 in the plan, the notes page, the branches ready to merge.

The `tc` commands need root on the laptop host (a toolbox cannot change the host network). Silverblue has no `tc` (the `iproute-tc` package is not installed). Only packets from `10.10.10.232` go to the shaper, so other traffic is not throttled. Watch the client log with Monitor (`grep --line-buffered "Adaptive bitrate"` on `/tmp/abr-client.log`); do not ask the user to read logs.

- [ ] **Step 1: Write the shaping script**

Create `/var/home/catapultam/src/abr-spike/abr-shape`:

```bash
#!/bin/bash
# Shape the download from the Apollo host for the adaptive bitrate test (plan Task 11).
# Only packets from the host go through ifb0; other traffic of the laptop does not change.
# Usage (root): abr-shape on [rate] | rate <rate> | netem <netem arguments> | show | off
set -euo pipefail
TC=/usr/local/sbin/tc
HOST=10.10.10.232
IFACE=$(ip route get "$HOST" | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')
case "${1:-}" in
on)
    modprobe ifb numifbs=1
    ip link set ifb0 up
    $TC qdisc add dev "$IFACE" handle ffff: ingress
    $TC filter add dev "$IFACE" parent ffff: protocol ip u32 match ip src "$HOST"/32 action mirred egress redirect dev ifb0
    $TC qdisc add dev ifb0 root tbf rate "${2:-30mbit}" burst 64kb latency 50ms
    ;;
rate)
    $TC qdisc replace dev ifb0 root tbf rate "$2" burst 64kb latency 50ms
    ;;
netem)
    shift
    $TC qdisc replace dev ifb0 root netem "$@"
    ;;
show)
    echo "interface $IFACE"
    $TC -s qdisc show dev "$IFACE"
    $TC -s qdisc show dev ifb0 2>/dev/null || true
    ;;
off)
    $TC qdisc del dev "$IFACE" ingress 2>/dev/null || true
    $TC qdisc del dev ifb0 root 2>/dev/null || true
    ip link set ifb0 down 2>/dev/null || true
    ;;
*)
    echo "usage: abr-shape on [rate] | rate <rate> | netem <arguments> | show | off" >&2
    exit 2
    ;;
esac
```

```bash
chmod 0755 /var/home/catapultam/src/abr-spike/abr-shape
ip route get 10.10.10.232
```

Expected: the route line names the interface (on 2026-10-09: `dev enp95s0`, the wired dock link). Over Tailscale it is `tailscale0`; the script finds it itself.

- [ ] **Step 2: Get tc and install the two files (the user runs the sudo part)**

The toolbox and the host are both Fedora 44, so the toolbox binary runs on the host. `$HOME` is shared and keeps the binary intact.

```bash
toolbox run -c moonlight sudo dnf install -y iproute-tc
toolbox run -c moonlight cp /usr/sbin/tc /var/home/catapultam/src/abr-spike/tc
ls -la /var/home/catapultam/src/abr-spike/tc
ldd /var/home/catapultam/src/abr-spike/tc | grep "not found" || echo "tc libraries found on the host"
```

Expected: the file exists and `tc libraries found on the host`.

Ask the user to run this block in a terminal (it asks for the sudo password one time). The sudoers line lets Claude run only the root-owned script without a password; it is removed in Step 6. If the user does not want the sudoers line, the user runs each `sudo abr-shape ...` command of Step 4 when Claude asks.

```bash
sudo install -o root -g root -m 0755 /var/home/catapultam/src/abr-spike/tc /usr/local/sbin/tc
sudo install -o root -g root -m 0755 /var/home/catapultam/src/abr-spike/abr-shape /usr/local/sbin/abr-shape
echo 'catapultam ALL=(root) NOPASSWD: /usr/local/sbin/abr-shape' | sudo tee /etc/sudoers.d/abr-shape >/dev/null
sudo chmod 0440 /etc/sudoers.d/abr-shape && sudo visudo -cf /etc/sudoers.d/abr-shape
```

Then check: `sudo -n /usr/local/sbin/abr-shape show`. Expected: `interface enp95s0` and the qdisc list, without a password prompt.

- [ ] **Step 3: Start the test stream**

Ask the user to start a video or a game with motion on the host (a static desktop is app-limited, spec 4.3) and to keep the stream window in front. Start the client with the Bash option `run_in_background: true`:

```bash
toolbox run -c moonlight env QT_QPA_PLATFORM=wayland SDL_AUDIO_DRIVER=pulseaudio \
  ~/GitHub/moonlight-qt-mic-abr/app/moonlight stream 10.10.10.232 Desktop \
  --resolution 2560x1600 --fps 60 --video-codec HEVC --display-mode windowed --performance-overlay --adaptive-bitrate \
  > /tmp/abr-client.log 2>&1
```

Start a Monitor on `tail -F /tmp/abr-client.log | grep --line-buffered -E "Adaptive bitrate|Recreating|Connection status"`.

Expected: `Adaptive bitrate: on, limit 44000 kbps, stream start 44000 kbps` (2560x1600 at 60 fps; `getDefaultBitrate` gives 44000) and the start request with an answer. The overlay shows `Bitrate: target 44.0 Mbps (limit 44.0), host encoder 34.2 Mbps, measured ...`. If the limit is not 44000, the stored bitrate is manual (`autoAdjustBitrate` off): ask the user to press "Use Default" in the settings, then start again.

- [ ] **Step 4: Run the steps of spec 8.3**

Record each result (the log lines with time) for Step 7. "In place" means: answer `APPLIED` and the line `no key frame after the in-place change of request <id>`.

1. No throttle, 60 s. Expected: no request after the start request.
2. `sudo -n /usr/local/sbin/abr-shape on 30mbit`. Expected: within about 5 s a request with `reason loss` (or `delay`) to 33000 kbps or less; answer `APPLIED`; `no key frame after the in-place change`; no `Recreating` line. More requests can follow until the target is below about 30000 kbps.
3. `sudo -n /usr/local/sbin/abr-shape rate 15mbit`. Expected: the target follows down below about 15000 kbps.
4. `sudo -n /usr/local/sbin/abr-shape off`, 90 s. Expected: `reason increase` requests (8 % steps, 3 % near the last failure rate), each at least 4 s apart, up to 44000 kbps.
5. `sudo -n /usr/local/sbin/abr-shape on 1000mbit`, then `sudo -n /usr/local/sbin/abr-shape netem loss 3%`, 60 s. Expected: decreases with `reason loss` or `FEC`. Record how low the target goes (spec R5: random loss also lowers the bitrate; the floor is 1500 kbps). Then `sudo -n /usr/local/sbin/abr-shape netem delay 40ms`, 60 s. Expected: one `reason delay` decrease at most, then the line `the RTT stays at ... ms after a decrease; this is now the RTT baseline`, then increases. Then `sudo -n /usr/local/sbin/abr-shape off`.
6. Live resize: ask the user to make the stream window smaller and press `Ctrl+Alt+Shift+W`. Expected: `Video stream is now <w>x<h>`, `the limit for <w>x<h> is <kbps> kbps` with a lower value, a request with `reason new limit`. Make the window bigger, press the hotkey again: the limit and the target go up.
7. Host cap (optional, needs the user in the Apollo web UI on `https://10.10.10.232:47990`): set "Maximum bitrate" to 20000, restart the stream. Expected: the start answer has `accepted 20000 kbps` and the limit is 20000. Set it back to 0 after the test.
8. End the stream. In Settings uncheck "Adapt bitrate to the network" (or start with `--no-adaptive-bitrate`). Stream, use `abr-shape on 30mbit` for 30 s, then `abr-shape off`. Expected: `Adaptive bitrate: off (a live resize still sets the bitrate)` and no request; a live resize gives one `reason new limit` request. Check the box again.
9. Stock host (optional): stream to the Steam Machine (`10.10.11.27`, stock Sunshine). Expected: one line `the host does not support bitrate changes`.
10. Second client (optional, needs a second device such as a phone with Artemis): both stream from CPLT-4A; throttle only the laptop. Expected: the laptop lowers its bitrate; the other client's bitrate does not change.
11. Non-NVENC (optional, needs the user to set `encoder = software` in the web UI and restart the stream; set it back after the test): with `abr-shape on 30mbit`. Expected: answers `APPLIED_RESTART`, one key frame per change, decreases at least 3 s apart and increases at least 15 s apart.

At the end of the stream the log has `Adaptive bitrate: target lowest ..., highest ..., mean ... kbps`.

Then read the host warnings through `Armor-Console`:

```text
Please run this and send me the output:
Select-String -Path 'C:\Program Files\Apollo\config\sunshine.log' -Pattern 'NvEnc: the first frame after a bitrate change|NvEncReconfigureEncoder|Bitrate request .* encoder failed|runt payload' | Select-Object -Last 20 | ForEach-Object { $_.Line }
```

Expected: no line. A line `the first frame after a bitrate change is an IDR frame` means that S1 fails on the real path: stop, tell the user, and apply outcome C of Task 4 Step 5.

- [ ] **Step 5: Clean up the network shaping (always, also after a failure)**

```bash
sudo -n /usr/local/sbin/abr-shape off || echo "ask the user to run: sudo /usr/local/sbin/abr-shape off"
IFACE=$(ip route get 10.10.10.232 | awk '{for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit }}')
/usr/sbin/ip link show ifb0 2>/dev/null | head -1
/usr/local/sbin/tc qdisc show dev "$IFACE"
```

Expected: no `ingress` qdisc on the interface and `ifb0` is `DOWN` (or absent).

- [ ] **Step 6: Remove the root access**

Ask the user to run: `sudo rm /etc/sudoers.d/abr-shape` (keep `/usr/local/sbin/tc` and `/usr/local/sbin/abr-shape` only if the user wants them for later tests; else `sudo rm /usr/local/sbin/tc /usr/local/sbin/abr-shape`). Check: `sudo -n /usr/local/sbin/abr-shape show` must ask for a password or fail.

- [ ] **Step 7: Record the results and update the notes and the spec**

Add a section `#### End-to-end result` at the end of this task in `~/GitHub/moonlight-qt-mic-abr/docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md` with one line per step of Step 4 (pass or fail, the values). In `docs/superpowers/specs/2026-10-09-live-resize-design.md`, row D4, change "Keep the current fps and bitrate." to "Keep the current fps. The bitrate follows adaptive bitrate D13 (`2026-10-10-adaptive-bitrate-design.md`).", so the two specs agree. In `docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md` change the status line to `Status: implemented (plan docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md).` and list the plan deviations of Global Constraints under a new section `## 10. Implementation notes` (one line each).

In `~/GitHub/homelab-notes/apollo-cplt-4a.md` add a section "Adaptive bitrate":

- The host advertises `a=x-ss-general.dynamicBitrate:1` (encoders with PARALLEL_ENCODING).
- Control message ids: `0x3102` SET_BITRATE (client to host), `0x3103` BITRATE_STATUS (host to client).
- NVENC (RTX 4090) changes the bitrate in place (spike S1 outcome of Task 2); other encoders restart with one IDR frame.
- The client setting "Adapt bitrate to the network" (default on); command line `--adaptive-bitrate` / `--no-adaptive-bitrate`.
- Host warning lines to look for in `sunshine.log` (min_log_level 3): the three lines of Task 4 Interfaces.
- How to test: the `abr-shape` script (`/var/home/catapultam/src/abr-spike/abr-shape` on the laptop), root needed, cleanup with `abr-shape off`.

```bash
cd ~/GitHub/homelab-notes
git add apollo-cplt-4a.md
git commit -m "Describe the adaptive bitrate feature of the Apollo host"
git push
cd ~/GitHub/moonlight-qt-mic-abr
git add docs/superpowers/plans/2026-10-10-adaptive-bitrate-plan.md docs/superpowers/specs/2026-10-10-adaptive-bitrate-design.md docs/superpowers/specs/2026-10-09-live-resize-design.md
git commit -m "Record the adaptive bitrate end-to-end test results"
git push
```

- [ ] **Step 8: Finish the client branches**

Use superpowers:finishing-a-development-branch for the client. The order of the merge: first the submodule branch `adaptive-bitrate` into `master` of `Catapultam-GMG/moonlight-common-c-mic` (so the pointer of the client is on the submodule `master`), then the client branch `adaptive-bitrate` into `master` of `Catapultam-GMG/moonlight-qt-mic`. After the merge, build the main checkout `~/GitHub/moonlight-qt-mic` (the launchers `moonlight-mic` and `moonlight-game` run it) with the build command of Global Constraints and the clean step, and check `ldd app/moonlight | grep placebo`. Ask the user if the worktrees `~/GitHub/moonlight-qt-mic-abr` and `~/GitHub/apollo-microphone-abr`, the prereleases of the `workflow_dispatch` runs (`gh release list -R catapultam/apollo-microphone` with the body line `branch: adaptive-bitrate`) and the release `spike-s1-nvenc-reconfigure` can be removed.

# Live Resize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One hotkey (`Ctrl+Alt+Shift+R`) makes the stream and the host virtual display equal to the size of the stream window, without a reconnect. Task 1 first repairs the microphone stream, which the host drops today.

**Architecture:** The client sends `RESIZE_REQUEST` (0x3100) on the encrypted control stream. The host validates it, removes and re-adds the SudoVDA monitor at the new size, and lets the capture thread reinitialize with a new encoder size. The host answers only with `RESIZE_REFUSED` (0x3101). The client detects the new size in the first decoded frame and recreates its decoder and renderer through the existing `SDL_RENDER_DEVICE_RESET` path.

**Tech Stack:** Client: C++17/Qt6/SDL2 (sdl2-compat) in `~/GitHub/moonlight-qt-mic`, C in the `moonlight-common-c` submodule (`Catapultam-GMG/moonlight-common-c-mic`), OpenSSL. Host: C++20 Apollo fork in `~/GitHub/apollo-microphone` (branch `live-resize`), built only by GitHub Actions (`build-windows.yml`, about 12 minutes). Host tests: googletest compiled on Linux in the `moonlight` toolbox.

**Spec:** `docs/superpowers/specs/2026-10-09-live-resize-design.md` (read it first; the plan argues from it). Research clones at `/tmp/live-resize/` are read-only.

## Global Constraints

- Client repository: `/var/home/catapultam/GitHub/moonlight-qt-mic`, branch `master`. Build: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && qmake6 moonlight-qt.pro && make release -j$(nproc)'`. After a change of a submodule header run `make -f Makefile.Release clean` in `app/` first. After each build check `ldd app/moonlight | grep placebo`.
- Submodule: `/var/home/catapultam/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c` (remote `origin` = `Catapultam-GMG/moonlight-common-c-mic`, detached at `2e38ae1`). Commit there on branch `master`, push, then bump the pointer in `moonlight-qt-mic`.
- Host repository: `/var/home/catapultam/GitHub/apollo-microphone`, branch `live-resize` (= `0affdaa6` + 5 commits). No Windows toolchain exists. Every host compile check is: `git push`, then `gh run list -R catapultam/apollo-microphone -L 1 --json databaseId -q '.[0].databaseId'`, then `gh run watch <id> -R catapultam/apollo-microphone --exit-status`. The artifact `build-Windows-AMD64` holds `Apollo.exe` (NSIS installer) and `Apollo.zip`. `BUILD_TESTS=OFF` in the workflow; host unit tests run on Linux in the toolbox.
- Host VDD code is Windows only: wrap it in `#ifdef _WIN32`. On other platforms the host refuses with `NOT_VIRTUAL_DISPLAY`.
- Wire protocol (spec 3.1): `RESIZE_REQUEST` id `0x3100`, payload `uint16 width, uint16 height, uint32 request_id` (little endian, 8 bytes). `RESIZE_REFUSED` id `0x3101`, payload `uint16 width, uint16 height, uint32 request_id, uint16 reason` (10 bytes). Reason codes: 1 `BUSY`, 2 `NOT_VIRTUAL_DISPLAY`, 3 `MULTIPLE_CLIENTS`, 4 `SIZE_LIMIT`, 5 `DISPLAY_FAILED`, 6 `ENCODER_FAILED`, 7 `NOT_SUPPORTED` (client only). `request_id` starts at 1.
- Client texts (spec 3.1): "Host is busy with a resize", "Host does not stream a virtual display", "Another client is connected", "Host rejected the size WxH", "Host could not change the display", "Host encoder rejected the size", "Host does not support live resize". Timeout text: "Host did not answer the resize request".
- SDP attribute (spec 3.3): `a=x-ss-general.liveResize:1`, written on Windows only and only when `proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK`. The `SS_FF_*` feature-flag bit space stays untouched.
- Hotkey: `Ctrl+Alt+Shift+R`, active only while a stream runs (spec D8).
- Size rule (spec D4): round each dimension down to an even number; keep fps and bitrate; do nothing when the size equals the current stream size.
- Host limits (spec 4.4): even; at least 320x200; at most 8192x8192; with H.264 (`videoFormat == 0`) at most 4096x4096; `input_only` sessions get `SIZE_LIMIT`.
- Timers: client waits 10 s for a new-size frame; the client overlay shows a refusal for 5 s; the host clears `in_progress` after 15 s.
- Only `controlBroadcastThread` may call `control_server.send()` (ENet is not thread safe). The VDD work runs in a detached worker thread that stores the session uuid, not the pointer (spec 4.6).
- No new settings, no saved size (spec D10, 5.7).
- Host display change order (spec R1, spikes S1/S1b): remove the monitor, add it again with the same GUID, call `changeDisplaySettings(name, w, h, fps)`, read the mode back; only when it is still wrong, add it again with a new GUID and update `display_guid`.
- All prose, comments, commit messages and log texts in ASD-STE100 Simplified Technical English. Commit messages never contain AI attribution.

## Review Focus

1. A refusal that arrives after the client timeout, when a newer request is pending: the client must ignore it (the `request_id` differs) and keep the newer request pending. Pinned by the `PendingRequest` test in Task 3.
2. A stream window below the host limits (for example 300x150, or 0x0 while minimized): the host must refuse with `SIZE_LIMIT`, the client must show "Host rejected the size WxH" and clear the pending state. Pinned by `validate_size` tests in Task 5 and the refusal test in Task 3.
3. A frame at a new size when no request is pending (the host changed the display on its own): the client must treat it as a new stream size and recreate the decoder, not crop it. Pinned by the `classifyFrameSize` test in Task 4.
4. The stream ends while a request is pending: the SDL timers must be removed before the session object goes away, or a late timer callback pushes an event with a dangling pointer. Pinned by the cleanup step and the manual test in Task 3.
5. A microphone Opus packet whose length is a multiple of 16 (for example 48 bytes): the host must receive exactly 48 bytes after decryption, with no padding block behind the data. Pinned by the length loop in the Task 1 test.

---

## File Structure

Client (`~/GitHub/moonlight-qt-mic`):

- `moonlight-common-c/moonlight-common-c/src/MicrophoneCrypto.c` (new): the pure encrypt function of the microphone stream, testable without sockets.
- `moonlight-common-c/moonlight-common-c/src/MicrophoneStream.c`: calls the new function.
- `moonlight-common-c/moonlight-common-c/test/mic_crypto_test.c` (new), `test/resize_protocol_test.c` (new): plain C tests built with `gcc` in the toolbox.
- `moonlight-common-c/moonlight-common-c/src/ControlStream.c`, `RtspConnection.c`, `Connection.c`, `Misc.c`, `FakeCallbacks.c`, `Limelight.h`, `Limelight-internal.h`: protocol side.
- `app/streaming/liveresize.h` (new): reason texts, size rule, pending-request state, frame-size classification. No SDL or Qt dependency.
- `app/tests/liveresize_test.cpp` (new): plain C++ test of `liveresize.h`.
- `app/streaming/input/input.h`, `input.cpp`, `keyboard.cpp`: hotkey and `setStreamSize()`.
- `app/streaming/session.h`, `session.cpp`: request, refusal, timeout, overlay, apply new size.
- `app/streaming/video/decoder.h`, `ffmpeg.h`, `ffmpeg.cpp`: expected size and new-size detection.
- `app/streaming/audio/capture/microphonecapture.cpp`, `session.cpp`: microphone diagnostics.

Host (`~/GitHub/apollo-microphone`):

- `src/live_resize.h` (new): message ids, payload structs, reason enum, `validate_size()`, and the Windows display-change API.
- `src/live_resize.cpp` (new): `change_display_size()` (Windows).
- `tests/unit/test_live_resize.cpp` (new): googletest for the pure parts.
- `src/process.h`, `src/process.cpp`: `proc_t::vdd`, `proc::vdd_lock`, `set_vdd_guid()`, `set_vdd_size()`.
- `src/globals.h`: mail names `resize`, `resize_refused`, `resize_done`, `encoder_failed`.
- `src/video.cpp`: `capture_async` consumes `mail::resize`; `encode_run` raises `resize_done` / `encoder_failed`.
- `src/stream.cpp`, `src/stream.h`: packet ids, session state, control handler, refusal send, worker, mic diagnostics.
- `src/rtsp.cpp`: SDP attribute.
- `cmake/compile_definitions/common.cmake`: new source files.

---

### Task 1: Microphone stream: find and fix the dropped packets

**Files:**
- Create: `moonlight-common-c/moonlight-common-c/src/MicrophoneCrypto.c`
- Create: `moonlight-common-c/moonlight-common-c/test/mic_crypto_test.c`
- Modify: `moonlight-common-c/moonlight-common-c/src/MicrophoneStream.c:1-6` (defines) and `:90-121` (encrypted branch)
- Modify: `moonlight-common-c/moonlight-common-c/src/Limelight-internal.h:59-61`
- Modify: `moonlight-common-c/moonlight-common-c.pro:64` (add the new source)
- Modify: `app/streaming/session.cpp:1826-1840` (client diagnostics)
- Modify (host): `src/stream.cpp:398-413` (session field), `:1416-1424` (diagnostics), `:2376-2381` (init)

**Interfaces:**
- Consumes: `PltEncryptMessage()` (`src/PlatformCrypto.c:36`), `ROUND_TO_PKCS7_PADDED_LEN`, `BE32` (`Limelight-internal.h`).
- Produces: `bool encryptMicrophonePacket(PPLT_CRYPTO_CONTEXT ctx, const unsigned char* key, int keyLength, uint32_t riKeyId, uint16_t sequenceNumber, const unsigned char* opusData, int opusLength, unsigned char* output, int* outputLength)` declared in `Limelight-internal.h`. `MIC_IV_LEN` moves to `Limelight-internal.h`, which also gets `PKCS7_ENCRYPTED_LEN(x)` (`((x / 16) + 1) * 16`, the length after one PKCS#7 padding).

**What is known (planning spike, 2026-10-09):**

- A Linux program that encrypts with the client's real `PltEncryptMessage()` and decrypts with a copy of the host's `cbc_t::decrypt()` (padding on) shows: the client flags `CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH | CIPHER_FLAG_PAD_TO_BLOCK_SIZE` add PKCS#7 padding twice on OpenSSL. The manual padding fills the last block, then `EVP_EncryptFinal_ex` adds one more block of 16 x `0x10`. The host strips only that last block, so the Opus data arrives with 1 to 15 padding bytes behind it (188 of 200 lengths were wrong). Decryption itself did not fail, also not with one decrypt context over 300 packets, as the host uses it. `src/PlatformCrypto.c:26` says the two flags must not be used together. This bug is real, but it does not explain "invalid payload" (a decrypt failure).
- Key and IV derivation are equal on both sides by code reading (`rikey` / `rikeyid` in `app/backend/nvhttp.cpp:210-228` and `src/nvhttp.cpp:389-414`). The host encrypts audio with the same `cbc_t` object, key and `avRiKeyId + sequence` IV scheme, and the client decrypts that audio without errors after the drops began (journal: `Received first audio packet`, no `Failed to decrypt audio packet`). So the host key, the host OpenSSL encrypt path and the IV scheme are good.
- The client build is the OpenSSL path (`nm moonlight-common-c/release/PlatformCrypto.o`: 11 `EVP_` references, 0 `psa_`), `DEFINES = -DNDEBUG -DHAVE_CLOCK_GETTIME=1 -DHAS_SOCKLEN_T -DQT_NO_DEBUG`, OpenSSL 3.5.7 in the toolbox since July (no dnf transaction today). The binary `app/moonlight` is from 12:18.
- The host log (level `warning`) shows no drop line before 12:34:48 and every packet dropped since. But the client journal shows a session at 12:24:41 (pid 1076064) with the same binary, the same launcher and the same `/resume` request as the 12:34:47 session (pid 1086893); a diff of the two session logs shows only noise. The "11:20 run" (pid 932466) started at 10:14:47, before the 10:40 build. So "no drops before 12:34" is not proof that an older client worked: it is equally consistent with the host log level having been `error` before about 12:30, or with packets of the earlier sessions never reaching the host's decrypt path (a session mismatch is silent, `src/stream.cpp:1403-1406`). The installed host binary (`0.0.0.0affdaa.dirty`) was not built from a clean tree, so it may differ from the source. Step 1 tells these cases apart before any code change.

- [ ] **Step 1: Read the host evidence through the agentbus session**

Ask the `agentbus:cplt-4a/alex-ba55d7` session to run these one at a time and report each output (read only). Replace nothing.

```powershell
Select-String -Path 'C:\Program Files\Apollo\config\sunshine.log' -Pattern 'Client microphone redirection requested|Received first client microphone packet|Dropping encrypted microphone packet' | Select-Object -First 3 | ForEach-Object { $_.Line }
```

```powershell
Get-Item 'C:\Program Files\Apollo\config\sunshine.conf' | Select-Object LastWriteTime; Select-String -Path 'C:\Program Files\Apollo\config\sunshine.conf' -Pattern '^(min_log_level|stream_mic|encryption)'; Select-String -Path 'C:\Program Files\Apollo\config\sunshine.log' -Pattern 'version|Logging|min_log_level|Starting' | Select-Object -Last 5 | ForEach-Object { $_.Line }
```

```powershell
Get-Process sunshine | Select-Object Id, StartTime, SessionId, Path; Get-Service ApolloService | Select-Object Status, StartType
```

Read the client side on the laptop:

```bash
journalctl --user -o short-iso --since "2026-10-09 10:00" | grep -E "Sent first client microphone packet|Enabling microphone encryption by host request" | tail -5
```

Decision table. `O` is the Opus size of the client line ("Sent first client microphone packet (O bytes Opus)"). `N` is the payload size of the first host line ("with payload N bytes"; this line has level `info`, so it exists only for sessions that ran with `min_log_level` at `info` or lower):

| Evidence | Meaning | Action |
|----------|---------|--------|
| `sunshine.conf` was written at about 12:30, or the log shows a level change or a process start at that time | The 12:34 boundary is a host-side artifact. The drops are older (since 2026-09-29 at least). | Continue with Step 2. The client build difference is not the cause. |
| `Get-Process` shows a `SessionId` other than 0, or a `StartTime` at about 12:30 | Apollo runs from an interactive start or restarted at the boundary. | Note it; after Step 9, ask the user to `Restart-Service ApolloService` once (ends any stream) and repeat Step 10. A working mic after the restart means stale host process state. |
| `N == ROUND16(O) + 16` with `O % 16 != 0` (64 for O = 44) | The current client wire format. The host source decrypts this (Step 8 proves it). | Continue; the clean host build of Task 9 decides. |
| `O % 16 == 0` (for example 80) | The current and the fixed client both send `O + 16`; this row does not tell them apart. | Use a session with `O % 16 != 0` for the comparison. |
| `N == O` | The client sent plain text while the host expects encryption. | Stop and report before any change: the negotiation differs and this plan does not cover it. |
| "with encryption disabled" in the host line | The host does not decrypt at all. | Stop and report. |

Record the lines in the task notes.

- [ ] **Step 2: Write the failing test**

Create `moonlight-common-c/moonlight-common-c/test/mic_crypto_test.c`:

```c
// Test of the microphone packet encryption against the host decrypt logic.
//
// Build and run in the moonlight toolbox:
//   gcc -O0 -g -I src -I enet/include test/mic_crypto_test.c \
//       src/MicrophoneCrypto.c src/PlatformCrypto.c -lcrypto -o /tmp/mic_crypto_test \
//   && /tmp/mic_crypto_test
#include "Limelight-internal.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RI_KEY_ID 0xF0E1D2C3u
// The largest Opus packet that fits a MAX_MIC_PACKET_SIZE packet with its 12 byte header
#define MAX_TEST_OPUS_LEN 1388

// Copy of crypto::cipher::cbc_t::decrypt() in Apollo src/crypto.cpp with
// padding = true, as stream.cpp constructs session->audio.cipher. The host
// keeps one decrypt context for the whole session and sets the IV per packet.
static int hostDecrypt(EVP_CIPHER_CTX* ctx, bool* initialized,
                       const unsigned char* key, const unsigned char* iv,
                       const unsigned char* cipher, int cipherLength,
                       unsigned char* plain, int* plainLength) {
    int updateLength = 0;
    int finalLength = 0;

    if (!*initialized) {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv) != 1) {
            return -1;
        }
        EVP_CIPHER_CTX_set_padding(ctx, 1);
        *initialized = true;
    }

    if (EVP_DecryptInit_ex(ctx, NULL, NULL, NULL, iv) != 1) {
        return -1;
    }
    if (EVP_DecryptUpdate(ctx, plain, &updateLength, cipher, cipherLength) != 1) {
        return -2;
    }
    if (EVP_DecryptFinal_ex(ctx, plain + updateLength, &finalLength) != 1) {
        return -3;
    }

    *plainLength = updateLength + finalLength;
    return 0;
}

// Builds the IV the host builds: BE32(avRiKeyId + sequenceNumber) in the first
// 4 bytes, the rest zero.
static void hostIv(uint32_t riKeyId, uint16_t sequenceNumber, unsigned char iv[16]) {
    uint32_t ivSeq = BE32(riKeyId + sequenceNumber);
    memset(iv, 0, 16);
    memcpy(iv, &ivSeq, sizeof(ivSeq));
}

static int failures;

static void check(bool condition, const char* what, int opusLength) {
    if (!condition) {
        failures++;
        printf("FAIL: %s (opus length %d)\n", what, opusLength);
    }
}

int main(void) {
    unsigned char key[16];
    PPLT_CRYPTO_CONTEXT clientCtx = PltCreateCryptoContext();
    EVP_CIPHER_CTX* hostCtx = EVP_CIPHER_CTX_new();
    bool hostInitialized = false;
    uint16_t sequenceNumber = 0;

    for (int i = 0; i < 16; i++) {
        key[i] = (unsigned char)(i * 7 + 1);
    }

    // One packet per length, in sequence, on one context pair as in a stream
    for (int opusLength = 1; opusLength <= MAX_TEST_OPUS_LEN; opusLength++, sequenceNumber++) {
        unsigned char opus[MAX_TEST_OPUS_LEN];
        unsigned char encrypted[ROUND_TO_PKCS7_PADDED_LEN(MAX_MIC_PACKET_SIZE)];
        unsigned char plain[ROUND_TO_PKCS7_PADDED_LEN(MAX_MIC_PACKET_SIZE) + 16];
        unsigned char iv[16];
        int encryptedLength = (int)sizeof(encrypted);
        int plainLength = 0;

        for (int i = 0; i < opusLength; i++) {
            opus[i] = (unsigned char)(i ^ 0x5A);
        }

        check(encryptMicrophonePacket(clientCtx, key, sizeof(key), TEST_RI_KEY_ID, sequenceNumber,
                                      opus, opusLength, encrypted, &encryptedLength),
              "encryptMicrophonePacket() returned false", opusLength);

        // The host expects one PKCS#7 padding: 1 to 16 bytes, so a multiple of 16 gets
        // a full block. ROUND_TO_PKCS7_PADDED_LEN() rounds up and gives 16 for 16, so it
        // is the wrong expectation here.
        check(encryptedLength == PKCS7_ENCRYPTED_LEN(opusLength),
              "encrypted length is not PKCS7_ENCRYPTED_LEN(opusLength)", opusLength);

        hostIv(TEST_RI_KEY_ID, sequenceNumber, iv);
        check(hostDecrypt(hostCtx, &hostInitialized, key, iv, encrypted, encryptedLength, plain, &plainLength) == 0,
              "host decrypt failed", opusLength);
        check(plainLength == opusLength, "host got a different length", opusLength);
        check(memcmp(plain, opus, opusLength) == 0, "host got different data", opusLength);
    }

    // A full output buffer must not be written past its end
    {
        unsigned char opus[32] = {0};
        unsigned char small[16];
        int smallLength = (int)sizeof(small);
        check(!encryptMicrophonePacket(clientCtx, key, sizeof(key), TEST_RI_KEY_ID, 0,
                                       opus, sizeof(opus), small, &smallLength),
              "encryptMicrophonePacket() accepted a too small output buffer", 32);
    }

    EVP_CIPHER_CTX_free(hostCtx);
    PltDestroyCryptoContext(clientCtx);

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }

    printf("mic_crypto_test: all checks passed\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c && gcc -O0 -g -I src -I enet/include test/mic_crypto_test.c src/MicrophoneCrypto.c src/PlatformCrypto.c -lcrypto -o /tmp/mic_crypto_test && /tmp/mic_crypto_test'`
Expected: the build fails with `src/MicrophoneCrypto.c: No such file or directory`.

- [ ] **Step 4: Move the microphone defines and declare the function**

In `src/Limelight-internal.h` replace lines 59-61 with:

```c
#define MIC_PACKET_MAGIC 0x12345678
#define MIC_PACKET_TYPE_OPUS 0x61
#define MAX_MIC_PACKET_SIZE 1400
#define MIC_IV_LEN 16

// Length of x bytes after one PKCS#7 padding: 1 to 16 padding bytes, so a
// multiple of 16 gets a full block. ROUND_TO_PKCS7_PADDED_LEN() rounds up
// and gives x for a multiple of 16, which is not the PKCS#7 length.
#define PKCS7_ENCRYPTED_LEN(x) ((((x) / 16) + 1) * 16)

// Encrypts one Opus packet for the microphone stream (MicrophoneCrypto.c).
// The output is AES-128-CBC with one PKCS#7 padding, as the host decrypts it.
// *outputLength holds the output buffer size on entry and the encrypted length
// on return. The buffer must hold PKCS7_ENCRYPTED_LEN(opusLength) bytes.
bool encryptMicrophonePacket(PPLT_CRYPTO_CONTEXT ctx,
                             const unsigned char* key, int keyLength,
                             uint32_t riKeyId, uint16_t sequenceNumber,
                             const unsigned char* opusData, int opusLength,
                             unsigned char* output, int* outputLength);
```

In `src/MicrophoneStream.c` delete the line `#define MIC_IV_LEN 16`.

- [ ] **Step 5: Write the function with the flags the client uses today**

Create `src/MicrophoneCrypto.c`:

```c
#include "Limelight-internal.h"

bool encryptMicrophonePacket(PPLT_CRYPTO_CONTEXT ctx,
                             const unsigned char* key, int keyLength,
                             uint32_t riKeyId, uint16_t sequenceNumber,
                             const unsigned char* opusData, int opusLength,
                             unsigned char* output, int* outputLength) {
    unsigned char iv[MIC_IV_LEN] = {0};
    unsigned char paddedData[ROUND_TO_PKCS7_PADDED_LEN(MAX_MIC_PACKET_SIZE)];
    uint32_t ivSeq;

    if (opusLength <= 0 || opusLength > MAX_MIC_PACKET_SIZE ||
            *outputLength < PKCS7_ENCRYPTED_LEN(opusLength)) {
        return false;
    }

    // The IV is the rikeyid plus the sequence number, in big endian
    ivSeq = BE32(riKeyId + sequenceNumber);
    memcpy(iv, &ivSeq, sizeof(ivSeq));
    memcpy(paddedData, opusData, opusLength);

    return PltEncryptMessage(ctx,
                             ALGORITHM_AES_CBC,
                             CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH | CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
                             (unsigned char*)key, keyLength,
                             iv, sizeof(iv),
                             NULL, 0,
                             paddedData, opusLength,
                             output, outputLength);
}
```

- [ ] **Step 6: Run the test to reproduce the bug**

Run the command of Step 3.
Expected: many lines `FAIL: encrypted length is not PKCS7_ENCRYPTED_LEN(opusLength)` and `FAIL: host got a different length`, then `N checks failed`, exit code 1. Lengths that are multiples of 16 pass, because the manual padding adds nothing there and the cipher's own block is then the only padding. This is the reproduction: the client adds a second padding block.

- [ ] **Step 7: Fix the flags**

Replace the body of `encryptMicrophonePacket()` in `src/MicrophoneCrypto.c` with:

```c
    unsigned char iv[MIC_IV_LEN] = {0};
    uint32_t ivSeq;

    if (opusLength <= 0 || opusLength > MAX_MIC_PACKET_SIZE ||
            *outputLength < PKCS7_ENCRYPTED_LEN(opusLength)) {
        return false;
    }

    // The IV is the rikeyid plus the sequence number, in big endian
    ivSeq = BE32(riKeyId + sequenceNumber);
    memcpy(iv, &ivSeq, sizeof(ivSeq));

    // CIPHER_FLAG_FINISH lets the cipher add the PKCS#7 padding. Do not add
    // CIPHER_FLAG_PAD_TO_BLOCK_SIZE: with OpenSSL the two flags together add a
    // second padding block, which the host decrypts into bytes behind the
    // Opus data. With PSA crypto both flag sets give the same bytes. Without
    // CIPHER_FLAG_PAD_TO_BLOCK_SIZE the input is not modified, so the cast is safe.
    return PltEncryptMessage(ctx,
                             ALGORITHM_AES_CBC,
                             CIPHER_FLAG_RESET_IV | CIPHER_FLAG_FINISH,
                             (unsigned char*)key, keyLength,
                             iv, sizeof(iv),
                             NULL, 0,
                             (unsigned char*)opusData, opusLength,
                             output, outputLength);
```

Remove the `paddedData` array.

- [ ] **Step 8: Run the test to see it pass**

Run the command of Step 3.
Expected: `mic_crypto_test: all checks passed`, exit code 0.

- [ ] **Step 9: Use the function in the stream and build the client**

In `src/MicrophoneStream.c` replace the encrypted branch (`if ((EncryptionFeaturesEnabled & SS_ENC_MICROPHONE) && micEncryptionCtx != NULL) {` up to the matching `}` before `else {`) with:

```c
    if ((EncryptionFeaturesEnabled & SS_ENC_MICROPHONE) && micEncryptionCtx != NULL) {
        unsigned char encryptedData[ROUND_TO_PKCS7_PADDED_LEN(MAX_MIC_PACKET_SIZE)];
        int encryptedLength = (int)sizeof(encryptedData);

        if (!encryptMicrophonePacket(micEncryptionCtx,
                                     (unsigned char*)StreamConfig.remoteInputAesKey,
                                     sizeof(StreamConfig.remoteInputAesKey),
                                     micRiKeyId, micSequenceNumber,
                                     opusData, opusLength,
                                     encryptedData, &encryptedLength)) {
            Limelog("MIC: Encryption failed\n");
            return -1;
        }

        packetLength = (int)sizeof(header) + encryptedLength;
        if (packetLength > MAX_MIC_PACKET_SIZE || packetLength > (int)sizeof(packet)) {
            Limelog("MIC: Encrypted packet too large (%d > %d)\n", packetLength, MAX_MIC_PACKET_SIZE);
            return -1;
        }

        if (micSequenceNumber == 0) {
            // One line per stream that the host diagnostics line can be compared with.
            // riKeyId travels in the clear in the launch URL, so this is not a secret.
            Limelog("MIC: first packet: riKeyId %u, %d bytes Opus, %d bytes encrypted, first bytes "
                    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    micRiKeyId, opusLength, encryptedLength,
                    encryptedData[0], encryptedData[1], encryptedData[2], encryptedData[3],
                    encryptedData[4], encryptedData[5], encryptedData[6], encryptedData[7],
                    encryptedData[8], encryptedData[9], encryptedData[10], encryptedData[11],
                    encryptedData[12], encryptedData[13], encryptedData[14], encryptedData[15]);
        }

        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), encryptedData, encryptedLength);
    }
```

In `moonlight-common-c/moonlight-common-c.pro` add after line 64 (`$$COMMON_C_DIR/src/MicrophoneStream.c \`):

```
    $$COMMON_C_DIR/src/MicrophoneCrypto.c \
```

The CMake build picks the file up through `aux_source_directory(src SRC_LIST)`.

In `app/streaming/session.cpp` add `#include <openssl/sha.h>` after `#include <openssl/rand.h>` (line 33) and, in `Session::exec()`, replace

```cpp
    if (m_Preferences->enableMicrophone) {
        if (LiIsMicrophoneStreamActive()) {
```

with

```cpp
    if (m_Preferences->enableMicrophone) {
        if (qEnvironmentVariableIsSet("MOONLIGHT_MIC_DEBUG")) {
            // Print the values that the host prints when it cannot decrypt, so
            // that the two logs can be compared. The key is only a fingerprint.
            unsigned char digest[SHA256_DIGEST_LENGTH];
            uint32_t riKeyId;

            SHA256(reinterpret_cast<const unsigned char*>(m_StreamConfig.remoteInputAesKey),
                   sizeof(m_StreamConfig.remoteInputAesKey), digest);
            memcpy(&riKeyId, m_StreamConfig.remoteInputAesIv, sizeof(riKeyId));
            riKeyId = qFromBigEndian(riKeyId);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Microphone diagnostics: avRiKeyId %u, key fingerprint %02x%02x%02x%02x, encryption %s",
                        riKeyId, digest[0], digest[1], digest[2], digest[3],
                        LiIsMicrophoneEncryptionEnabled() ? "on" : "off");
        }

        if (LiIsMicrophoneStreamActive()) {
```

Build: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && make -C app -f Makefile.Release clean && qmake6 moonlight-qt.pro && make release -j$(nproc)'` then `ldd app/moonlight | grep placebo`.
Expected: build succeeds; `libplacebo.so` is listed.

- [ ] **Step 10: Live check against the installed host**

Start a stream with `moonlight-mic` (microphone enabled in Settings), speak for 10 seconds, then read the host counters from the laptop. The web UI user name and password are the ones the user enters in the Apollo web UI (the user types them; do not store them):

```bash
read -r -p 'Apollo web user: ' U; read -r -s -p 'Apollo web password: ' P; echo
curl -sk -c /tmp/apollo.jar -H 'Content-Type: application/json' \
  -d "{\"username\":\"$U\",\"password\":\"$P\"}" https://10.10.10.232:47990/api/login
curl -sk -b /tmp/apollo.jar https://10.10.10.232:47990/api/audio-debug | python3 -I -m json.tool
unset P
```

If the login answers 403 (origin check), open `https://localhost:47990/api/audio-debug` in a browser on the host from inside the stream instead.

Expected with the fixed client: `encryptionEnabled: true`, `packetsReceived` and `packetsDecoded` increase, `decryptErrors` stays 0, `lastPayloadSize` equals `O + 16 - (O % 16)` (48 for 44-byte Opus packets, 96 for 80-byte packets).

If `decryptErrors` still increases, do these A/B checks in this order; each is one stream of about 20 seconds followed by the `curl` above:

1. Host process state: ask the user (or the agentbus session) to run `Restart-Service ApolloService` (ends any stream), then stream again. A mic that works after the restart means stale host process state; note it and continue, because the Task 9 deploy restarts the service anyway.
2. Launcher environment: run the binary with the launcher environment of before 12:18: `toolbox run -c moonlight env QT_QPA_PLATFORM=wayland SDL_GAMECONTROLLER_IGNORE_DEVICES=0x3434/0x0ea0 ~/GitHub/moonlight-qt-mic/app/moonlight`. A difference means an environment effect; report it.
3. Fresh build tree: `git -C ~/GitHub/moonlight-qt-mic worktree add /tmp/mqm-ab master && cd /tmp/mqm-ab && git submodule update --init --recursive && toolbox run -c moonlight bash -lc 'cd /tmp/mqm-ab && qmake6 moonlight-qt.pro && make release -j$(nproc)'`, then run `/tmp/mqm-ab/app/moonlight` with the normal launcher environment. A difference means the main tree build is corrupt; compare `cmp` of `moonlight-common-c/release/PlatformCrypto.o` and `MicrophoneStream.o` between the trees and report. Remove the worktree afterwards (`git worktree remove /tmp/mqm-ab`).

When all three still fail: the packet bytes are right (Step 8 proves it against the host source), so the installed host binary is the suspect. Record the `/api/audio-debug` output, do Steps 11-12 (host diagnostics) and continue with Task 2. The clean host build of Task 9 decides: after that deploy, repeat this step. The host log then shows the `Microphone decrypt diagnostics` line of Step 11 and the client journal shows the `MIC: first packet` line of Step 9 (and the `Microphone diagnostics` line with `MOONLIGHT_MIC_DEBUG=1`). Compare `riKeyId` with `avRiKeyId`, the key fingerprints, the encrypted length with `payload`, and the 16 first bytes. Equal values with a failing decrypt point at the host OpenSSL build (the `OpenSSL error` text says which step failed); different values mean the host session got another launch session than the one the client negotiated. Both cases are outside this plan: stop and report them with the two lines.

- [ ] **Step 11: Add the host diagnostics line**

In `~/GitHub/apollo-microphone/src/stream.cpp`, in `session_t`'s `audio` struct after `bool first_mic_packet_logged;` (line 413) add:

```cpp
      bool first_mic_decrypt_error_logged;
```

In `session::alloc` after `session->audio.first_mic_packet_logged = false;` add:

```cpp
      session->audio.first_mic_decrypt_error_logged = false;
```

In `micRecvThread`, replace

```cpp
        if (session->audio.cipher.decrypt(std::string_view {reinterpret_cast<const char *>(payload), payload_len}, decrypted_payload, &iv) != 0) {
          BOOST_LOG(warning) << "Dropping encrypted microphone packet with invalid payload for ["sv << session->device_name
```

with

```cpp
        if (session->audio.cipher.decrypt(std::string_view {reinterpret_cast<const char *>(payload), payload_len}, decrypted_payload, &iv) != 0) {
          if (!session->audio.first_mic_decrypt_error_logged) {
            // Print once what the client prints with MOONLIGHT_MIC_DEBUG=1, so the two logs can be compared
            session->audio.first_mic_decrypt_error_logged = true;
            const auto &key = session->audio.cipher.key;
            const auto key_fingerprint = util::hex(crypto::hash(std::string_view {reinterpret_cast<const char *>(key.data()), key.size()})).to_string().substr(0, 8);
            char openssl_error[256] = {};
            ERR_error_string_n(ERR_peek_last_error(), openssl_error, sizeof(openssl_error));
            BOOST_LOG(warning) << "Microphone decrypt diagnostics for ["sv << session->device_name
                               << "]: avRiKeyId "sv << session->audio.avRiKeyId
                               << ", sequence "sv << sequence_number
                               << ", payload "sv << payload_len << " bytes, first bytes "sv
                               << util::hex_vec(payload, payload + std::min<std::size_t>(payload_len, 16))
                               << ", key fingerprint "sv << key_fingerprint
                               << ", OpenSSL error ["sv << openssl_error << ']';
          }
          BOOST_LOG(warning) << "Dropping encrypted microphone packet with invalid payload for ["sv << session->device_name
```

- [ ] **Step 12: Commit the host diagnostics (CI runs with Task 5)**

```bash
cd ~/GitHub/apollo-microphone
git add src/stream.cpp
git commit -m "Log microphone decrypt diagnostics once per session"
```

The compile check for this commit is the CI run of Task 5 (one run covers both; if that run fails in `stream.cpp`, fix it in this task).

- [ ] **Step 13: Commit the client fix**

```bash
cd ~/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c
git fetch origin && git log --oneline -1 origin/master   # expect 2e38ae1; stop if it moved
git checkout -B master
git add src/MicrophoneCrypto.c src/MicrophoneStream.c src/Limelight-internal.h test/mic_crypto_test.c
git commit -m "Encrypt microphone packets with one PKCS7 padding

With OpenSSL, CIPHER_FLAG_PAD_TO_BLOCK_SIZE and CIPHER_FLAG_FINISH together
add a second padding block. The host strips only one block, so 1 to 15
padding bytes stayed behind the Opus data. Use CIPHER_FLAG_FINISH only,
which gives the same bytes with OpenSSL and PSA. Add a test that decrypts
with the host logic."
git push origin master
cd ~/GitHub/moonlight-qt-mic
git add moonlight-common-c/moonlight-common-c moonlight-common-c/moonlight-common-c.pro app/streaming/session.cpp
git commit -m "Fix microphone packet padding and add mic diagnostics

Bump moonlight-common-c for the one-padding fix. Print the key fingerprint
and avRiKeyId at stream start when MOONLIGHT_MIC_DEBUG is set."
```

---

### Task 2: moonlight-common-c: resize request, refusal callback, SDP flag

**Files:**
- Modify: `moonlight-common-c/moonlight-common-c/src/Limelight.h:486-505` (callback), `:560-585` (API)
- Modify: `moonlight-common-c/moonlight-common-c/src/Limelight-internal.h:47` (global), `:59` (parser prototype)
- Modify: `moonlight-common-c/moonlight-common-c/src/ControlStream.c:46-88` (queue union), `:130-142` (indices), `:147-216` (tables), `:300-345` (init), `:907-1104` (callbacks), new function after `LiRequestIdrFrame()`
- Modify: `moonlight-common-c/moonlight-common-c/src/RtspConnection.c:1148-1150`
- Modify: `moonlight-common-c/moonlight-common-c/src/Connection.c:37`, `src/Misc.c:155`, `src/FakeCallbacks.c:39-60,143-150`
- Create: `moonlight-common-c/moonlight-common-c/test/resize_protocol_test.c`

**Interfaces:**
- Consumes: `sendMessageAndForget()` (`ControlStream.c:846`), `needsAsyncCallback()` / `queueAsyncCallback()` (`:1023-1102`), `parseSdpAttributeToUInt()` (`RtspConnection.c:905`), `LE16`/`LE32`, `IS_SUNSHINE()`.
- Produces (used by Tasks 3 and 4): `int LiSendResizeRequest(uint16_t width, uint16_t height, uint32_t* requestId)` (0 on success, -1 otherwise); `bool LiIsLiveResizeSupported(void)`; callback `typedef void(*ConnListenerResizeRefused)(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason)` as the last field `resizeRefused` of `CONNECTION_LISTENER_CALLBACKS`; defines `LI_RESIZE_REFUSED_BUSY` (1) to `LI_RESIZE_REFUSED_NOT_SUPPORTED` (7).

- [ ] **Step 1: Write the failing test**

Create `test/resize_protocol_test.c`:

```c
// Test of the live resize API without a connection and of the SDP parse.
//
// Build the library once in the moonlight toolbox:
//   cmake -S . -B /tmp/mcc-build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug
//   cmake --build /tmp/mcc-build -j
// Then:
//   gcc -O0 -g -I src -I enet/include test/resize_protocol_test.c \
//       /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a \
//       -lcrypto -lpthread -o /tmp/resize_protocol_test && /tmp/resize_protocol_test
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

    // Without a connection the host cannot have advertised the feature
    check(!LiIsLiveResizeSupported(), "LiIsLiveResizeSupported() is true before a connection");

    // Without a connection the request must fail and leave the id alone
    check(LiSendResizeRequest(2536, 1390, &requestId) == -1, "LiSendResizeRequest() did not fail without a connection");
    check(requestId == 77, "LiSendResizeRequest() changed the id on failure");

    // The SDP attribute parse used by the DESCRIBE handler
    const char* sdpWith = "a=x-ss-general.featureFlags:3\r\na=x-ss-general.liveResize:1\r\na=rtpmap:96 opus/48000/1\r\n";
    const char* sdpWithout = "a=x-ss-general.featureFlags:3\r\na=rtpmap:96 opus/48000/1\r\n";
    check(parseSdpAttributeToUInt(sdpWith, "x-ss-general.liveResize", &value) && value == 1,
          "liveResize attribute not parsed as 1");
    value = 5;
    check(!parseSdpAttributeToUInt(sdpWithout, "x-ss-general.liveResize", &value) && value == 5,
          "missing liveResize attribute reported as present");

    if (failures != 0) {
        printf("%d checks failed\n", failures);
        return 1;
    }

    printf("resize_protocol_test: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c && cmake -S . -B /tmp/mcc-build -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug && cmake --build /tmp/mcc-build -j && gcc -O0 -g -I src -I enet/include test/resize_protocol_test.c /tmp/mcc-build/libmoonlight-common-c.a /tmp/mcc-build/enet/libenet.a -lcrypto -lpthread -o /tmp/resize_protocol_test && /tmp/resize_protocol_test'`
Expected: `implicit declaration of function 'LiIsLiveResizeSupported'` (and the others), build fails.

- [ ] **Step 3: Public API in Limelight.h**

After the `ConnListenerSetControllerLED` typedef (line 489) add:

```c
// This callback is invoked when the host refuses a resize request that was sent
// with LiSendResizeRequest(). The reason is one of the LI_RESIZE_REFUSED_* values.
// The host sends no message on success; the new size arrives in the video stream.
typedef void(*ConnListenerResizeRefused)(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason);
```

In `CONNECTION_LISTENER_CALLBACKS` add as the last field, after `setAdaptiveTriggers`:

```c
    ConnListenerResizeRefused resizeRefused;
```

After `bool LiIsMicrophoneStreamActive(void);` (line 583) add:

```c
// Reason codes for ConnListenerResizeRefused()
#define LI_RESIZE_REFUSED_BUSY 1
#define LI_RESIZE_REFUSED_NOT_VIRTUAL_DISPLAY 2
#define LI_RESIZE_REFUSED_MULTIPLE_CLIENTS 3
#define LI_RESIZE_REFUSED_SIZE_LIMIT 4
#define LI_RESIZE_REFUSED_DISPLAY_FAILED 5
#define LI_RESIZE_REFUSED_ENCODER_FAILED 6
#define LI_RESIZE_REFUSED_NOT_SUPPORTED 7

// Returns true when the host advertised live resize in the RTSP DESCRIBE reply
// (SDP attribute x-ss-general.liveResize). False before a connection.
bool LiIsLiveResizeSupported(void);

// Asks the host to change the stream and the virtual display to width x height.
// The host answers only when it refuses (ConnListenerResizeRefused). On success,
// *requestId receives the id of this request, which the refusal callback repeats.
// Returns 0 when the request was sent, or -1 when the host does not support live
// resize, the control stream is not encrypted, or no connection is active.
int LiSendResizeRequest(uint16_t width, uint16_t height, uint32_t* requestId);
```

- [ ] **Step 4: Internal declarations**

In `src/Limelight-internal.h` after `extern uint32_t SunshineFeatureFlags;` (line 47) add:

```c
extern bool LiveResizeSupported;
```

After the `MIC_*` defines add:

```c
// RtspConnection.c: reads an SDP attribute of the form a=name:value
bool parseSdpAttributeToUInt(const char* payload, const char* name, uint32_t* val);
```

In `src/Connection.c` after `uint32_t SunshineFeatureFlags;` (line 37) add `bool LiveResizeSupported;`.

In `src/Misc.c` after `LiGetHostFeatureFlags()` add:

```c
bool LiIsLiveResizeSupported(void) {
    return LiveResizeSupported;
}
```

In `src/RtspConnection.c` after the `SunshineFeatureFlags` parse block (ends line 1150) add:

```c
        // Look for the live resize attribute (Apollo extension)
        {
            uint32_t liveResize;
            if (parseSdpAttributeToUInt(response.payload, "x-ss-general.liveResize", &liveResize)) {
                LiveResizeSupported = (liveResize != 0);
            }
            else {
                LiveResizeSupported = false;
            }
        }
```

In `src/FakeCallbacks.c` add after `fakeClSetControllerLED`:

```c
static void fakeClResizeRefused(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason) {}
```

add `.resizeRefused = fakeClResizeRefused,` as the last initializer of `fakeClCallbacks`, and in `fixupMissingCallbacks()` after the `setAdaptiveTriggers` fixup:

```c
        if ((*clCallbacks)->resizeRefused == NULL) {
            (*clCallbacks)->resizeRefused = fakeClResizeRefused;
        }
```

- [ ] **Step 5: Control stream send and receive**

In `src/ControlStream.c`:

After `#define IDX_DS_ADAPTIVE_TRIGGERS 12` add:

```c
#define IDX_RESIZE_REQUEST 13
#define IDX_RESIZE_REFUSED 14
```

`packetTypesGen3`, `packetTypesGen4`, `packetTypesGen5` and `packetTypesGen7` have 12 entries (indices 0 to 11), but `IDX_DS_ADAPTIVE_TRIGGERS` is 12, so `needsAsyncCallback()` already reads one entry past their end on non-Sunshine hosts (pre-existing). Append three entries to each of these four arrays, after the last entry, with the same comma style, so that index 12 is the adaptive triggers and 13 and 14 are the resize messages:

```c
    -1,     // Set adaptive triggers (unused)
    -1,     // Resize request (unused)
    -1,     // Resize refused (unused)
```

Append to `packetTypesGen7Enc` after `0x5503`:

```c
    0x3100, // Resize request (Apollo live resize extension)
    0x3101, // Resize refused (Apollo live resize extension)
```

In the `QUEUED_ASYNC_CALLBACK` union add after `dsAdaptiveTrigger`:

```c
        struct {
            uint16_t width;
            uint16_t height;
            uint32_t requestId;
            uint16_t reason;
        } resizeRefused;
```

After `static PPLT_CRYPTO_CONTEXT decryptionCtx;` add:

```c
static uint32_t nextResizeRequestId;
```

In `initializeControlStream()` after `PltCreateMutex(&enetMutex);` add:

```c
    nextResizeRequestId = 1;
```

After the `#pragma pack(pop)` that ends the packet header structs near the top of the file (search for `NVCTL_ENCRYPTED_PACKET_HEADER`), add:

```c
#pragma pack(push, 1)
// Payload of the resize request (Apollo live resize extension), little endian
typedef struct _SS_RESIZE_REQUEST {
    uint16_t width;
    uint16_t height;
    uint32_t requestId;
} SS_RESIZE_REQUEST, *PSS_RESIZE_REQUEST;
#pragma pack(pop)
```

In `needsAsyncCallback()` add a line:

```c
           packetType == packetTypes[IDX_DS_ADAPTIVE_TRIGGERS] ||
           packetType == packetTypes[IDX_RESIZE_REFUSED];
```

In `queueAsyncCallback()` before the final `else {` add:

```c
    else if (ctlHdr->type == packetTypes[IDX_RESIZE_REFUSED]) {
        BbGet16(&bb, &queuedCb->data.resizeRefused.width);
        BbGet16(&bb, &queuedCb->data.resizeRefused.height);
        BbGet32(&bb, &queuedCb->data.resizeRefused.requestId);
        BbGet16(&bb, &queuedCb->data.resizeRefused.reason);

        queuedCb->typeIndex = IDX_RESIZE_REFUSED;
    }
```

In `asyncCallbackThreadFunc()` before `default:` add:

```c
        case IDX_RESIZE_REFUSED:
            // Refusals are infrequent and are not batched
            ListenerCallbacks.resizeRefused(queuedCb->data.resizeRefused.width,
                                            queuedCb->data.resizeRefused.height,
                                            queuedCb->data.resizeRefused.requestId,
                                            queuedCb->data.resizeRefused.reason);
            break;
```

After `LiRequestIdrFrame()` add:

```c
int LiSendResizeRequest(uint16_t width, uint16_t height, uint32_t* requestId) {
    SS_RESIZE_REQUEST request;

    // The request goes only to a Sunshine-type host over the encrypted control stream
    if (!LiveResizeSupported || !IS_SUNSHINE() || !encryptedControlStream) {
        return -1;
    }

    if (packetTypes[IDX_RESIZE_REQUEST] == -1 || peer == NULL || stopping) {
        return -1;
    }

    request.width = LE16(width);
    request.height = LE16(height);
    request.requestId = LE32(nextResizeRequestId);

    if (!sendMessageAndForget(packetTypes[IDX_RESIZE_REQUEST], sizeof(request), &request,
                              CTRL_CHANNEL_GENERIC, ENET_PACKET_FLAG_RELIABLE, false)) {
        return -1;
    }

    *requestId = nextResizeRequestId++;
    return 0;
}
```

Note: `peer` is the static `ENetPeer*` of this file; `stopping` is set by `stopControlStream()`. The remaining race with a termination that runs at the same time is the same as for every other sender in this file.

- [ ] **Step 6: Run the test to see it pass**

Run the command of Step 2.
Expected: `resize_protocol_test: all checks passed`.

- [ ] **Step 7: Build the client and commit**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && make -C app -f Makefile.Release clean && qmake6 moonlight-qt.pro && make release -j$(nproc) 2>&1 | tail -3'`
Expected: a successful link of `app/moonlight` (the `k_ConnCallbacks` initializer in `session.cpp` has one entry fewer than the struct; C++ zero-fills the missing last field, so the build passes; Task 3 fills it).

```bash
cd ~/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c
git fetch origin && git log --oneline -1 origin/master   # expect the Task 1 commit; stop if it moved
git add src/ControlStream.c src/RtspConnection.c src/Connection.c src/Misc.c src/FakeCallbacks.c src/Limelight.h src/Limelight-internal.h test/resize_protocol_test.c
git commit -m "Add the live resize request and refusal messages

The client sends RESIZE_REQUEST (0x3100) on the encrypted control stream
and gets RESIZE_REFUSED (0x3101) through a new async callback. The host
advertises the feature with the SDP attribute x-ss-general.liveResize."
git push origin master
cd ~/GitHub/moonlight-qt-mic
git add moonlight-common-c/moonlight-common-c
git commit -m "Bump moonlight-common-c for the live resize messages"
```

---

### Task 3: Client hotkey, request, refusal and timeout

**Files:**
- Create: `app/streaming/liveresize.h`
- Create: `app/tests/liveresize_test.cpp`
- Modify: `app/streaming/input/input.h:163-175` (enum), `:86-100` (public), `app/streaming/input/input.cpp:86-138` (combos), `app/streaming/input/keyboard.cpp:165-180`
- Modify: `app/streaming/session.h:102-130` (public), `:199-230` (callbacks), `:251-290` (members)
- Modify: `app/streaming/session.cpp:26-31` (codes), `:51-65` (callbacks), `:173-200` (status update), `:560-590` (ctor), `:1549-1563` (mouse emulation), `:2061-2095` (user events), `:2369-2373` (cleanup)
- Modify: `app/app.pro:210` (HEADERS)

**Interfaces:**
- Consumes: `LiSendResizeRequest()`, `LiIsLiveResizeSupported()`, `LI_RESIZE_REFUSED_*`, `ConnListenerResizeRefused` (Task 2); `StreamUtils::getNativeDesktopMode()` (`app/streaming/streamutils.h:24`); `Overlay::OverlayManager` (`app/streaming/video/overlaymanager.h`).
- Produces: `namespace LiveResize { enum Reason; void roundDownEven(int&, int&); const char* reasonText(uint16_t, int, int, char*, size_t); struct PendingRequest; }` in `liveresize.h`; `void Session::requestLiveResize()`; `void Session::clearPendingResize()`; `void Session::showResizeStatus(const char*)`; `void SdlInputHandler::setStreamSize(int, int)`; members `m_PendingResize`, `m_ResizeTimeoutTimer`, `m_StatusOverlayTimer`, `m_StatusOverlayOwner`. Event codes `SDL_CODE_RESIZE_REFUSED 107`, `SDL_CODE_RESIZE_TIMEOUT 108`, `SDL_CODE_RESIZE_STATUS_TIMEOUT 109`. Task 4 adds `applyStreamSize()` and code 106.

- [ ] **Step 1: Write the failing test**

Create `app/tests/liveresize_test.cpp`:

```cpp
// Test of app/streaming/liveresize.h. Build and run in the moonlight toolbox:
//   g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp \
//       -o /tmp/liveresize_test && /tmp/liveresize_test
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

int main()
{
    testRoundDownEven();
    testPendingRequest();
    testReasonText();
    puts("liveresize_test: all checks passed");
    return 0;
}
```

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test && /tmp/liveresize_test'`
Expected: `streaming/liveresize.h: No such file or directory`.

- [ ] **Step 3: Write liveresize.h**

Create `app/streaming/liveresize.h`:

```cpp
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

}
```

- [ ] **Step 4: Run the test to see it pass**

Run the command of Step 2.
Expected: `liveresize_test: all checks passed`.

- [ ] **Step 5: Hotkey in the input handler**

In `app/streaming/input/input.h`, in `enum KeyCombo` add `KeyComboResizeToWindow,` before `KeyComboMax`. In the public section after `void setWindow(SDL_Window* window);` add:

```cpp
    // Updates the stream size that absolute mouse and touch events are scaled to
    void setStreamSize(int width, int height)
    {
        m_StreamWidth = width;
        m_StreamHeight = height;
    }
```

In `app/streaming/input/input.cpp` after the `KeyComboToggleKeyboardGrab` block (ends line 138) add:

```cpp
    m_SpecialKeyCombos[KeyComboResizeToWindow].keyCombo = KeyComboResizeToWindow;
    m_SpecialKeyCombos[KeyComboResizeToWindow].keyCode = SDLK_r;
    m_SpecialKeyCombos[KeyComboResizeToWindow].scanCode = SDL_SCANCODE_R;
    m_SpecialKeyCombos[KeyComboResizeToWindow].enabled = true;
```

In `app/streaming/input/keyboard.cpp` in `performSpecialKeyCombo()` before `default:` add:

```cpp
    case KeyComboResizeToWindow:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Detected resize to window combo");
        Session::s_ActiveSession->requestLiveResize();
        break;
```

- [ ] **Step 6: Session declarations**

In `app/streaming/session.h` add `#include "liveresize.h"` with the other includes, and in the public section after `void setShouldExit(bool quitHostApp = false);` add:

```cpp
    // Asks the host to make the stream equal to the window size (Ctrl+Alt+Shift+R)
    void requestLiveResize();
```

In the private section after `void clSetAdaptiveTriggers(...)` add:

```cpp
    static
    void clResizeRefused(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason);

    static
    Uint32 resizeTimeoutTimerCallback(Uint32 interval, void* param);

    static
    Uint32 statusOverlayTimerCallback(Uint32 interval, void* param);

    // Shows a live resize message in the status overlay for a few seconds
    void showResizeStatus(const char* text);

    // Forgets the pending request and stops its timeout
    void clearPendingResize();
```

After `Overlay::OverlayManager m_OverlayManager;` add:

```cpp
    // Who put the current text in the status overlay. The connection warning
    // and the gamepad mouse mode are not replaced by resize messages.
    enum class StatusOverlayOwner {
        None,
        Connection,
        MouseEmulation,
        Resize,
    };
    StatusOverlayOwner m_StatusOverlayOwner;
    LiveResize::PendingRequest m_PendingResize;
    SDL_TimerID m_ResizeTimeoutTimer;
    SDL_TimerID m_StatusOverlayTimer;
    // Counts showResizeStatus() calls; a timer event from an older call is ignored
    uint32_t m_StatusOverlayGeneration;
```

In `app/app.pro` add `streaming/liveresize.h \` to the `HEADERS` list after `streaming/session.h \`.

- [ ] **Step 7: Session implementation**

In `app/streaming/session.cpp` after `#define SDL_CODE_GAMECONTROLLER_SET_ADAPTIVE_TRIGGERS 105` add:

```cpp
#define SDL_CODE_RESIZE_REFUSED 107
#define SDL_CODE_RESIZE_TIMEOUT 108
#define SDL_CODE_RESIZE_STATUS_TIMEOUT 109

// Time to wait for the first frame at the new size
#define RESIZE_TIMEOUT_MS 10000
// Time a resize message stays in the status overlay
#define RESIZE_STATUS_MS 5000
```

(Code 106 is `SDL_CODE_STREAM_SIZE_CHANGED` in `decoder.h`, Task 4. Before fixing codes 106 to 109 run `grep -rn "SDL_CODE_\|user.code" app/` and confirm that no other code uses them; the old battery-overlay branch used 106.)

In `k_ConnCallbacks` add `Session::clResizeRefused` after `Session::clSetAdaptiveTriggers` (with a comma between).

In the constructor initializer list after `m_MicrophoneEnabled(false)` add:

```cpp
      m_StatusOverlayOwner(StatusOverlayOwner::None),
      m_ResizeTimeoutTimer(0),
      m_StatusOverlayTimer(0),
      m_StatusOverlayGeneration(0)
```

In `clConnectionStatusUpdate()` set the owner: in `case CONN_STATUS_POOR:` add `s_ActiveSession->m_StatusOverlayOwner = StatusOverlayOwner::Connection;` before the `break;`, and in `case CONN_STATUS_OKAY:` add `s_ActiveSession->m_StatusOverlayOwner = StatusOverlayOwner::None;` before the `break;`.

In `notifyMouseEmulationMode()` add `m_StatusOverlayOwner = StatusOverlayOwner::MouseEmulation;` in the `if` branch and `m_StatusOverlayOwner = StatusOverlayOwner::None;` in the `else` branch.

After `clSetAdaptiveTriggers()` add:

```cpp
void Session::clResizeRefused(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Host refused resize request %u to %ux%u with reason %u",
                requestId, width, height, reason);

    // Handle it on the main thread, which owns the pending state and the overlay
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_RESIZE_REFUSED;
    event.user.data1 = (void*)(uintptr_t)requestId;
    event.user.data2 = (void*)(uintptr_t)reason;
    SDL_PushEvent(&event);
}

Uint32 Session::resizeTimeoutTimerCallback(Uint32, void* param)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_RESIZE_TIMEOUT;
    event.user.data1 = param;
    SDL_PushEvent(&event);

    // One shot
    return 0;
}

Uint32 Session::statusOverlayTimerCallback(Uint32, void* param)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = SDL_CODE_RESIZE_STATUS_TIMEOUT;
    event.user.data1 = param;
    SDL_PushEvent(&event);

    // One shot
    return 0;
}

void Session::showResizeStatus(const char* text)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Live resize: %s", text);

    // The connection warning and the gamepad mouse mode keep the overlay
    if (m_StatusOverlayOwner == StatusOverlayOwner::Connection || m_MouseEmulationRefCount > 0) {
        return;
    }

    m_OverlayManager.updateOverlayText(Overlay::OverlayStatusUpdate, text);
    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, true);
    m_StatusOverlayOwner = StatusOverlayOwner::Resize;

    if (m_StatusOverlayTimer != 0) {
        SDL_RemoveTimer(m_StatusOverlayTimer);
    }
    m_StatusOverlayGeneration++;
    m_StatusOverlayTimer = SDL_AddTimer(RESIZE_STATUS_MS, statusOverlayTimerCallback,
                                        (void*)(uintptr_t)m_StatusOverlayGeneration);
}

void Session::clearPendingResize()
{
    m_PendingResize.clear();

    if (m_ResizeTimeoutTimer != 0) {
        SDL_RemoveTimer(m_ResizeTimeoutTimer);
        m_ResizeTimeoutTimer = 0;
    }
}

void Session::requestLiveResize()
{
    int width, height;

    if (!LiIsLiveResizeSupported()) {
        showResizeStatus("Host does not support live resize");
        return;
    }

    if (m_PendingResize.active) {
        showResizeStatus("Resize in progress");
        return;
    }

    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN) {
        // Exclusive full screen reports the emulated mode, so read the desktop mode
        SDL_DisplayMode mode;
        SDL_Rect safeArea;

        if (!StreamUtils::getNativeDesktopMode(SDL_GetWindowDisplayIndex(m_Window), &mode, &safeArea)) {
            showResizeStatus("Cannot read the display mode");
            return;
        }

        width = mode.w;
        height = mode.h;
    }
    else {
        // Windowed and borderless full screen report the real pixel size
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
    }

    LiveResize::roundDownEven(width, height);

    if (width == m_ActiveVideoWidth && height == m_ActiveVideoHeight) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Live resize: stream is already %dx%d", width, height);
        return;
    }

    uint32_t requestId;
    if (LiSendResizeRequest((uint16_t)width, (uint16_t)height, &requestId) != 0) {
        showResizeStatus("Could not send the resize request");
        return;
    }

    m_PendingResize.begin(width, height, requestId);
    m_ResizeTimeoutTimer = SDL_AddTimer(RESIZE_TIMEOUT_MS, resizeTimeoutTimerCallback, (void*)(uintptr_t)requestId);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Requested live resize from %dx%d to %dx%d (request %u)",
                m_ActiveVideoWidth, m_ActiveVideoHeight, width, height, requestId);
}
```

In the `SDL_USEREVENT` switch of `execInternal()` before `default:` add:

```cpp
            case SDL_CODE_RESIZE_REFUSED: {
                uint32_t requestId = (uint32_t)(uintptr_t)event.user.data1;
                uint16_t reason = (uint16_t)(uintptr_t)event.user.data2;

                if (!m_PendingResize.matchesRefusal(requestId)) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Ignoring refusal of resize request %u (not pending)", requestId);
                    break;
                }

                char text[128];
                LiveResize::reasonText(reason, m_PendingResize.width, m_PendingResize.height, text, sizeof(text));
                clearPendingResize();
                showResizeStatus(text);
                break;
            }
            case SDL_CODE_RESIZE_TIMEOUT:
                if (m_PendingResize.matchesRefusal((uint32_t)(uintptr_t)event.user.data1)) {
                    clearPendingResize();
                    showResizeStatus("Host did not answer the resize request");
                }
                break;
            case SDL_CODE_RESIZE_STATUS_TIMEOUT:
                if ((uint32_t)(uintptr_t)event.user.data1 != m_StatusOverlayGeneration) {
                    // A timer of an older message that fired before its removal
                    break;
                }
                m_StatusOverlayTimer = 0;
                if (m_StatusOverlayOwner == StatusOverlayOwner::Resize) {
                    m_OverlayManager.setOverlayState(Overlay::OverlayStatusUpdate, false);
                    m_StatusOverlayOwner = StatusOverlayOwner::None;
                }
                break;
```

After the `DispatchDeferredCleanup:` label, directly after `StreamUtils::exitAsyncLoggingMode();`, add:

```cpp
    // Stop the live resize timers before this object can go away
    clearPendingResize();
    if (m_StatusOverlayTimer != 0) {
        SDL_RemoveTimer(m_StatusOverlayTimer);
        m_StatusOverlayTimer = 0;
    }
```

- [ ] **Step 8: Build and test by hand against the installed host**

Build: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && qmake6 moonlight-qt.pro && make release -j$(nproc) 2>&1 | tail -3'`.

Run `moonlight-mic`, start the Desktop stream in windowed mode, press `Ctrl+Alt+Shift+R`.
Expected: the overlay shows "Host does not support live resize" and hides after 5 s; the journal shows `Live resize: Host does not support live resize`; the host log (agentbus, `Select-String -Pattern 'Unknown' sunshine.log`) shows no new `type [Unknown]` line, because no message was sent.

Review Focus 4: press the hotkey and quit the stream with `Ctrl+Alt+Shift+Q` within one second. Expected: the client returns to the GUI without a crash and `journalctl --user -o cat -n 50` shows no SDL assertion.

- [ ] **Step 9: Commit**

```bash
cd ~/GitHub/moonlight-qt-mic
git add app/streaming/liveresize.h app/tests/liveresize_test.cpp app/streaming/input/input.h app/streaming/input/input.cpp app/streaming/input/keyboard.cpp app/streaming/session.h app/streaming/session.cpp app/app.pro
git commit -m "Add the live resize hotkey and request state

Ctrl+Alt+Shift+R sends a resize request with the window pixel size,
rounded down to even. Refusals and a 10 s timeout show in the status
overlay for 5 s. The decoder side follows in the next commit."
```

---

### Task 4: Client decoder: detect the new size and recreate

**Files:**
- Modify: `app/streaming/liveresize.h` (add `FrameSizeClass`, `classifyFrameSize()`)
- Modify: `app/tests/liveresize_test.cpp` (add the classification test)
- Modify: `app/streaming/video/decoder.h:7` (event code), `:76-90` (interface)
- Modify: `app/streaming/video/ffmpeg.h:30-32` (override), `:126-129` (members)
- Modify: `app/streaming/video/ffmpeg.cpp:1-5` (include), `:230-240` (ctor), `:1927-1946` (crop block), `:2041-2043` (submit)
- Modify: `app/streaming/session.h` (declare `applyStreamSize`), `app/streaming/session.cpp` (`requestLiveResize`, `clearPendingResize`, user event, new function)

**Interfaces:**
- Consumes: `PendingRequest`, `clearPendingResize()`, `m_PendingResize` (Task 3); `SDL_RENDER_DEVICE_RESET` handler (`session.cpp:2224-2300`); `SdlInputHandler::setStreamSize()` (Task 3).
- Produces: `virtual void IVideoDecoder::setExpectedFrameSize(int width, int height)` (default empty body; `0, 0` clears); `#define SDL_CODE_STREAM_SIZE_CHANGED 106` in `decoder.h` (`data1` = width, `data2` = height); `enum class LiveResize::FrameSizeClass { Original, Padding, NewSize }`; `LiveResize::classifyFrameSize(int frameW, int frameH, int expectedW, int expectedH, int originalW, int originalH)`; `void Session::applyStreamSize(int width, int height)`.

- [ ] **Step 1: Write the failing test**

In `app/tests/liveresize_test.cpp` add before `main()`:

```cpp
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
```

and call `testClassifyFrameSize();` in `main()` after `testReasonText();`.

- [ ] **Step 2: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test && /tmp/liveresize_test'`
Expected: `'FrameSizeClass' is not a member of 'LiveResize'`.

- [ ] **Step 3: Add the classification**

In `app/streaming/liveresize.h` before the closing `}` of the namespace add:

```cpp
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
```

- [ ] **Step 4: Run the test to see it pass**

Run the command of Step 2.
Expected: `liveresize_test: all checks passed`.

- [ ] **Step 5: Decoder interface**

In `app/streaming/video/decoder.h` after `#define SDL_CODE_FRAME_READY 0` add:

```cpp
// Pushed by a decoder when a frame arrives at a new stream size.
// data1 = width, data2 = height. The session recreates the decoder.
#define SDL_CODE_STREAM_SIZE_CHANGED 106
```

In `class IVideoDecoder` after `virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) = 0;` add:

```cpp
    // Tells the decoder which frame size a pending live resize request asked for,
    // so that a frame of that size is not taken for encoder padding. 0, 0 clears it.
    virtual void setExpectedFrameSize(int width, int height) { (void)width; (void)height; }
```

In `app/streaming/video/ffmpeg.h` after `virtual bool notifyWindowChanged(...) override;` add:

```cpp
    virtual void setExpectedFrameSize(int width, int height) override;
```

and after `int m_OriginalVideoHeight;` add:

```cpp
    // Set by setExpectedFrameSize() from the main thread, read by the decoder thread
    std::atomic<int> m_ExpectedVideoWidth;
    std::atomic<int> m_ExpectedVideoHeight;
```

Add `#include <atomic>` to `ffmpeg.h` with the other standard includes.

- [ ] **Step 6: Decoder implementation**

In `app/streaming/video/ffmpeg.cpp` add `#include "streaming/liveresize.h"` after `#include "streaming/session.h"`.

In the constructor initializer list after `m_VideoFormat(0),` add:

```cpp
      m_ExpectedVideoWidth(0),
      m_ExpectedVideoHeight(0),
```

After `notifyWindowChanged()` add:

```cpp
void FFmpegVideoDecoder::setExpectedFrameSize(int width, int height)
{
    m_ExpectedVideoWidth = width;
    m_ExpectedVideoHeight = height;
}
```

In `decoderThreadProc()` replace the crop block

```cpp
                    if (frame->width != m_OriginalVideoWidth || frame->height != m_OriginalVideoHeight) {
                        int cropWidth = frame->width - m_OriginalVideoWidth;
                        int cropHeight = frame->height - m_OriginalVideoHeight;

                        if (cropWidth >= 0 && cropWidth < 64 && cropHeight >= 0 && cropHeight < 64) {
```

with

```cpp
                    LiveResize::FrameSizeClass sizeClass =
                        LiveResize::classifyFrameSize(frame->width, frame->height,
                                                      m_ExpectedVideoWidth.load(), m_ExpectedVideoHeight.load(),
                                                      m_OriginalVideoWidth, m_OriginalVideoHeight);
                    if (sizeClass == LiveResize::FrameSizeClass::Padding) {
                        int cropWidth = frame->width - m_OriginalVideoWidth;
                        int cropHeight = frame->height - m_OriginalVideoHeight;

                        {
```

(the inner block keeps the existing log and crop code; the two closing braces stay as they are).

Replace

```cpp
                    // Queue the frame for rendering (or render now if pacer is disabled)
                    m_Pacer->submitFrame(frame);
```

with

```cpp
                    if (sizeClass == LiveResize::FrameSizeClass::NewSize) {
                        // The stream size changed. The renderer was created for the old
                        // size, so drop this frame and let the session recreate us.
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "Stream size changed from %dx%d to %dx%d",
                                    m_OriginalVideoWidth, m_OriginalVideoHeight,
                                    frame->width, frame->height);

                        SDL_Event event = {};
                        event.type = SDL_USEREVENT;
                        event.user.code = SDL_CODE_STREAM_SIZE_CHANGED;
                        event.user.data1 = (void*)(uintptr_t)frame->width;
                        event.user.data2 = (void*)(uintptr_t)frame->height;
                        SDL_PushEvent(&event);

                        // Don't consume any additional data
                        SDL_AtomicSet(&m_DecoderThreadShouldQuit, 1);
                        av_frame_free(&frame);
                    }
                    else {
                        // Queue the frame for rendering (or render now if pacer is disabled)
                        m_Pacer->submitFrame(frame);
                    }
```

The frame counters and `m_FrameInfoQueue` stay in step, because the classification runs after `m_FramesOut++` and the dequeue, at the point where the frame would be submitted.

Spec 5.5 step 5 (`setExpectedFrameSize()` on the new decoder) is not needed: the new decoder is created with `m_ActiveVideoWidth/Height`, so its original size is the new size and the padding rule has the right base. `clearPendingResize()` sets the expected size to `0, 0` on whichever decoder exists.

- [ ] **Step 7: Session applies the new size**

In `app/streaming/session.h` private section add:

```cpp
    // Takes the new stream size from the decoder and recreates the decoder
    void applyStreamSize(int width, int height);
```

In `app/streaming/session.cpp`:

In `requestLiveResize()` after `m_PendingResize.begin(width, height, requestId);` add:

```cpp
    SDL_LockMutex(m_DecoderLock);
    if (m_VideoDecoder != nullptr) {
        m_VideoDecoder->setExpectedFrameSize(width, height);
    }
    SDL_UnlockMutex(m_DecoderLock);
```

In `clearPendingResize()` after `m_PendingResize.clear();` add:

```cpp
    SDL_LockMutex(m_DecoderLock);
    if (m_VideoDecoder != nullptr) {
        m_VideoDecoder->setExpectedFrameSize(0, 0);
    }
    SDL_UnlockMutex(m_DecoderLock);
```

Note: `clearPendingResize()` runs on the main thread, which also holds `m_DecoderLock` in the `SDL_RENDER_DEVICE_RESET` handler; the two never nest because `applyStreamSize()` only pushes the reset event.

After `requestLiveResize()` add:

```cpp
void Session::applyStreamSize(int width, int height)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Video stream is now %dx%d", width, height);

    if (m_PendingResize.active && !m_PendingResize.matchesFrame(width, height)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Stream size %dx%d differs from the requested %dx%d",
                    width, height, m_PendingResize.width, m_PendingResize.height);
    }
    clearPendingResize();

    // These feed chooseDecoder() in the reset handler and the mouse scaling
    m_ActiveVideoWidth = width;
    m_ActiveVideoHeight = height;
    m_StreamConfig.width = width;
    m_StreamConfig.height = height;
    m_InputHandler->setStreamSize(width, height);

    // Recreate the decoder and renderer at the new size. The handler also
    // requests an IDR frame and sets the HDR mode again.
    SDL_Event event = {};
    event.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&event);
}
```

In the `SDL_USEREVENT` switch before `case SDL_CODE_RESIZE_REFUSED:` add:

```cpp
            case SDL_CODE_STREAM_SIZE_CHANGED:
                applyStreamSize((int)(uintptr_t)event.user.data1, (int)(uintptr_t)event.user.data2);
                break;
```

- [ ] **Step 8: Build and commit**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && qmake6 moonlight-qt.pro && make release -j$(nproc) 2>&1 | tail -3 && ldd app/moonlight | grep placebo'`
Expected: successful build; `libplacebo.so` listed. Start a stream and check that video still plays (the classification returns `Original` for every frame).

```bash
cd ~/GitHub/moonlight-qt-mic
git add app/streaming/liveresize.h app/tests/liveresize_test.cpp app/streaming/video/decoder.h app/streaming/video/ffmpeg.h app/streaming/video/ffmpeg.cpp app/streaming/session.h app/streaming/session.cpp
git commit -m "Recreate the decoder when the stream size changes

A decoded frame at the requested size, or at any size that is not
encoder padding, is dropped and the session recreates the decoder and
renderer through the SDL_RENDER_DEVICE_RESET path."
```

---

### Task 5: Host: wire format, size rules and a Linux unit test

**Files:**
- Create: `~/GitHub/apollo-microphone/src/live_resize.h`
- Create: `~/GitHub/apollo-microphone/tests/unit/test_live_resize.cpp`
- Modify: `~/GitHub/apollo-microphone/cmake/compile_definitions/common.cmake:90-91` (add the header; the `.cpp` comes in Task 6)

**Interfaces:**
- Produces (used by Tasks 6-8): `namespace live_resize { constexpr uint16_t PACKET_TYPE_REQUEST = 0x3100, PACKET_TYPE_REFUSED = 0x3101; enum class reason_e : uint16_t { ok = 0, busy = 1, not_virtual_display = 2, multiple_clients = 3, size_limit = 4, display_failed = 5, encoder_failed = 6 }; struct request_payload_t { uint16_t width, height; uint32_t request_id; }; struct refused_payload_t { uint16_t width, height; uint32_t request_id; uint16_t reason; }; constexpr reason_e validate_size(int width, int height, int video_format, bool input_only); constexpr auto HOST_TIMEOUT = 15s; }`.

- [ ] **Step 1: Install googletest in the toolbox (once)**

Run: `toolbox run -c moonlight sudo dnf install -y gtest-devel`
Expected: `gtest-devel` installed (`ls /usr/include/gtest/gtest.h`).

- [ ] **Step 2: Write the failing test**

Create `tests/unit/test_live_resize.cpp`:

```cpp
/**
 * @file tests/unit/test_live_resize.cpp
 * @brief Test src/live_resize.h
 *
 * Builds without the rest of Sunshine. In the moonlight toolbox:
 *   g++ -std=c++20 -Wall -Werror -I . tests/unit/test_live_resize.cpp -lgtest -lgtest_main -pthread -o /tmp/test_live_resize && /tmp/test_live_resize
 */
#include <gtest/gtest.h>
#include <src/live_resize.h>

using live_resize::reason_e;
using live_resize::validate_size;

TEST(LiveResizeTests, PayloadSizesMatchTheSpec) {
  EXPECT_EQ(sizeof(live_resize::request_payload_t), 8u);
  EXPECT_EQ(sizeof(live_resize::refused_payload_t), 10u);
  EXPECT_EQ(live_resize::PACKET_TYPE_REQUEST, 0x3100);
  EXPECT_EQ(live_resize::PACKET_TYPE_REFUSED, 0x3101);
}

TEST(LiveResizeTests, AcceptsEvenSizesInsideTheLimits) {
  EXPECT_EQ(validate_size(2536, 1390, 1, false), reason_e::ok);
  EXPECT_EQ(validate_size(320, 200, 0, false), reason_e::ok);
  EXPECT_EQ(validate_size(4096, 4096, 0, false), reason_e::ok);
  EXPECT_EQ(validate_size(8192, 8192, 2, false), reason_e::ok);
}

TEST(LiveResizeTests, RejectsOddSizes) {
  EXPECT_EQ(validate_size(2537, 1390, 1, false), reason_e::size_limit);
  EXPECT_EQ(validate_size(2536, 1391, 1, false), reason_e::size_limit);
}

TEST(LiveResizeTests, RejectsSmallSizes) {
  // Review Focus 2: a minimized or tiny window
  EXPECT_EQ(validate_size(0, 0, 1, false), reason_e::size_limit);
  EXPECT_EQ(validate_size(318, 200, 1, false), reason_e::size_limit);
  EXPECT_EQ(validate_size(320, 198, 1, false), reason_e::size_limit);
}

TEST(LiveResizeTests, RejectsSizesAboveTheCodecLimit) {
  EXPECT_EQ(validate_size(4098, 100, 0, false), reason_e::size_limit);  // H.264
  EXPECT_EQ(validate_size(4098, 200, 1, false), reason_e::ok);  // HEVC
  EXPECT_EQ(validate_size(8194, 200, 1, false), reason_e::size_limit);
  EXPECT_EQ(validate_size(200, 8194, 2, false), reason_e::size_limit);
}

TEST(LiveResizeTests, RejectsInputOnlySessions) {
  EXPECT_EQ(validate_size(2536, 1390, 1, true), reason_e::size_limit);
}
```

- [ ] **Step 3: Run the test to see it fail to build**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/apollo-microphone && g++ -std=c++20 -Wall -Werror -I . tests/unit/test_live_resize.cpp -lgtest -lgtest_main -pthread -o /tmp/test_live_resize && /tmp/test_live_resize'`
Expected: `src/live_resize.h: No such file or directory`.

- [ ] **Step 4: Write the header**

Create `src/live_resize.h`:

```cpp
/**
 * @file src/live_resize.h
 * @brief Wire format, size rules and display change of the live resize extension.
 * @details A Moonlight client asks for a new stream size with RESIZE_REQUEST on the
 * control stream. The host changes the virtual display and the encoder size. The host
 * answers only with RESIZE_REFUSED. See docs/superpowers/specs/2026-10-09-live-resize-design.md
 * in the moonlight-qt-mic repository.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace live_resize {

  // Control message ids (Apollo live resize extension)
  constexpr std::uint16_t PACKET_TYPE_REQUEST = 0x3100;
  constexpr std::uint16_t PACKET_TYPE_REFUSED = 0x3101;

  // The host clears a request that did not finish after this time
  constexpr auto HOST_TIMEOUT = std::chrono::seconds(15);

  enum class reason_e : std::uint16_t {
    ok = 0,  ///< Not sent on the wire
    busy = 1,  ///< A resize is in progress
    not_virtual_display = 2,  ///< The capture display is not a SudoVDA monitor
    multiple_clients = 3,  ///< Another session shares the display
    size_limit = 4,  ///< The size is odd, too small, too large, or the session is input only
    display_failed = 5,  ///< The display change failed and was reverted
    encoder_failed = 6,  ///< The encoder rejected the size and the old size was restored
    // 7 (not supported) is used by the client only
  };

#pragma pack(push, 1)
  struct request_payload_t {
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t request_id;
  };

  struct refused_payload_t {
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t request_id;
    std::uint16_t reason;
  };
#pragma pack(pop)

  static_assert(sizeof(request_payload_t) == 8, "RESIZE_REQUEST payload is 8 bytes");
  static_assert(sizeof(refused_payload_t) == 10, "RESIZE_REFUSED payload is 10 bytes");

  constexpr int MIN_WIDTH = 320;
  constexpr int MIN_HEIGHT = 200;
  constexpr int MAX_DIMENSION = 8192;
  constexpr int MAX_DIMENSION_H264 = 4096;

  /**
   * @brief Check a requested size against the stream limits.
   * @param width Requested width.
   * @param height Requested height.
   * @param video_format 0 - H.264, 1 - HEVC, 2 - AV1 (video::config_t::videoFormat).
   * @param input_only True when the session streams no video.
   * @return reason_e::ok when the size is valid, otherwise reason_e::size_limit.
   */
  constexpr reason_e validate_size(int width, int height, int video_format, bool input_only) {
    if (input_only) {
      return reason_e::size_limit;
    }
    if ((width & 1) != 0 || (height & 1) != 0) {
      return reason_e::size_limit;
    }
    if (width < MIN_WIDTH || height < MIN_HEIGHT) {
      return reason_e::size_limit;
    }
    const int max_dimension = video_format == 0 ? MAX_DIMENSION_H264 : MAX_DIMENSION;
    if (width > max_dimension || height > max_dimension) {
      return reason_e::size_limit;
    }
    return reason_e::ok;
  }

#ifdef _WIN32
  /**
   * @brief Result of a display change.
   */
  struct display_result_t {
    bool ok;
    std::string message;  ///< Reason of the failure, empty on success
  };

  /**
   * @brief Set the virtual display of the running app to a new size.
   * @details Removes the SudoVDA monitor, adds it again with the new preferred mode and
   * applies the mode. When Windows keeps the saved mode, adds the monitor again with a
   * new GUID. Holds proc::vdd_lock for the whole change. Updates proc::proc.display_name,
   * config::video.output_name and the launch session size on success.
   * @param width New width.
   * @param height New height.
   * @return ok == false when no app with a virtual display runs or the display did not
   * reach the size. The caller reverts with the old size.
   */
  display_result_t change_display_size(int width, int height);
#endif

}  // namespace live_resize
```

- [ ] **Step 5: Run the test to see it pass**

Run the command of Step 3.
Expected: `[  PASSED  ] 6 tests.`

- [ ] **Step 6: Add the header to the host source list and run CI**

In `cmake/compile_definitions/common.cmake` after `"${CMAKE_SOURCE_DIR}/src/stream.h"` add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/live_resize.h"
```

```bash
cd ~/GitHub/apollo-microphone
git add src/live_resize.h tests/unit/test_live_resize.cpp cmake/compile_definitions/common.cmake
git commit -m "Add the live resize wire format and size rules

New header with the control message ids, payload structs, reason codes
and the size check, with a googletest that builds on Linux without the
rest of Sunshine."
git push origin live-resize
RUN=$(gh run list -R catapultam/apollo-microphone -L 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
```

Expected: the run passes (it also compiles the Task 1 diagnostics in `stream.cpp`). If it fails in `stream.cpp`, fix the Task 1 change (`util::hex_vec` takes two iterators or a container, see `src/utility.h:304` and `:346`) and push again.

---

### Task 6: Host: virtual display change (Windows)

**Files:**
- Modify: `src/process.h:16-30` (includes), `:104-150` (`proc_t`)
- Modify: `src/process.cpp:274-298` (fill `vdd`), `:759-767` (clear under the lock), new methods after `proc_t::running()`
- Create: `src/live_resize.cpp`
- Modify: `cmake/compile_definitions/common.cmake:90-92` (add the `.cpp`)

**Interfaces:**
- Consumes: `VDISPLAY::getDeviceSettings()`, `changeDisplaySettings()`, `changeDisplaySettings2()`, `createVirtualDisplay()`, `removeVirtualDisplay()` (`src/platform/windows/virtual_display.h`); `platf::from_utf8()` / `platf::to_utf8()` (`src/platform/windows/misc.h:28,35`); `display_device::map_display_name()` (`src/display_device.h:54`); `uuid_util::uuid_t::generate()` (`src/uuid.h:35`); `config::video.isolated_virtual_display_option`.
- Produces: `struct proc_t::vdd_t { bool valid; std::string device_uuid; std::string device_name; std::uint32_t target_fps; GUID guid; }`, member `proc_t::vdd`, namespace mutex `proc::vdd_lock`, `void proc_t::set_vdd_guid(const GUID&)`, `void proc_t::set_vdd_size(int, int)`, and `live_resize::change_display_size(int, int)` (declared in Task 5).

Why a namespace mutex: `proc_t` uses `KITTY_DEFAULT_CONSTR_MOVE_THROW`, so a `std::mutex` member would delete its move constructor, which `proc::refresh()` needs.

- [ ] **Step 1: proc_t state**

In `src/process.h` add `#include <mutex>` after `#include <optional>`. In `class proc_t` after `bool allow_client_commands = false;` add:

```cpp
#ifdef _WIN32
    /**
     * @brief The virtual display of the running app, kept for live resize.
     * @details execute() fills it and terminate() clears it, both under proc::vdd_lock.
     */
    struct vdd_t {
      bool valid = false;
      std::string device_uuid;
      std::string device_name;
      std::uint32_t target_fps = 0;
      GUID guid {};
    };

    vdd_t vdd;

    /**
     * @brief Store a new GUID after live resize added the display with a new identity.
     * @details The caller holds proc::vdd_lock. Also updates the launch session, which terminate() uses.
     */
    void set_vdd_guid(const GUID &guid);

    /**
     * @brief Store the new stream size in the launch session after a live resize.
     */
    void set_vdd_size(int width, int height);
#endif
```

After the class, inside `namespace proc`, add:

```cpp
#ifdef _WIN32
  /**
   * @brief Held while the virtual display is removed and added again, and while terminate() removes it.
   */
  extern std::mutex vdd_lock;
#endif
```

- [ ] **Step 2: proc_t implementation**

In `src/process.cpp` after `VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = ...;` (line 63) add:

```cpp
  std::mutex vdd_lock;
```

In `proc_t::execute()`, after the `if (config::video.double_refreshrate) { target_fps *= 2; }` block and before `std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(` add:

```cpp
        {
          std::lock_guard lg(vdd_lock);
          vdd.valid = true;
          vdd.device_uuid = device_uuid_str;
          vdd.device_name = device_name;
          vdd.target_fps = target_fps;
          vdd.guid = launch_session->display_guid;
        }
```

In `proc_t::terminate()` replace

```cpp
    if (used_virtual_display) {
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
```

with

```cpp
    if (used_virtual_display) {
      // Hold the lock so a live resize worker cannot add the display back after this remove
      std::lock_guard lg(vdd_lock);
      vdd = {};
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
```

After `proc_t::running()` add:

```cpp
#ifdef _WIN32
  void proc_t::set_vdd_guid(const GUID &guid) {
    vdd.guid = guid;
    if (_launch_session) {
      _launch_session->display_guid = guid;
    }
  }

  void proc_t::set_vdd_size(int width, int height) {
    if (_launch_session) {
      _launch_session->width = width;
      _launch_session->height = height;
    }
  }
#endif
```

- [ ] **Step 3: The display change**

Create `src/live_resize.cpp`:

```cpp
/**
 * @file src/live_resize.cpp
 * @brief Virtual display change of the live resize extension (Windows).
 */
#include "live_resize.h"

#ifdef _WIN32

  #include <cstring>
  #include <mutex>

  #include "config.h"
  #include "display_device.h"
  #include "logging.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
  #include "process.h"
  #include "uuid.h"

using namespace std::literals;

namespace live_resize {

  // Reads the current mode of a display. Returns false when the display is not found.
  static bool read_mode(const std::wstring &name, int &width, int &height) {
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (VDISPLAY::getDeviceSettings(name.c_str(), mode) != ERROR_SUCCESS) {
      return false;
    }
    width = (int) mode.dmPelsWidth;
    height = (int) mode.dmPelsHeight;
    return true;
  }

  // Adds the monitor with this GUID at the size and applies the mode, as proc_t::execute() does.
  // Returns the device name, or an empty string when the driver gave no name in time.
  static std::wstring add_and_apply(const proc::proc_t::vdd_t &vdd, const GUID &guid, int width, int height) {
    auto name = VDISPLAY::createVirtualDisplay(vdd.device_uuid.c_str(), vdd.device_name.c_str(), width, height, vdd.target_fps, guid);
    if (name.empty()) {
      return name;
    }

    // Spike S1b: after a re-add Windows first shows the saved mode; this call applies the new one
    VDISPLAY::changeDisplaySettings(name.c_str(), width, height, vdd.target_fps);
    if (config::video.isolated_virtual_display_option) {
      VDISPLAY::changeDisplaySettings2(name.c_str(), width, height, vdd.target_fps, true);
    }
    return name;
  }

  static bool is_at_size(const std::wstring &name, int width, int height, int &current_width, int &current_height) {
    current_width = 0;
    current_height = 0;
    return !name.empty() && read_mode(name, current_width, current_height) && current_width == width && current_height == height;
  }

  display_result_t change_display_size(int width, int height) {
    auto &proc = proc::proc;
    std::lock_guard lg(proc::vdd_lock);

    if (proc.running() == 0 || !proc.vdd.valid || !proc.virtual_display) {
      return {false, "no app with a virtual display runs"};
    }

    // A failed remove is not fatal: the add below returns the existing monitor for a known GUID
    if (!VDISPLAY::removeVirtualDisplay(proc.vdd.guid)) {
      BOOST_LOG(warning) << "Live resize: could not remove the virtual display before the re-add"sv;
    }

    int current_width, current_height;
    auto name = add_and_apply(proc.vdd, proc.vdd.guid, width, height);
    bool at_size = is_at_size(name, width, height, current_width, current_height);

    if (!at_size) {
      // Spike S1: Windows kept the mode it saved for this monitor identity. Use a new identity.
      BOOST_LOG(warning) << "Live resize: display ["sv << platf::to_utf8(name) << "] is at "sv
                         << current_width << 'x' << current_height << ", adding it again with a new GUID"sv;
      if (!name.empty()) {
        VDISPLAY::removeVirtualDisplay(proc.vdd.guid);
      }

      auto new_uuid = uuid_util::uuid_t::generate();
      GUID new_guid;
      static_assert(sizeof(new_guid) == sizeof(new_uuid), "GUID and uuid_t have the same size");
      std::memcpy(&new_guid, &new_uuid, sizeof(new_guid));
      proc.set_vdd_guid(new_guid);

      name = add_and_apply(proc.vdd, new_guid, width, height);
      at_size = is_at_size(name, width, height, current_width, current_height);
    }

    if (!at_size) {
      return {false, "display is at " + std::to_string(current_width) + "x" + std::to_string(current_height) + " instead of " + std::to_string(width) + "x" + std::to_string(height)};
    }

    // The capture thread finds the display by this name at its next reinit
    proc.display_name = platf::to_utf8(name);
    config::video.output_name = display_device::map_display_name(proc.display_name);
    proc.set_vdd_size(width, height);

    BOOST_LOG(info) << "Live resize: display ["sv << proc.display_name << "] is now "sv << width << 'x' << height;
    return {true, {}};
  }

}  // namespace live_resize

#endif
```

In `cmake/compile_definitions/common.cmake` after `"${CMAKE_SOURCE_DIR}/src/live_resize.h"` add:

```cmake
        "${CMAKE_SOURCE_DIR}/src/live_resize.cpp"
```

- [ ] **Step 4: CI compile check and commit**

```bash
cd ~/GitHub/apollo-microphone
git add src/process.h src/process.cpp src/live_resize.cpp cmake/compile_definitions/common.cmake
git commit -m "Add the virtual display change for live resize

proc_t keeps the virtual display parameters and a lock that terminate()
and the resize worker share. change_display_size() removes the SudoVDA
monitor, adds it again at the new size and applies the mode, with a new
GUID when Windows keeps the saved mode."
git push origin live-resize
RUN=$(gh run list -R catapultam/apollo-microphone -L 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
```

Expected: the run passes. The function is not called yet; the compile is the test. On a compile error in `live_resize.cpp`, read the log with `gh run view "$RUN" -R catapultam/apollo-microphone --log-failed | grep -A3 'error:'`, fix, amend and push again.

---

### Task 7: Host: capture and encoder follow the new size

**Files:**
- Modify: `src/globals.h:46-57` (mail names)
- Modify: `src/video.cpp:1-30` (include), `:1903-1917` (encode_run start), `:2354-2436` (capture_async)

**Interfaces:**
- Consumes: `live_resize::reason_e` (Task 5); `safe::mail_raw_t::event_t<T>` (`pop(0ms)` returns `bool` for `T = bool`, `std::optional<T>` otherwise, `src/utility.h:855`).
- Produces: mail names `mail::resize` (`std::pair<int,int>`, raised by the control handler), `mail::resize_refused` (`std::uint16_t` reason, raised here and by the worker), `mail::resize_done` (`bool`, raised by `encode_run` at every encoder session start), `mail::encoder_failed` (`bool`, raised by `encode_run` when `make_encode_session()` returns null). Task 8 drains `resize_done` and `resize_refused` on the control thread.

How it works (spec 4.4): `capture_async` reads `mail::resize` at the top of its loop, which it reaches when `encode_run` returns on the reinit that the display change caused. The new size goes into its `config`. `encode_run` cannot report back while it runs, so it raises `resize_done` once the encoder session exists, and `encoder_failed` when it does not. After `encode_run` returns, `capture_async` restores the last good config when the encoder failed during a resize.

- [ ] **Step 1: Mail names**

In `src/globals.h` after `MAIL(hdr);` add:

```cpp
  // Live resize: new size for capture_async, refusal reason for the control thread,
  // encoder session start, encoder session failure
  MAIL(resize);
  MAIL(resize_refused);
  MAIL(resize_done);
  MAIL(encoder_failed);
```

- [ ] **Step 2: encode_run reports the encoder state**

In `src/video.cpp` add `#include "live_resize.h"` after `#include "process.h"` (line 22).

In `encode_run()` replace

```cpp
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }
```

with

```cpp
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      // capture_async reverts a live resize when this follows a size change
      mail->event<bool>(mail::encoder_failed)->raise(true);
      return;
    }

    // Live resize: tell the control thread that an encoder runs at config.width x config.height.
    // The control thread ignores this when no resize is in progress.
    mail->event<bool>(mail::resize_done)->raise(true);
```

- [ ] **Step 3: capture_async consumes the new size**

In `capture_async()` after `auto hdr_event = mail->event<hdr_info_t>(mail::hdr);` add:

```cpp
    // Live resize state (see src/live_resize.h)
    auto resize_event = mail->event<std::pair<int, int>>(mail::resize);
    auto encoder_failed_event = mail->event<bool>(mail::encoder_failed);
    auto resize_refused_event = mail->event<std::uint16_t>(mail::resize_refused);
    config_t last_good_config = config;
    bool resize_pending = false;
```

After the block that gets `display` (ends with `display = ref->display_wp->lock(); }`) and before `auto &encoder = *chosen_encoder;` add:

```cpp
      // A live resize changes the encoder size. The display change that goes with it
      // caused the reinit that brought us here, so the display has the new size too.
      if (auto size = resize_event->pop(0ms)) {
        last_good_config = config;
        config.width = size->first;
        config.height = size->second;
        resize_pending = true;
        BOOST_LOG(info) << "Live resize: encoder size set to "sv << config.width << 'x' << config.height;
      }
```

After the `encode_run(...)` call (its closing `);`) add:

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

Note on R2 (headless mode): the capture thread's reinit loop (`src/video.cpp:1333-1393`) retries `refresh_displays()` until a display opens and prefers `proc::proc.display_name`, which `change_display_size()` set before the add. Without `headless_mode` and with a physical display present, the loop can pick the physical display during the gap; the host then refuses nothing but streams the wrong display until the next reinit. This is outside the spec (the user runs `headless_mode`), and the reviewer should name it as a known limit.

- [ ] **Step 4: CI compile check and commit**

```bash
cd ~/GitHub/apollo-microphone
git add src/globals.h src/video.cpp
git commit -m "Let the encoder follow a live resize

capture_async takes the new size from the resize mail at the reinit
that the display change causes. encode_run reports the encoder start
and failure through mail, so the control thread can finish or refuse
the request."
git push origin live-resize
RUN=$(gh run list -R catapultam/apollo-microphone -L 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
```

Expected: the run passes.

---

### Task 8: Host: control handler, refusal, worker and SDP attribute

**Files:**
- Modify: `src/stream.h:42-60` (declare `running_sessions`), add `#include <atomic>`
- Modify: `src/stream.cpp:44-77` (ids), `:17-42` (includes), `:242-249` (struct), `:420-430` (session state), `:909-937` (send), `:942-1066` (handler), `:1189-1215` (drain), `:2376-2385` (init)
- Modify: `src/rtsp.cpp:30` (include), `:791-792` (attribute)

**Interfaces:**
- Consumes: `live_resize::*` (Task 5), `change_display_size()` (Task 6), mail names (Task 7), `encode_control()` (`stream.cpp:446`), `session::uuid()` (`stream.h:51`), `rtsp_stream::find_session()` (`src/rtsp.h:82`), `session::running_sessions` (`stream.cpp:2081`), `proc::vDisplayDriverStatus` (`src/process.h:46`).
- Produces: `IDX_RESIZE_REQUEST 19`, `IDX_RESIZE_REFUSED 20` in `packetTypes`; `session_t::resize` state; `int send_resize_refused(session_t*, std::uint16_t, std::uint16_t, std::uint32_t, std::uint16_t)`; `void live_resize_worker(std::string, int, int, int, int)`; the SDP line `a=x-ss-general.liveResize:1`.

- [ ] **Step 1: Packet ids and includes**

In `src/stream.cpp` after `#define IDX_SET_ADAPTIVE_TRIGGERS 18` add:

```cpp
#define IDX_RESIZE_REQUEST 19
#define IDX_RESIZE_REFUSED 20
```

Append to `packetTypes[]` after `0x5503,  // Set Adaptive triggers ...`:

```cpp
  0x3100,  // Resize request (Apollo live resize extension)
  0x3101,  // Resize refused (Apollo live resize extension)
```

Add `#include "live_resize.h"` and `#include "rtsp.h"` to the project includes after `#include "process.h"`.

In `src/stream.h` add `#include <atomic>` with the other includes and, inside `namespace session` after `void graceful_stop(session_t& session);`, add:

```cpp
    // Number of sessions in state RUNNING, counted in start() and stop()
    extern std::atomic_uint running_sessions;
```

- [ ] **Step 2: Structs and session state**

In `src/stream.cpp` inside the `#pragma pack(push, 1)` region, after `struct mic_packet_header_t { ... };` add:

```cpp
  struct control_resize_refused_t {
    control_header_v2 header;

    live_resize::refused_payload_t payload;
  };
```

In `session_t` after the `control` struct (after its closing `} control;`) add:

```cpp
    struct {
      std::atomic<bool> in_progress {false};  ///< Set by the handler, cleared by the control thread
      int width;  ///< Size the host targets now; the control thread writes it
      int height;
      int last_width;  ///< Size before the pending request, for the revert
      int last_height;
      std::uint32_t request_id;
      std::chrono::steady_clock::time_point started;
      safe::mail_raw_t::event_t<std::uint16_t> refused_queue;  ///< Reason from the worker or capture_async
      safe::mail_raw_t::event_t<bool> done_queue;  ///< Raised by encode_run at each encoder start
    } resize;
```

In `session::alloc()` before `session->mail = std::move(mail);` add:

```cpp
      session->resize.in_progress = false;
      session->resize.width = config.monitor.width;
      session->resize.height = config.monitor.height;
      session->resize.last_width = config.monitor.width;
      session->resize.last_height = config.monitor.height;
      session->resize.request_id = 0;
      session->resize.refused_queue = mail->event<std::uint16_t>(mail::resize_refused);
      session->resize.done_queue = mail->event<bool>(mail::resize_done);
```

- [ ] **Step 3: Refusal send and the worker**

After `send_hdr_mode()` add:

```cpp
  int send_resize_refused(session_t *session, std::uint16_t width, std::uint16_t height, std::uint32_t request_id, std::uint16_t reason) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send resize refusal, still waiting for PING from Moonlight"sv;
      return -1;
    }

    control_resize_refused_t plaintext {};
    plaintext.header.type = packetTypes[IDX_RESIZE_REFUSED];
    plaintext.header.payloadLength = sizeof(live_resize::refused_payload_t);
    plaintext.payload.width = width;
    plaintext.payload.height = height;
    plaintext.payload.request_id = request_id;
    plaintext.payload.reason = reason;

    std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send resize refusal to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(info) << "Refused resize request "sv << request_id << " to "sv << width << 'x' << height << " with reason "sv << reason;
    return 0;
  }

#ifdef _WIN32
  /**
   * @brief Change the virtual display for a resize request, off the control thread.
   * @details Keeps only the session uuid. On failure, reverts the display and queues DISPLAY_FAILED.
   */
  void live_resize_worker(std::string session_uuid, int width, int height, int old_width, int old_height) {
    auto result = live_resize::change_display_size(width, height);
    if (result.ok) {
      // The capture thread sees the display loss and reinitializes with the new size
      return;
    }

    BOOST_LOG(warning) << "Live resize to "sv << width << 'x' << height << " failed: "sv << result.message;

    auto session = rtsp_stream::find_session(session_uuid);
    if (!session) {
      // The session ended; proc_t::terminate() removes the display
      return;
    }

    // Take the size back before capture_async reads it, or give capture_async the old size
    auto resize_event = session->mail->event<std::pair<int, int>>(mail::resize);
    if (!resize_event->pop(0ms)) {
      resize_event->raise(std::make_pair(old_width, old_height));
    }

    auto revert = live_resize::change_display_size(old_width, old_height);
    if (!revert.ok) {
      BOOST_LOG(error) << "Live resize: revert to "sv << old_width << 'x' << old_height << " failed: "sv << revert.message;
    }

    session->resize.refused_queue->raise((std::uint16_t) live_resize::reason_e::display_failed);
  }
#endif
```

- [ ] **Step 4: The control handler**

In `controlBroadcastThread()` after the `IDX_FILE_TRANSFER_NONCE_REQUEST` mapping (before `server->map(packetTypes[IDX_ENCRYPTED], ...)`) add:

```cpp
    server->map(packetTypes[IDX_RESIZE_REQUEST], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_RESIZE_REQUEST]"sv;

      if (payload.size() < sizeof(live_resize::request_payload_t)) {
        BOOST_LOG(warning) << "Resize request: runt payload of "sv << payload.size() << " bytes"sv;
        return;
      }

      live_resize::request_payload_t request;
      std::memcpy(&request, payload.data(), sizeof(request));
      const int width = request.width;
      const int height = request.height;

      auto refuse = [&](live_resize::reason_e reason) {
        send_resize_refused(session, request.width, request.height, request.request_id, (std::uint16_t) reason);
      };

      auto &resize = session->resize;

      if (width == resize.width && height == resize.height) {
        BOOST_LOG(debug) << "Resize request to the current size "sv << width << 'x' << height << ", ignored"sv;
        return;
      }

      if (resize.in_progress) {
        refuse(live_resize::reason_e::busy);
        return;
      }

#ifdef _WIN32
      if (!proc::proc.virtual_display || proc::vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        refuse(live_resize::reason_e::not_virtual_display);
        return;
      }
#else
      refuse(live_resize::reason_e::not_virtual_display);
      return;
#endif

      // One virtual display serves all sessions; a resize would change the picture for the others
      if (session::running_sessions.load(std::memory_order_acquire) > 1) {
        refuse(live_resize::reason_e::multiple_clients);
        return;
      }

      auto size_check = live_resize::validate_size(width, height, session->config.monitor.videoFormat, session->config.monitor.input_only);
      if (size_check != live_resize::reason_e::ok) {
        refuse(size_check);
        return;
      }

      resize.in_progress = true;
      resize.started = std::chrono::steady_clock::now();
      resize.request_id = request.request_id;
      resize.last_width = resize.width;
      resize.last_height = resize.height;
      resize.width = width;
      resize.height = height;

      // capture_async reads this at its next reinit, which the display change causes
      session->mail->event<std::pair<int, int>>(mail::resize)->raise(std::make_pair(width, height));

      BOOST_LOG(info) << "Resize request "sv << request.request_id << " from ["sv << session->device_name
                      << "] from "sv << resize.last_width << 'x' << resize.last_height << " to "sv << width << 'x' << height;

#ifdef _WIN32
      // The display change takes up to seconds; it must not block the control thread
      std::thread worker(live_resize_worker, session::uuid(*session), width, height, resize.last_width, resize.last_height);
      worker.detach();
#endif
    });
```

`refuse` with `size_check`: `validate_size` returns `reason_e`, so `refuse(size_check)` compiles as is.

- [ ] **Step 5: Drain the results on the control thread**

In the session loop of `controlBroadcastThread()`, after the `hdr_queue` drain (after `send_hdr_mode(session, std::move(hdr_info)); }`) and before the closing `}` of `else {`, add:

```cpp
            // Live resize results. Refusals first: a refusal and an encoder start can be
            // pending together after an encoder failure, and the refusal must win.
            auto &resize = session->resize;
            while (session->control.peer && resize.refused_queue->peek()) {
              auto reason = resize.refused_queue->pop();
              if (!reason || !resize.in_progress) {
                continue;
              }

              send_resize_refused(session, (std::uint16_t) resize.width, (std::uint16_t) resize.height, resize.request_id, *reason);
              resize.width = resize.last_width;
              resize.height = resize.last_height;
              resize.in_progress = false;

#ifdef _WIN32
              if (*reason == (std::uint16_t) live_resize::reason_e::encoder_failed) {
                // capture_async went back to the old size; put the display back too
                std::thread revert([width = resize.width, height = resize.height] {
                  auto result = live_resize::change_display_size(width, height);
                  if (!result.ok) {
                    BOOST_LOG(error) << "Live resize: revert after encoder failure to "sv << width << 'x' << height << " failed: "sv << result.message;
                  }
                });
                revert.detach();
              }
#endif
            }

            while (resize.done_queue->peek()) {
              resize.done_queue->pop();
              if (resize.in_progress) {
                BOOST_LOG(info) << "Resize request "sv << resize.request_id << " done: encoder runs at "sv << resize.width << 'x' << resize.height;
                resize.in_progress = false;
              }
            }

            if (resize.in_progress && now - resize.started > live_resize::HOST_TIMEOUT) {
              // Watchdog: a lost result must not block the session
              BOOST_LOG(warning) << "Resize request "sv << resize.request_id << " did not finish in time, state cleared"sv;
              resize.in_progress = false;
            }
```

`now` is the `std::chrono::steady_clock::now()` taken at the top of the loop (line 1151).

- [ ] **Step 6: SDP attribute**

In `src/rtsp.cpp` add `#include "process.h"` after `#include "rtsp.h"`. In `cmd_describe()` after `ss << "a=x-ss-general.featureFlags:" << (uint32_t) platf::get_capabilities() << std::endl;` add:

```cpp
#ifdef _WIN32
    // Live resize needs the SudoVDA driver (Apollo live resize extension)
    if (proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      ss << "a=x-ss-general.liveResize:1"sv << std::endl;
    }
#endif
```

- [ ] **Step 7: CI build, download the artifact, commit**

```bash
cd ~/GitHub/apollo-microphone
git add src/stream.h src/stream.cpp src/rtsp.cpp
git commit -m "Handle live resize requests on the control stream

Validate the request, start the display change in a worker thread and
let the capture thread pick up the new size. Send RESIZE_REFUSED from
the control thread only. Advertise the feature in the RTSP DESCRIBE
reply with a=x-ss-general.liveResize:1."
git push origin live-resize
RUN=$(gh run list -R catapultam/apollo-microphone -L 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RUN" -R catapultam/apollo-microphone --exit-status
rm -rf /tmp/live-resize/host-artifact && mkdir -p /tmp/live-resize/host-artifact
gh run download "$RUN" -R catapultam/apollo-microphone -n build-Windows-AMD64 -D /tmp/live-resize/host-artifact
ls -la /tmp/live-resize/host-artifact && sha256sum /tmp/live-resize/host-artifact/Apollo.exe
echo "$RUN" > /tmp/live-resize/host-artifact/RUN_ID
```

Expected: the run passes; `Apollo.exe` and `Apollo.zip` are in `/tmp/live-resize/host-artifact/`. Record the run id and the sha256 for Task 9.

---

### Task 9: Host deploy on CPLT-4A with backup and rollback

**Files:**
- None in the repositories. Artifacts: `/tmp/live-resize/host-artifact/Apollo.exe` (Task 8). Host paths: `C:\Program Files\Apollo` (install), `C:\Program Files\Apollo\config` (must survive), `C:\ApolloBackup` (new).

**Interfaces:**
- Consumes: the CI artifact and run id of Task 8; the agentbus session `agentbus:cplt-4a/alex-ba55d7` (its permission classifier may block scripts; every command below is one line so a block is visible per step); the user inside a Moonlight stream for the fallback.
- Produces: the new host running on CPLT-4A; backup zip and config copy under `C:\ApolloBackup`; a release tag `live-resize-<date>` on `catapultam/apollo-microphone` (public fork) as the file transport.

The user has no copy of the installed version's installer, so the backup of the install directory is the only rollback. The NSIS installer stops `ApolloService`, copies the files, installs the (unchanged) SudoVDA driver and starts the service; the config directory is not touched (spec 6.1, `cmake/packaging/windows_nsis.cmake`). A running stream ends when the service stops.

- [ ] **Step 1: Publish the installer as a release asset**

```bash
cd /tmp/live-resize/host-artifact
TAG="live-resize-$(date +%Y%m%d-%H%M)"
gh release create "$TAG" Apollo.exe -R catapultam/apollo-microphone --prerelease \
  --title "Live resize test build $TAG" \
  --notes "Test build of branch live-resize, CI run $(cat RUN_ID). Not for general use."
echo "$TAG" > TAG
sha256sum Apollo.exe
```

Expected: the release exists; keep the sha256 and the tag.

- [ ] **Step 2: Back up the host through the agentbus session**

Ask the agentbus session to run these three commands one at a time and to report each output. Stop a running stream first (the backup reads files while the service runs; that is safe, but a stream makes the step slower).

```powershell
New-Item -ItemType Directory -Force -Path 'C:\ApolloBackup' | Out-Null; $stamp = Get-Date -Format 'yyyyMMdd-HHmm'; Compress-Archive -Path 'C:\Program Files\Apollo\*' -DestinationPath "C:\ApolloBackup\Apollo-install-$stamp.zip" -Force; Copy-Item -Recurse -Force 'C:\Program Files\Apollo\config' "C:\ApolloBackup\config-$stamp"; Get-ChildItem 'C:\ApolloBackup' | Select-Object Name, Length
```

```powershell
& 'C:\Program Files\Apollo\sunshine.exe' --version
```

```powershell
Get-Service ApolloService | Select-Object Status, StartType
```

Expected: a zip of about 100 MB and a `config-<stamp>` folder with `sunshine.conf`, `apps.json` and the credential files; the version string `0.0.0.0affdaa.dirty` (or the installed one); the service `Running`. Record the stamp.

- [ ] **Step 3: Download the installer on the host and check the hash**

Through the agentbus session (replace `<TAG>` with the tag of Step 1):

```powershell
Invoke-WebRequest -Uri 'https://github.com/catapultam/apollo-microphone/releases/download/<TAG>/Apollo.exe' -OutFile 'C:\ApolloBackup\Apollo-live-resize.exe'; (Get-FileHash 'C:\ApolloBackup\Apollo-live-resize.exe' -Algorithm SHA256).Hash.ToLower()
```

Expected: the hash equals the `sha256sum` of Step 1. Do not continue when it differs.

- [ ] **Step 4: Install (silent through agentbus; the user from a stream as the fallback)**

Primary path, through the agentbus session (needs elevation; UAC or the classifier can block it):

```powershell
Start-Process -FilePath 'C:\ApolloBackup\Apollo-live-resize.exe' -ArgumentList '/S' -Wait -Verb RunAs; Get-Service ApolloService | Select-Object Status
```

Fallback when the primary path is blocked: tell the user to start a stream (`moonlight-mic`, Desktop app), open `C:\ApolloBackup\Apollo-live-resize.exe` in the host Explorer and go through the installer pages. The stream ends when the installer stops the service; the installer continues on the host and starts the service again. The user reconnects after about one minute. The Finish page waits for a click; it is harmless and the user closes it at the next stream.

- [ ] **Step 5: Verify the install**

Through the agentbus session:

```powershell
& 'C:\Program Files\Apollo\sunshine.exe' --version; Get-Service ApolloService | Select-Object Status; Test-Path 'C:\Program Files\Apollo\config\sunshine.conf'; Select-String -Path 'C:\Program Files\Apollo\config\sunshine.conf' -Pattern '^(capture|headless_mode|stream_mic|server_cmd)'
```

Expected: a version string from the CI build (the branch `live-resize`, see `BRANCH` in `build-windows.yml`), service `Running`, `True`, and the four config lines unchanged (`capture = ddx`, `headless_mode = enabled`, `stream_mic = enabled`, `server_cmd`). Start a stream from the laptop; it must connect. The client journal shows no `Host does not support live resize` after the hotkey (Task 10 tests the rest).

- [ ] **Step 6: Rollback procedure (run only when Step 5 fails or the user asks)**

Through the agentbus session (replace `<stamp>`), or by the user from a stream when the host still streams:

```powershell
Stop-Service ApolloService; Expand-Archive -Path 'C:\ApolloBackup\Apollo-install-<stamp>.zip' -DestinationPath 'C:\Program Files\Apollo' -Force; Copy-Item -Recurse -Force 'C:\ApolloBackup\config-<stamp>\*' 'C:\Program Files\Apollo\config\'; Start-Service ApolloService; & 'C:\Program Files\Apollo\sunshine.exe' --version
```

Expected: the old version string and a running service. When the host does not stream after a failed install and agentbus is blocked too, the user needs physical or RDP access to CPLT-4A; say so before Step 4.

- [ ] **Step 7: Record the deploy in the homelab notes**

In `~/GitHub/homelab-notes` add the page `apollo-cplt-4a.md` (or extend the existing page that describes CPLT-4A, if `grep -ril cplt-4a *.md` finds one) with: the installed version and CI run id, the branch and fork, the install path and service name, the backup location and stamp, the install and rollback commands of Steps 4 and 6, the fact that the config directory survives the NSIS install, and the live resize hotkey. Link the page from `README.md`. No secrets (say that the web UI credentials are the ones the user holds).

```bash
cd ~/GitHub/homelab-notes
git add README.md apollo-cplt-4a.md
git commit -m "Record the Apollo live-resize deploy on CPLT-4A"
git push
```

---

### Task 10: Client build, submodule pointer, end-to-end tests

**Files:**
- Modify: `~/GitHub/moonlight-qt-mic` (submodule pointer already bumped in Tasks 1 and 2; final build and push)
- Modify: `~/GitHub/homelab-notes/apollo-cplt-4a.md` (test results)

**Interfaces:**
- Consumes: everything above; the host from Task 9.
- Produces: the verified feature; pushed `master` of `Catapultam-GMG/moonlight-qt-mic` and `Catapultam-GMG/moonlight-common-c-mic`.

- [ ] **Step 1: Clean build of the client**

Run: `toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && git submodule status && make -C app -f Makefile.Release clean && qmake6 moonlight-qt.pro && make release -j$(nproc) 2>&1 | tail -3 && ldd app/moonlight | grep placebo'`
Expected: the submodule is at the Task 2 commit; the build passes; `libplacebo.so` is listed.

Run the three unit tests again:

```bash
toolbox run -c moonlight bash -lc 'cd ~/GitHub/moonlight-qt-mic && g++ -std=c++17 -Wall -Wextra -Werror -I app app/tests/liveresize_test.cpp -o /tmp/liveresize_test && /tmp/liveresize_test && cd moonlight-common-c/moonlight-common-c && gcc -O0 -g -I src -I enet/include test/mic_crypto_test.c src/MicrophoneCrypto.c src/PlatformCrypto.c -lcrypto -o /tmp/mic_crypto_test && /tmp/mic_crypto_test'
```

Expected: both print `all checks passed`.

- [ ] **Step 2: Microphone check against the new host**

Repeat Task 1 Step 10 against the new host. Expected: `decryptErrors` stays 0 and `packetsDecoded` increases, and the user hears the microphone on the host. When errors still increase, compare the host line `Microphone decrypt diagnostics` (agentbus `Select-String`) with the client lines `MIC: first packet` and `Microphone diagnostics` (the second needs `MOONLIGHT_MIC_DEBUG=1 moonlight-mic`), as Task 1 Step 10 describes; report both lines to the user and stop the microphone work here.

- [ ] **Step 3: End-to-end live resize (spec 7.2)**

Setup: `moonlight-mic`, Desktop app, windowed mode, HDR off, mouse mode On (remote desktop). Keep `journalctl --user -f -o cat | grep -E 'resize|Resize|Video stream|Recreating'` open, and ask the agentbus session for `Get-Content -Wait 'C:\Program Files\Apollo\config\sunshine.log' | Select-String 'resize|Resize|Desktop resolution|Virtual display'` when possible.

1. Start the stream at the saved size (for example 2560x1600). Host log: `Virtual Display created`.
2. Resize the window to an odd size (for example 2537x1391). Press `Ctrl+Alt+Shift+R`. Expected within about 2 s: client `Requested live resize from 2560x1600 to 2536x1390 (request 1)`, host `Resize request 1 from [...] to 2536x1390`, `Live resize: display [...] is now 2536x1390`, `Live resize: encoder size set to 2536x1390`, `Resize request 1 done`; client `Stream size changed from 2560x1600 to 2536x1390`, `Video stream is now 2536x1390`, `Recreating renderer by internal request`. The picture fills the window without bars or blur.
3. Move the mouse to the four window corners; the host cursor reaches the display corners. Click a desktop icon in the bottom right corner.
4. Press the hotkey again without a window change. Expected: `Live resize: stream is already 2536x1390` and no host log line.
5. Press the hotkey twice fast after a window change. Expected: the second press shows "Resize in progress" (client) or the host refuses with BUSY; the first request completes.
6. Toggle full screen (`Ctrl+Alt+Shift+X`), press the hotkey. Expected: the native mode of the display (for example 3840x2160).
7. Stop the stream, start it again. Expected: the saved start size (spec D10).
8. Refusal: connect a second client (Artemis on a phone, or a second laptop), press the hotkey. Expected: "Another client is connected".
9. Size limit: shrink the window to about 300x150 logical pixels, press the hotkey. Expected: "Host rejected the size WxH" with the rounded size, and the next hotkey press after a window change works (the pending state cleared).
10. HDR on (PlVk renderer): repeat steps 1 to 3. Expected: the HDR state survives the recreate (`setHdrMode` in the reset path).
11. Microphone: speak during step 2. Expected: the host `/api/audio-debug` shows no gap longer than the resize and `decryptErrors` stays 0.
12. Host application: run a game in windowed mode on the host, resize, check that the window is still on the virtual display after the re-add (spec U1).
13. Disconnect during the change (spec R13): press the hotkey and quit the client with `Ctrl+Alt+Shift+Q` within 100 ms. Start a new session. Expected: the host log shows `Virtual Display created` at the saved start size and the stream works.
14. Software decoder and SDL renderer (spec R6): `moonlight-mic stream CPLT-4A Desktop --video-decoder software`, then repeat steps 2 and 3. Expected: the same log lines, no crash in `SdlRenderer` (the new-size frame is dropped before it reaches the renderer).

Record every deviation in the notes page. A failure in step 2 that shows `Live resize: display [...] is at WxH` means the S1 fallback with a new GUID ran; note it. A failure with `Host could not change the display` and no later stream means the revert failed too; use the Task 9 rollback and report.

- [ ] **Step 4: Push the client repositories**

```bash
cd ~/GitHub/moonlight-qt-mic/moonlight-common-c/moonlight-common-c && git status --short --branch
cd ~/GitHub/moonlight-qt-mic && git status --short --branch
git push origin master
```

Expected: the submodule `master` is already pushed (Tasks 1 and 2); `moonlight-qt-mic` `master` pushes cleanly. The two repositories point at each other correctly: `git submodule status` shows no `+` prefix.

- [ ] **Step 5: Record the results**

Append the end-to-end results (date, host version, client commit, which steps passed, known limits: exclusive full screen with two monitors can pick the wrong display (spec D9), DPI and window positions after the re-add (spec U1)) to `~/GitHub/homelab-notes/apollo-cplt-4a.md`, and update the memory note `~/.claude/projects/-var-home-catapultam/memory/moonlight-qt-mic-build.md` with one line about the hotkey and the mic fix.

```bash
cd ~/GitHub/homelab-notes
git add apollo-cplt-4a.md
git commit -m "Record the live resize end-to-end test on CPLT-4A"
git push
```

---

## Self-review notes

- Spec coverage: D1-D10 map to Tasks 2, 3, 4, 6, 7, 8; section 3 (wire protocol) to Tasks 2, 5, 8; section 4 (host) to Tasks 6, 7, 8; section 5 (client) to Tasks 3, 4; section 6 (build and deploy) to Tasks 8, 9, 10; section 7 (testing) to the unit tests in Tasks 1-5 and Task 10 Step 3; R1-R13 are either resolved by spikes or named in the task that owns them (R2 Task 7, R5 Task 7, R6 Task 4, R8 Task 3, R9 Task 2, R11 Task 6, R13 Task 10 step 13).
- Permission gate (`PERM::server_cmd`, spec 4.5): not added; the reviewer of Task 8 decides.
- Names used across tasks: `LiSendResizeRequest(uint16_t, uint16_t, uint32_t*)`, `LiIsLiveResizeSupported()`, `ConnListenerResizeRefused`, `LI_RESIZE_REFUSED_*` 1-7; `LiveResize::PendingRequest`, `roundDownEven`, `reasonText`, `classifyFrameSize`, `FrameSizeClass`; `Session::requestLiveResize/clearPendingResize/showResizeStatus/applyStreamSize`; `IVideoDecoder::setExpectedFrameSize(int, int)`; `SDL_CODE_STREAM_SIZE_CHANGED 106`, `SDL_CODE_RESIZE_REFUSED 107`, `SDL_CODE_RESIZE_TIMEOUT 108`, `SDL_CODE_RESIZE_STATUS_TIMEOUT 109`; host `live_resize::reason_e`, `request_payload_t`, `refused_payload_t`, `validate_size`, `change_display_size`, `HOST_TIMEOUT`; `proc_t::vdd`, `proc::vdd_lock`, `set_vdd_guid`, `set_vdd_size`; `mail::resize`, `mail::resize_refused`, `mail::resize_done`, `mail::encoder_failed`; `IDX_RESIZE_REQUEST` 13/14 (client), 19/20 (host); ids `0x3100`/`0x3101`.

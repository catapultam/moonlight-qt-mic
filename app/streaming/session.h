#pragma once

#include <QSemaphore>
#include <QQuickWindow>

#include <Limelight.h>
#include <opus_multistream.h>
#include "settings/streamingpreferences.h"
#include "input/input.h"
#include "video/decoder.h"
#include "audio/renderers/renderer.h"
#include "video/overlaymanager.h"
#include "liveresize.h"
#include "adaptivebitrate.h"
#include "bandwidth.h"

#include <atomic>
#include <mutex>
#include <vector>

class MicrophoneCapture;

class SupportedVideoFormatList : public QList<int>
{
public:
    operator int() const
    {
        int value = 0;

        for (const int v : *this) {
            value |= v;
        }

        return value;
    }

    void
    removeByMask(int mask)
    {
        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                this->removeAt(i);
            }
            else {
                i++;
            }
        }
    }

    void
    deprioritizeByMask(int mask)
    {
        QList<int> deprioritizedList;

        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                deprioritizedList.append(this->takeAt(i));
            }
            else {
                i++;
            }
        }

        this->append(std::move(deprioritizedList));
    }

    int maskByServerCodecModes(int serverCodecModes)
    {
        int mask = 0;

        const QMap<int, int> mapping = {
            {SCM_H264, VIDEO_FORMAT_H264},
            {SCM_H264_HIGH8_444, VIDEO_FORMAT_H264_HIGH8_444},
            {SCM_HEVC, VIDEO_FORMAT_H265},
            {SCM_HEVC_MAIN10, VIDEO_FORMAT_H265_MAIN10},
            {SCM_HEVC_REXT8_444, VIDEO_FORMAT_H265_REXT8_444},
            {SCM_HEVC_REXT10_444, VIDEO_FORMAT_H265_REXT10_444},
            {SCM_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN8},
            {SCM_AV1_MAIN10, VIDEO_FORMAT_AV1_MAIN10},
            {SCM_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH8_444},
            {SCM_AV1_HIGH10_444, VIDEO_FORMAT_AV1_HIGH10_444},
        };

        for (QMap<int, int>::const_iterator it = mapping.cbegin(); it != mapping.cend(); ++it) {
            if (serverCodecModes & it.key()) {
                mask |= it.value();
                serverCodecModes &= ~it.key();
            }
        }

        // Make sure nobody forgets to update this for new SCM values
        SDL_assert(serverCodecModes == 0);

        int val = *this;
        return val & mask;
    }
};

class Session : public QObject
{
    Q_OBJECT

    friend class SdlInputHandler;
    friend class DeferredSessionCleanupTask;
    friend class AsyncConnectionStartThread;

public:
    explicit Session(NvComputer* computer, NvApp& app, StreamingPreferences *preferences = nullptr);
    virtual ~Session();

    Q_INVOKABLE bool initialize(QQuickWindow* qtWindow);
    Q_INVOKABLE void start();
    Q_INVOKABLE void interrupt();
    Q_PROPERTY(QStringList launchWarnings MEMBER m_LaunchWarnings NOTIFY launchWarningsChanged);

    static
    void getDecoderInfo(SDL_Window* window,
                        bool& isHardwareAccelerated, bool& isFullScreenOnly,
                        bool& isHdrSupported, QSize& maxResolution);

    static Session* get()
    {
        return s_ActiveSession;
    }

    Overlay::OverlayManager& getOverlayManager()
    {
        return m_OverlayManager;
    }

    void flushWindowEvents();

    void setShouldExit(bool quitHostApp = false);

    // Asks the host to make the stream equal to the window size (Ctrl+Alt+Shift+W)
    void requestLiveResize();

    // Writes the bitrate line of the stats overlay (adaptive bitrate spec 6.5).
    // Any thread may call it. Returns the snprintf() result.
    int formatBitrateStats(char* output, int length);

    // Counts the bytes and the key frame of a received decode unit for the adaptive
    // bitrate. drSubmitDecodeUnit() calls it for push decoders, and the FFmpeg decoder
    // thread for the pull model (FFmpeg never uses the push model). Any thread may call it.
    void countDecodeUnit(const DECODE_UNIT* du);

signals:
    void stageStarting(QString stage);

    void stageFailed(QString stage, int errorCode, QString failingPorts);

    void connectionStarted();

    void displayLaunchError(QString text);

    void quitStarting();

    void sessionFinished(int portTestResult);

    // Emitted after sessionFinished() when the session is ready to be destroyed
    void readyForDeletion();

    void launchWarningsChanged();

private:
    void exec();

    bool startConnectionAsync();

    bool validateLaunch(SDL_Window* testWindow);

    void emitLaunchWarning(QString text);

    bool populateDecoderProperties(SDL_Window* window);

    IAudioRenderer* createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);

    bool initializeAudioRenderer();
    bool initializeMicrophoneCapture();

    bool testAudio(int audioConfiguration);

    int getAudioRendererCapabilities(int audioConfiguration);

    void destroyMicrophoneCapture();

    void getWindowDimensions(int& x, int& y,
                             int& width, int& height);

    void toggleFullscreen();

    void notifyMouseEmulationMode(bool enabled);

    void updateOptimalWindowDisplayMode();

    enum class DecoderAvailability {
        None,
        Software,
        Hardware
    };

    static
    DecoderAvailability getDecoderAvailability(SDL_Window* window,
                                               StreamingPreferences::VideoDecoderSelection vds,
                                               int videoFormat, int width, int height, int frameRate);

    static
    bool chooseDecoder(StreamingPreferences::VideoDecoderSelection vds,
                       StreamingPreferences::RendererSelection renderer,
                       SDL_Window* window, int videoFormat, int width, int height,
                       int frameRate, bool enableVsync, bool enableFramePacing,
                       bool testOnly,
                       IVideoDecoder*& chosenDecoder);

    static
    void clStageStarting(int stage);

    static
    void clStageFailed(int stage, int errorCode);

    static
    void clConnectionTerminated(int errorCode);

    static
    void clLogMessage(const char* format, ...);

    static
    void clRumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

    static
    void clConnectionStatusUpdate(int connectionStatus);

    static
    void clSetHdrMode(bool enabled);

    static
    void clRumbleTriggers(uint16_t controllerNumber, uint16_t leftTrigger, uint16_t rightTrigger);

    static
    void clSetMotionEventState(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

    static
    void clSetControllerLED(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

    static
    void clSetAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

    static
    void clResizeRefused(uint16_t width, uint16_t height, uint32_t requestId, uint16_t reason);

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

    static
    Uint32 resizeTimeoutTimerCallback(Uint32 interval, void* param);

    static
    Uint32 statusOverlayTimerCallback(Uint32 interval, void* param);

    static
    Uint32 resizePumpTimerCallback(Uint32 interval, void* param);

    // Shows a live resize message in the status overlay for a few seconds
    void showResizeStatus(const char* text);

    // Stops the timeout of the request in flight and removes the expected
    // size from the decoder. The caller updates m_ResizeState.
    void clearPendingResize();

    // Writes the target size for a live resize (spec D9, D4). Returns false
    // when the size cannot be read. For a manual request it shows the reason.
    bool getLiveResizeTarget(int& width, int& height, bool manual);

    // Sets the window size as the automatic target when the setting is on and
    // the logical window size or the full-screen mode changed (spec 5.2)
    void triggerAutoLiveResize();

    // Sends the next request when the rules allow it, else starts the timer
    // for the next check
    void pumpLiveResize();

    // Takes the new stream size from the decoder and recreates the decoder
    void applyStreamSize(int width, int height);

    static
    int arInit(int audioConfiguration,
               const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
               void* arContext, int arFlags);

    static
    void arCleanup();

    static
    void arDecodeAndPlaySample(char* sampleData, int sampleLength);

    static
    int drSetup(int videoFormat, int width, int height, int frameRate, void*, int);

    static
    void drCleanup();

    static
    int drSubmitDecodeUnit(PDECODE_UNIT du);

    StreamingPreferences* m_Preferences;
    bool m_IsFullScreen;
    SupportedVideoFormatList m_SupportedVideoFormats; // Sorted in order of descending priority
    STREAM_CONFIGURATION m_StreamConfig;
    DECODER_RENDERER_CALLBACKS m_VideoCallbacks;
    AUDIO_RENDERER_CALLBACKS m_AudioCallbacks;
    NvComputer* m_Computer;
    NvApp m_App;
    SDL_Window* m_Window;
    IVideoDecoder* m_VideoDecoder;
    SDL_mutex* m_DecoderLock;
    bool m_AudioDisabled;
    bool m_AudioMuted;
    Uint32 m_FullScreenFlag;
    QQuickWindow* m_QtWindow;
    bool m_UnexpectedTermination;
    SdlInputHandler* m_InputHandler;
    int m_MouseEmulationRefCount;
    int m_FlushingWindowEventsRef;
    QStringList m_LaunchWarnings;
    bool m_ShouldExit;

    bool m_AsyncConnectionSuccess;
    int m_PortTestResults;

    int m_ActiveVideoFormat;
    int m_ActiveVideoWidth;
    int m_ActiveVideoHeight;
    int m_ActiveVideoFrameRate;

    OpusMSDecoder* m_OpusDecoder;
    IAudioRenderer* m_AudioRenderer;
    OPUS_MULTISTREAM_CONFIGURATION m_ActiveAudioConfig;
    OPUS_MULTISTREAM_CONFIGURATION m_OriginalAudioConfig;
    int m_AudioSampleCount;
    Uint32 m_DropAudioEndTime;
    MicrophoneCapture* m_MicrophoneCapture;
    bool m_MicrophoneEnabled;

    Overlay::OverlayManager m_OverlayManager;

    // Who put the current text in the status overlay. The connection warning
    // and the gamepad mouse mode are not replaced by resize messages.
    enum class StatusOverlayOwner {
        None,
        Connection,
        MouseEmulation,
        Resize,
    };
    StatusOverlayOwner m_StatusOverlayOwner;
    LiveResize::ResizeController m_ResizeState;
    // The window state at the last automatic target
    LiveResize::WindowTrigger m_AutoResizeWindow;
    SDL_TimerID m_ResizeTimeoutTimer;
    // One-shot timer of the debounce and the BUSY retry
    SDL_TimerID m_ResizePumpTimer;
    // Counts the pump timers; a timer event from an older timer is ignored
    uint32_t m_ResizePumpGeneration;
    // True after the first decoded frame, when the automatic triggers work
    bool m_AutoResizeArmed;
    // The automatic mode logs a host without live resize once
    bool m_AutoResizeUnsupportedLogged;
    // True from a stream size change until the new decoder decodes a frame.
    // No request is sent then, so that the new decoder knows its last frame
    // before the request (spec 5.4).
    bool m_ResizeWaitingForFrame;
    SDL_TimerID m_StatusOverlayTimer;
    // Counts showResizeStatus() calls; a timer event from an older call is ignored
    uint32_t m_StatusOverlayGeneration;

    // Adaptive bitrate (spec section 6). The controller and the timer belong to the main thread.
    AdaptiveBitrate::Controller m_BitrateController;
    SDL_TimerID m_BitrateTickTimer;
    bool m_BitrateUnsupportedLogged;
    // Received video bytes of the session (all decoders, see countDecodeUnit())
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
    // Rate limit of the send failure warning
    uint32_t m_BitrateSendFailKbps;
    uint64_t m_BitrateSendFailLogMs;
    // True after the first request that was sent (overlay with adaptation off)
    bool m_BitrateRequestSent;
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

    static CONNECTION_LISTENER_CALLBACKS k_ConnCallbacks;
    static Session* s_ActiveSession;
    static QSemaphore s_ActiveSessionSemaphore;
};

#include "PictureWindow.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
#include <evr.h>
#include <mfapi.h>
#include <mfplay.h>
#include <propvarutil.h>
#include <windows.h>
#endif

namespace triggerfish {

namespace {

void showPictureError(const juce::String& message) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon,
        "Picture Playback Error",
        message);
}

#if JUCE_WINDOWS
constexpr double kHundredNanosPerSecond = 10000000.0;

double positionFromPropVariant(const PROPVARIANT& value) {
    switch (value.vt) {
        case VT_I8:
            return static_cast<double>(value.hVal.QuadPart) / kHundredNanosPerSecond;
        case VT_UI8:
            return static_cast<double>(value.uhVal.QuadPart) / kHundredNanosPerSecond;
        default:
            return 0.0;
    }
}

PROPVARIANT propVariantFromSeconds(double seconds) {
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = static_cast<LONGLONG>(
        std::llround(juce::jmax(0.0, seconds) * kHundredNanosPerSecond));
    return value;
}

#endif
}  // namespace

#if JUCE_WINDOWS
class PictureWindowContent::MediaFoundationCallback : public IMFPMediaPlayerCallback {
public:
    explicit MediaFoundationCallback(PictureWindowContent& owner) : owner_(owner) {}

#if JUCE_WINDOWS
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }

        *ppvObject = nullptr;
        if (riid == IID_IUnknown || riid == IID_IMFPMediaPlayerCallback) {
            *ppvObject = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(++refCount_);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const auto value = --refCount_;
        if (value == 0) {
            delete this;
        }
        return static_cast<ULONG>(value);
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override {
        if (eventHeader == nullptr) {
            return;
        }

        auto safeOwner = juce::Component::SafePointer<PictureWindowContent>(&owner_);
        const auto eventType = eventHeader->eEventType;
        const auto eventResult = eventHeader->hrEvent;

        juce::MessageManager::callAsync([safeOwner, eventType, eventResult] {
            if (safeOwner == nullptr) {
                return;
            }

            auto& owner = *safeOwner;
            switch (eventType) {
                case MFP_EVENT_TYPE_PLAYBACK_ENDED:
                    owner.isPlaying_ = false;
                    owner.positionSeconds_ = owner.durationSeconds_;
                    owner.refreshControls();
                    break;

                case MFP_EVENT_TYPE_MEDIAITEM_SET:
                    owner.hasLoadedVideo_ = owner.currentVideoFile_.existsAsFile();
                    owner.positionSeconds_ = 0.0;
                    owner.primeFirstFramePending_ = true;
                    if (owner.player_ != nullptr) {
                        static_cast<IMFPMediaPlayer*>(owner.player_)->Play();
                    }
                    owner.refreshControls();
                    break;

                case MFP_EVENT_TYPE_ERROR:
                    owner.isPlaying_ = false;
                    owner.refreshControls();
                    showPictureError("Media Foundation could not render this picture file.");
                    break;

                default:
                    if (FAILED(eventResult)) {
                        owner.isPlaying_ = false;
                        owner.refreshControls();
                    }
                    break;
            }
        });
    }
#endif

private:
    std::atomic<long> refCount_{1};
    PictureWindowContent& owner_;
};
#else
class PictureWindowContent::MediaFoundationCallback {};
#endif

PictureWindowContent::PictureWindowContent() {
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    addMouseListener(this, true);
#if JUCE_WINDOWS
    addAndMakeVisible(videoHost_);
#else
    addAndMakeVisible(videoComponent_);
#endif

    fileLabel_.setColour(juce::Label::textColourId, colours::textDim);
    fileLabel_.setFont(juce::FontOptions(11.0f));
    fileLabel_.setText("No picture loaded", juce::dontSendNotification);
    addAndMakeVisible(fileLabel_);

    timeLabel_.setColour(juce::Label::textColourId, colours::textDim);
    timeLabel_.setFont(juce::FontOptions(11.0f));
    timeLabel_.setJustificationType(juce::Justification::centredRight);
    timeLabel_.setText("00:00.00 / 00:00.00", juce::dontSendNotification);
    addAndMakeVisible(timeLabel_);

    loadButton_.onClick = [this] {
        if (onInteraction) {
            onInteraction();
        }
        if (onLoadRequested) {
            onLoadRequested();
        }
    };
    addAndMakeVisible(loadButton_);

    clearButton_.onClick = [this] {
        if (onInteraction) {
            onInteraction();
        }
        clearVideo();
    };
    addAndMakeVisible(clearButton_);

    playPauseButton_.onClick = [this] {
        if (onInteraction) {
            onInteraction();
        }
        if (!hasLoadedVideo_) {
            return;
        }

        if (isPlaying_) {
            pause();
        } else {
            play();
        }
    };
    addAndMakeVisible(playPauseButton_);

    stopButton_.onClick = [this] {
        if (onInteraction) {
            onInteraction();
        }
        stop(true);
    };
    addAndMakeVisible(stopButton_);

    timelineSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    timelineSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    timelineSlider_.setRange(0.0, 1.0, 0.0);
    timelineSlider_.onValueChange = [this] {
        if (suppressTimelineCallback_ || !hasLoadedVideo_) {
            return;
        }

        if (onInteraction) {
            onInteraction();
        }
        seek(timelineSlider_.getValue());
    };
    addAndMakeVisible(timelineSlider_);

    volumeLabel_.setColour(juce::Label::textColourId, colours::textDim);
    volumeLabel_.setFont(juce::FontOptions(11.0f));
    volumeLabel_.setText("Volume", juce::dontSendNotification);
    addAndMakeVisible(volumeLabel_);

    volumeSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    volumeSlider_.setRange(0.0, 1.0, 0.01);
    volumeSlider_.setValue(volume_, juce::dontSendNotification);
    volumeSlider_.setCtrlClickResetValue(1.0);
    volumeSlider_.textFromValueFunction = [](double value) {
        return juce::String(juce::roundToInt(value * 100.0)) + "%";
    };
    volumeSlider_.onValueChange = [this] {
        if (onInteraction) {
            onInteraction();
        }
        volume_ = volumeSlider_.getValue();
        applyPlayerVolume();
    };
    addAndMakeVisible(volumeSlider_);

    refreshControls();
    startTimerHz(15);
}

PictureWindowContent::~PictureWindowContent() {
    stopTimer();
    destroyPlayer();
    destroyVideoWindow();

#if JUCE_WINDOWS
    if (mediaFoundationReady_) {
        MFShutdown();
        mediaFoundationReady_ = false;
    }
#endif
}

void PictureWindowContent::paint(juce::Graphics& g) {
    g.fillAll(colours::background);
}

void PictureWindowContent::mouseDown(const juce::MouseEvent&) {
    grabKeyboardFocus();
    if (onInteraction) {
        onInteraction();
    }
}

bool PictureWindowContent::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::spaceKey && hasLoadedVideo_) {
        grabKeyboardFocus();
        if (onInteraction) {
            onInteraction();
        }
        if (isPlaying_) {
            stop(false);
        } else {
            play();
        }
        return true;
    }
    return false;
}

void PictureWindowContent::resized() {
    auto area = getLocalBounds().reduced(8);
    auto topRow = area.removeFromTop(26);

    stopButton_.setBounds(topRow.removeFromRight(56));
    topRow.removeFromRight(4);
    playPauseButton_.setBounds(topRow.removeFromRight(64));
    topRow.removeFromRight(4);
    clearButton_.setBounds(topRow.removeFromRight(56));
    topRow.removeFromRight(4);
    loadButton_.setBounds(topRow.removeFromRight(56));
    topRow.removeFromRight(8);
    timeLabel_.setBounds(topRow.removeFromRight(128));
    fileLabel_.setBounds(topRow);

    area.removeFromTop(6);
    timelineSlider_.setBounds(area.removeFromTop(18));

    area.removeFromTop(6);
    auto volumeRow = area.removeFromTop(24);
    volumeLabel_.setBounds(volumeRow.removeFromLeft(52));
    volumeRow.removeFromLeft(8);
    volumeSlider_.setBounds(volumeRow);

    area.removeFromTop(6);
 #if JUCE_WINDOWS
    videoHost_.setBounds(area);

    if (player_ != nullptr) {
        videoHost_.updateHWNDBounds();
        static_cast<IMFPMediaPlayer*>(player_)->UpdateVideo();
    }
#else
    videoComponent_.setBounds(area);
#endif
}

void PictureWindowContent::loadVideo(const juce::File& file) {
    if (!file.existsAsFile()) {
        return;
    }

#if JUCE_WINDOWS
    destroyPlayer();

    if (!ensurePlayerCreated()) {
        return;
    }

    currentVideoFile_ = file;
    hasLoadedVideo_ = true;
    isPlaying_ = false;
    primeFirstFramePending_ = false;
    positionSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    fileLabel_.setText(file.getFileName(), juce::dontSendNotification);

    IMFPMediaPlayer* newPlayer = nullptr;
    const HRESULT result = MFPCreateMediaPlayer(
        file.getFullPathName().toWideCharPointer(),
        FALSE,
        MFP_OPTION_NONE,
        playerCallback_.get(),
        static_cast<HWND>(videoWindow_),
        &newPlayer);

    if (FAILED(result) || newPlayer == nullptr) {
        currentVideoFile_ = juce::File();
        hasLoadedVideo_ = false;
        fileLabel_.setText("No picture loaded", juce::dontSendNotification);
        showPictureError("Media Foundation could not open that picture file.");
        refreshControls();
        return;
    }

    player_ = newPlayer;
    newPlayer->SetAspectRatioMode(MFVideoARMode_PreservePicture);
    applyPlayerVolume();
    newPlayer->UpdateVideo();
    refreshControls();
#else
    currentVideoFile_ = file;
    hasLoadedVideo_ = false;
    isPlaying_ = false;
    positionSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    fileLabel_.setText(file.getFileName(), juce::dontSendNotification);

    const auto result = videoComponent_.load(file);
    if (result.failed()) {
        currentVideoFile_ = juce::File();
        fileLabel_.setText("No picture loaded", juce::dontSendNotification);
        showPictureError(result.getErrorMessage().isNotEmpty()
                             ? result.getErrorMessage()
                             : "The picture file could not be opened.");
        refreshControls();
        return;
    }

    hasLoadedVideo_ = true;
    durationSeconds_ = videoComponent_.getVideoDuration();
    videoComponent_.setPlayPosition(0.0);
    videoComponent_.stop();
    videoComponent_.setAudioVolume(static_cast<float>(volume_));
    refreshControls();
#endif
}

void PictureWindowContent::clearVideo() {
#if JUCE_WINDOWS
    destroyPlayer();
    destroyVideoWindow();
    ensureVideoWindowCreated();
    resized();
#else
    videoComponent_.closeVideo();
#endif
    currentVideoFile_ = juce::File();
    hasLoadedVideo_ = false;
    isPlaying_ = false;
    primeFirstFramePending_ = false;
    positionSeconds_ = 0.0;
    durationSeconds_ = 0.0;
    fileLabel_.setText("No picture loaded", juce::dontSendNotification);
    refreshControls();
    if (onVideoCleared) {
        onVideoCleared();
    }
}

void PictureWindowContent::play() {
#if JUCE_WINDOWS
    if (!hasLoadedVideo_ || player_ == nullptr) {
        return;
    }

    static_cast<IMFPMediaPlayer*>(player_)->Play();
#else
    if (!hasLoadedVideo_) {
        return;
    }

    videoComponent_.play();
#endif
}

void PictureWindowContent::pause() {
#if JUCE_WINDOWS
    if (!hasLoadedVideo_ || player_ == nullptr) {
        return;
    }

    static_cast<IMFPMediaPlayer*>(player_)->Pause();
#else
    if (!hasLoadedVideo_) {
        return;
    }

    videoComponent_.stop();
#endif
}

void PictureWindowContent::stop(bool rewindToStart) {
#if JUCE_WINDOWS
    if (!hasLoadedVideo_ || player_ == nullptr) {
        return;
    }

    auto* player = static_cast<IMFPMediaPlayer*>(player_);
    player->Pause();
    isPlaying_ = false;

    if (rewindToStart) {
        seek(0.0);
    } else {
        player->UpdateVideo();
    }
#else
    if (!hasLoadedVideo_) {
        return;
    }

    videoComponent_.stop();
    isPlaying_ = false;
    if (rewindToStart) {
        videoComponent_.setPlayPosition(0.0);
        positionSeconds_ = 0.0;
    } else {
        positionSeconds_ = videoComponent_.getPlayPosition();
    }
#endif
}

void PictureWindowContent::seek(double seconds) {
#if JUCE_WINDOWS
    if (!hasLoadedVideo_ || player_ == nullptr) {
        return;
    }

    auto* player = static_cast<IMFPMediaPlayer*>(player_);
    auto position = propVariantFromSeconds(seconds);
    player->SetPosition(MFP_POSITIONTYPE_100NS, &position);
    PropVariantClear(&position);
    positionSeconds_ = juce::jmax(0.0, seconds);

    if (!isPlaying_) {
        player->Pause();
    }
    player->UpdateVideo();
#else
    if (!hasLoadedVideo_) {
        return;
    }

    videoComponent_.setPlayPosition(seconds);
    positionSeconds_ = juce::jmax(0.0, seconds);
    if (!isPlaying_) {
        videoComponent_.stop();
    }
#endif
}

bool PictureWindowContent::hasLoadedVideo() const {
    return hasLoadedVideo_;
}

bool PictureWindowContent::isPlaying() const {
    return isPlaying_;
}

double PictureWindowContent::currentPositionSeconds() const {
    return positionSeconds_;
}

double PictureWindowContent::durationSeconds() const {
    return durationSeconds_;
}

double PictureWindowContent::volume() const {
    return volume_;
}

void PictureWindowContent::setVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    volumeSlider_.setValue(volume_, juce::dontSendNotification);
    applyPlayerVolume();
}

void PictureWindowContent::timerCallback() {
#if JUCE_WINDOWS
    if (player_ != nullptr) {
        auto* player = static_cast<IMFPMediaPlayer*>(player_);

        MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
        if (SUCCEEDED(player->GetState(&state))) {
            isPlaying_ = (state == MFP_MEDIAPLAYER_STATE_PLAYING);
        }

        PROPVARIANT position;
        PropVariantInit(&position);
        if (SUCCEEDED(player->GetPosition(MFP_POSITIONTYPE_100NS, &position))) {
            positionSeconds_ = positionFromPropVariant(position);
        }
        PropVariantClear(&position);

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(player->GetDuration(MFP_POSITIONTYPE_100NS, &duration))) {
            durationSeconds_ = positionFromPropVariant(duration);
        }
        PropVariantClear(&duration);

        if (primeFirstFramePending_ && durationSeconds_ > 0.0) {
            player->Pause();
            isPlaying_ = false;
            seek(0.0);
            primeFirstFramePending_ = false;
        }

        player->UpdateVideo();
    }
#endif

#if !JUCE_WINDOWS
    if (hasLoadedVideo_) {
        isPlaying_ = videoComponent_.isPlaying();
        positionSeconds_ = videoComponent_.getPlayPosition();
        durationSeconds_ = videoComponent_.getVideoDuration();
    }
#endif

    refreshControls();
}

bool PictureWindowContent::ensurePlayerCreated() {
#if JUCE_WINDOWS
    if (!mediaFoundationReady_) {
        const HRESULT startupResult = MFStartup(MF_VERSION);
        if (FAILED(startupResult)) {
            showPictureError("Media Foundation could not be started on this system.");
            return false;
        }

        mediaFoundationReady_ = true;
    }

    if (!ensureVideoWindowCreated()) {
        return false;
    }

    if (!playerCallback_) {
        playerCallback_ = std::make_unique<MediaFoundationCallback>(*this);
    }

    return true;
#else
    return true;
#endif
}

bool PictureWindowContent::ensureVideoWindowCreated() {
#if JUCE_WINDOWS
    if (videoWindow_ != nullptr) {
        return true;
    }

    auto hwnd = CreateWindowExW(
        0,
        L"Static",
        L"",
        WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        0,
        16,
        16,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (hwnd == nullptr) {
        showPictureError("Could not create the native picture window.");
        return false;
    }

    videoWindow_ = hwnd;
    videoHost_.setHWND(hwnd);
    return true;
#else
    return false;
#endif
}

void PictureWindowContent::destroyPlayer() {
#if JUCE_WINDOWS
    if (player_ != nullptr) {
        auto* player = static_cast<IMFPMediaPlayer*>(player_);
        player->Shutdown();
        player->Release();
        player_ = nullptr;
    }
#endif
}

void PictureWindowContent::destroyVideoWindow() {
#if JUCE_WINDOWS
    if (videoWindow_ != nullptr) {
        videoHost_.setHWND(nullptr);
        DestroyWindow(static_cast<HWND>(videoWindow_));
        videoWindow_ = nullptr;
    }
#endif
}

void PictureWindowContent::applyPlayerVolume() {
#if JUCE_WINDOWS
    if (player_ != nullptr) {
        static_cast<IMFPMediaPlayer*>(player_)->SetVolume(static_cast<float>(volume_));
    }
#else
    videoComponent_.setAudioVolume(static_cast<float>(volume_));
#endif
}

void PictureWindowContent::refreshControls() {
    clearButton_.setEnabled(hasLoadedVideo_);
    playPauseButton_.setEnabled(hasLoadedVideo_);
    stopButton_.setEnabled(hasLoadedVideo_);
    timelineSlider_.setEnabled(hasLoadedVideo_ && durationSeconds_ > 0.0);
    volumeSlider_.setEnabled(true);
    playPauseButton_.setButtonText(isPlaying_ ? "Pause" : "Play");
    timeLabel_.setText(formatTimecode(positionSeconds_) + " / " + formatTimecode(durationSeconds_),
                       juce::dontSendNotification);

    suppressTimelineCallback_ = true;
    timelineSlider_.setRange(0.0, durationSeconds_ > 0.0 ? durationSeconds_ : 1.0, 0.0);
    if (!timelineSlider_.isMouseButtonDown()) {
        timelineSlider_.setValue(positionSeconds_, juce::dontSendNotification);
    }
    suppressTimelineCallback_ = false;
}

juce::String PictureWindowContent::formatTimecode(double seconds) const {
    const auto clampedSeconds = juce::jmax(0.0, seconds);
    const auto totalMinutes = static_cast<int>(clampedSeconds / 60.0);
    const auto wholeSeconds = static_cast<int>(clampedSeconds) % 60;
    const auto hundredths = static_cast<int>(std::round((clampedSeconds - std::floor(clampedSeconds)) * 100.0));

    juce::String formatted;
    formatted << juce::String(totalMinutes).paddedLeft('0', 2)
              << ":" << juce::String(wholeSeconds).paddedLeft('0', 2)
              << "." << juce::String(juce::jmin(hundredths, 99)).paddedLeft('0', 2);
    return formatted;
}

PictureWindow::PictureWindow()
    : juce::DocumentWindow("Triggerfish Picture",
                           colours::background,
                           juce::DocumentWindow::closeButton) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setResizeLimits(480, 320, 1920, 1400);
    setContentOwned(&content_, false);
    centreWithSize(960, 620);
}

PictureWindow::~PictureWindow() = default;

PictureWindowContent& PictureWindow::content() {
    return content_;
}

const PictureWindowContent& PictureWindow::content() const {
    return content_;
}

void PictureWindow::closeButtonPressed() {
    setVisible(false);
}

}  // namespace triggerfish

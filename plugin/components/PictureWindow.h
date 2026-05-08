#pragma once

#include "../LookAndFeel_Radium.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_video/juce_video.h>
#include <functional>
#include <memory>

namespace triggerfish {

class PictureWindowContent : public juce::Component,
                             private juce::Timer {
public:
    PictureWindowContent();
    ~PictureWindowContent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

    std::function<void()> onLoadRequested;
    std::function<void()> onVideoCleared;
    std::function<void()> onInteraction;

    void loadVideo(const juce::File& file);
    void clearVideo();
    void play();
    void pause();
    void stop(bool rewindToStart);
    void seek(double seconds);

    bool hasLoadedVideo() const;
    bool isPlaying() const;
    double currentPositionSeconds() const;
    double durationSeconds() const;
    double volume() const;
    void setVolume(double volume);

private:
    void timerCallback() override;
    bool ensurePlayerCreated();
    bool ensureVideoWindowCreated();
    void destroyPlayer();
    void destroyVideoWindow();
    void applyPlayerVolume();
    void refreshControls();
    juce::String formatTimecode(double seconds) const;

    class MediaFoundationCallback;

    juce::Label fileLabel_;
    juce::Label timeLabel_;
    juce::TextButton loadButton_{"Load"};
    juce::TextButton clearButton_{"Clear"};
    juce::TextButton playPauseButton_{"Play"};
    juce::TextButton stopButton_{"Stop"};
    juce::Slider timelineSlider_;
    juce::Label volumeLabel_;
    triggerfish::FineTuneSlider volumeSlider_;
#if JUCE_WINDOWS
    juce::HWNDComponent videoHost_;
#else
    juce::VideoComponent videoComponent_;
#endif
    juce::File currentVideoFile_;
    bool hasLoadedVideo_ = false;
    bool isPlaying_ = false;
    bool suppressTimelineCallback_ = false;
    bool mediaFoundationReady_ = false;
    bool primeFirstFramePending_ = false;
    double positionSeconds_ = 0.0;
    double durationSeconds_ = 0.0;
    double volume_ = 1.0;
    void* player_ = nullptr;
    void* videoWindow_ = nullptr;
    std::unique_ptr<MediaFoundationCallback> playerCallback_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PictureWindowContent)
};

class PictureWindow : public juce::DocumentWindow {
public:
    PictureWindow();
    ~PictureWindow() override;

    PictureWindowContent& content();
    const PictureWindowContent& content() const;

    void closeButtonPressed() override;

private:
    PictureWindowContent content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PictureWindow)
};

}  // namespace triggerfish

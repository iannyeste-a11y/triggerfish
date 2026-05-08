#pragma once

#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>
#include <functional>

namespace triggerfish {

class VideoPlaybackComponent : public juce::Component {
public:
    VideoPlaybackComponent();

    void paint(juce::Graphics&) override;
    void resized() override;

    std::function<void()> onLoadVideo;
    std::function<void()> onClearVideo;
    std::function<void()> onPlayPause;
    std::function<void()> onStopVideo;
    std::function<void(double seconds)> onSeek;

    juce::VideoComponent& videoComponent();
    const juce::VideoComponent& videoComponent() const;

    void clearLoadedVideo();
    void setLoadedVideo(const juce::File& file, double durationSeconds);
    void setPlaybackState(bool loaded, bool playing, double positionSeconds, double durationSeconds);
    bool isScrubbingTimeline() const;

private:
    juce::String formatTimecode(double seconds) const;

    juce::Label titleLabel_{"", "PICTURE"};
    juce::Label fileLabel_;
    juce::Label timeLabel_;
    juce::TextButton loadButton_{"Load"};
    juce::TextButton clearButton_{"Clear"};
    juce::TextButton playPauseButton_{"Play"};
    juce::TextButton stopButton_{"Stop"};
    juce::Slider timelineSlider_;
    juce::VideoComponent videoComponent_{false};

    bool hasLoadedVideo_ = false;
    bool suppressTimelineCallback_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoPlaybackComponent)
};

}  // namespace triggerfish

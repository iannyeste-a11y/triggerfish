#include "VideoPlaybackComponent.h"
#include <algorithm>

namespace triggerfish {

VideoPlaybackComponent::VideoPlaybackComponent() {
    titleLabel_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, colours::textPrimary);
    addAndMakeVisible(titleLabel_);

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
        if (onLoadVideo) onLoadVideo();
    };
    addAndMakeVisible(loadButton_);

    clearButton_.onClick = [this] {
        if (onClearVideo) onClearVideo();
    };
    clearButton_.setEnabled(false);
    addAndMakeVisible(clearButton_);

    playPauseButton_.onClick = [this] {
        if (onPlayPause) onPlayPause();
    };
    playPauseButton_.setEnabled(false);
    addAndMakeVisible(playPauseButton_);

    stopButton_.onClick = [this] {
        if (onStopVideo) onStopVideo();
    };
    stopButton_.setEnabled(false);
    addAndMakeVisible(stopButton_);

    timelineSlider_.setSliderStyle(juce::Slider::LinearBar);
    timelineSlider_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    timelineSlider_.setRange(0.0, 1.0, 0.0);
    timelineSlider_.onValueChange = [this] {
        if (suppressTimelineCallback_ || !onSeek) return;
        onSeek(timelineSlider_.getValue());
    };
    timelineSlider_.setEnabled(false);
    addAndMakeVisible(timelineSlider_);

    addAndMakeVisible(videoComponent_);
}

void VideoPlaybackComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::panel);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(colours::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    auto videoBounds = videoComponent_.getBounds();
    g.setColour(colours::waveBg);
    g.fillRect(videoBounds);
    g.setColour(colours::border);
    g.drawRect(videoBounds, 1);

    if (!hasLoadedVideo_) {
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Load an MP4, MOV, M4V, or QT file",
                   videoBounds.reduced(12),
                   juce::Justification::centred,
                   true);
    }
}

void VideoPlaybackComponent::resized() {
    auto area = getLocalBounds().reduced(6);

    auto headerRow = area.removeFromTop(24);
    titleLabel_.setBounds(headerRow.removeFromLeft(60));
    stopButton_.setBounds(headerRow.removeFromRight(48));
    headerRow.removeFromRight(4);
    playPauseButton_.setBounds(headerRow.removeFromRight(64));
    headerRow.removeFromRight(4);
    clearButton_.setBounds(headerRow.removeFromRight(52));
    headerRow.removeFromRight(4);
    loadButton_.setBounds(headerRow.removeFromRight(52));

    auto infoRow = area.removeFromTop(18);
    timeLabel_.setBounds(infoRow.removeFromRight(112));
    fileLabel_.setBounds(infoRow);

    area.removeFromBottom(4);
    auto timelineArea = area.removeFromBottom(24);
    timelineSlider_.setBounds(timelineArea);

    area.removeFromBottom(4);
    videoComponent_.setBounds(area);
}

juce::VideoComponent& VideoPlaybackComponent::videoComponent() {
    return videoComponent_;
}

const juce::VideoComponent& VideoPlaybackComponent::videoComponent() const {
    return videoComponent_;
}

void VideoPlaybackComponent::clearLoadedVideo() {
    hasLoadedVideo_ = false;
    fileLabel_.setText("No picture loaded", juce::dontSendNotification);
    setPlaybackState(false, false, 0.0, 0.0);
}

void VideoPlaybackComponent::setLoadedVideo(const juce::File& file, double durationSeconds) {
    hasLoadedVideo_ = true;
    fileLabel_.setText(file.getFileName(), juce::dontSendNotification);
    setPlaybackState(true, false, 0.0, durationSeconds);
}

void VideoPlaybackComponent::setPlaybackState(bool loaded,
                                              bool playing,
                                              double positionSeconds,
                                              double durationSeconds) {
    hasLoadedVideo_ = loaded;
    clearButton_.setEnabled(loaded);
    playPauseButton_.setEnabled(loaded);
    stopButton_.setEnabled(loaded);
    timelineSlider_.setEnabled(loaded);
    playPauseButton_.setButtonText(playing ? "Pause" : "Play");

    const double safeDuration = std::max(0.0, durationSeconds);
    const double safePosition = std::clamp(positionSeconds, 0.0, safeDuration > 0.0 ? safeDuration : 0.0);

    suppressTimelineCallback_ = true;
    timelineSlider_.setRange(0.0, safeDuration > 0.0 ? safeDuration : 1.0, 0.0);
    if (!isScrubbingTimeline()) {
        timelineSlider_.setValue(safePosition, juce::dontSendNotification);
    }
    suppressTimelineCallback_ = false;

    timeLabel_.setText(formatTimecode(safePosition) + " / " + formatTimecode(safeDuration),
                       juce::dontSendNotification);
    repaint();
}

bool VideoPlaybackComponent::isScrubbingTimeline() const {
    return timelineSlider_.isMouseButtonDown();
}

juce::String VideoPlaybackComponent::formatTimecode(double seconds) const {
    const auto clampedSeconds = std::max(0.0, seconds);
    const auto totalMinutes = static_cast<int>(clampedSeconds / 60.0);
    const auto wholeSeconds = static_cast<int>(clampedSeconds) % 60;
    const auto hundredths = static_cast<int>(std::round((clampedSeconds - std::floor(clampedSeconds)) * 100.0));

    juce::String formatted;
    formatted << juce::String(totalMinutes).paddedLeft('0', 2)
              << ":" << juce::String(wholeSeconds).paddedLeft('0', 2)
              << "." << juce::String(std::min(hundredths, 99)).paddedLeft('0', 2);
    return formatted;
}

}  // namespace triggerfish

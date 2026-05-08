#include "StereoLevelMeterComponent.h"
#include "../LookAndFeel_Radium.h"

#include <algorithm>

namespace triggerfish {

namespace {

constexpr float kGreenEnd = 0.25118864f;   // -12 dBFS
constexpr float kYellowEnd = 0.70794576f;  // -3 dBFS
constexpr int kPeakHoldFrames = 18;
constexpr float kReleasePerUpdate = 0.035f;
constexpr float kPeakReleasePerUpdate = 0.015f;

juce::String meterLabelForIndex(int index, int channelCount) {
    if (channelCount == 8) {
        switch (index) {
            case 0: return "L";
            case 1: return "R";
            case 2: return "C";
            case 3: return "LFE";
            case 4: return "Ls";
            case 5: return "Rs";
            case 6: return "Lrs";
            case 7: return "Rrs";
            default: return juce::String(index + 1);
        }
    }
    if (channelCount == 7) {
        switch (index) {
            case 0: return "L";
            case 1: return "R";
            case 2: return "C";
            case 3: return "Ls";
            case 4: return "Rs";
            case 5: return "Lrs";
            case 6: return "Rrs";
            default: return juce::String(index + 1);
        }
    }
    if (channelCount == 5) {
        switch (index) {
            case 0: return "L";
            case 1: return "R";
            case 2: return "C";
            case 3: return "Ls";
            case 4: return "Rs";
            default: return juce::String(index + 1);
        }
    }
    switch (index) {
        case 0: return "L";
        case 1: return "R";
        case 2: return "C";
        case 3: return "LFE";
        case 4: return "Ls";
        case 5: return "Rs";
        default: return juce::String(index + 1);
    }
}

}  // namespace

void StereoLevelMeterComponent::paint(juce::Graphics& g) {
    auto area = getLocalBounds().toFloat();
    g.setColour(colours::panel);
    g.fillRoundedRectangle(area, 3.0f);
    g.setColour(colours::accentRecord.withAlpha(0.4f));
    g.drawRoundedRectangle(area.reduced(0.5f), 3.0f, 2.0f);

    auto content = area.reduced(5.0f, 4.0f);
    if (bars_.empty()) {
        return;
    }

    const float gap = bars_.size() > 2 ? 1.5f : 4.0f;
    const float totalGap = gap * static_cast<float>(std::max(0, static_cast<int>(bars_.size()) - 1));
    const float barHeight = std::max(3.0f, (content.getHeight() - totalGap) / static_cast<float>(bars_.size()));

    for (std::size_t i = 0; i < bars_.size(); ++i) {
        auto barArea = content.removeFromTop(barHeight);
        if (i + 1 < bars_.size()) {
            content.removeFromTop(gap);
        }
        drawMeterBar(g,
                     barArea,
                     bars_[i].level,
                     bars_[i].peakHold,
                     meterLabelForIndex(static_cast<int>(i), static_cast<int>(bars_.size())));
    }
}

void StereoLevelMeterComponent::setLevels(float left, float right) {
    setLevels(std::vector<float>{left, right});
}

void StereoLevelMeterComponent::setLevels(const std::vector<float>& levels) {
    const std::size_t targetCount = std::max<std::size_t>(2, levels.empty() ? 2 : levels.size());
    if (bars_.size() != targetCount) {
        bars_.assign(targetCount, MeterBarState{});
    }

    for (std::size_t i = 0; i < bars_.size(); ++i) {
        const float level = i < levels.size() ? levels[i] : 0.0f;
        updateMeterState(std::max(0.0f, level),
                         bars_[i].level,
                         bars_[i].peakHold,
                         bars_[i].peakHoldFrames);
    }
    repaint();
}

void StereoLevelMeterComponent::updateMeterState(float inputLevel,
                                                 float& displayedLevel,
                                                 float& peakHold,
                                                 int& peakHoldFrames) {
    const float clamped = std::clamp(inputLevel, 0.0f, 1.0f);
    if (clamped >= displayedLevel) {
        displayedLevel = clamped;
    } else {
        displayedLevel = std::max(clamped, displayedLevel - kReleasePerUpdate);
    }

    if (clamped >= peakHold) {
        peakHold = clamped;
        peakHoldFrames = kPeakHoldFrames;
    } else if (peakHoldFrames > 0) {
        --peakHoldFrames;
    } else {
        peakHold = std::max(displayedLevel, peakHold - kPeakReleasePerUpdate);
    }
}

void StereoLevelMeterComponent::drawMeterBar(juce::Graphics& g,
                                             juce::Rectangle<float> area,
                                             float level,
                                             float peakHold,
                                             const juce::String& label) {
    auto bounds = area.reduced(0.0f, 1.0f);
    auto labelArea = bounds.removeFromLeft(std::min(32.0f, bounds.getWidth() * 0.18f));
    auto barArea = bounds.reduced(1.0f, 0.0f);

    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(barArea.getHeight() < 7.0f ? 7.0f : 10.0f, juce::Font::bold));
    g.drawText(label, labelArea, juce::Justification::centredLeft, false);

    g.setColour(colours::waveBg);
    g.fillRoundedRectangle(barArea, 2.0f);
    g.setColour(colours::border.withAlpha(0.8f));
    g.drawRoundedRectangle(barArea, 2.0f, 1.0f);

    const float clamped = std::clamp(level, 0.0f, 1.0f);
    const float fillWidth = barArea.getWidth() * clamped;
    if (fillWidth <= 0.0f) {
        const float holdX = barArea.getX() + barArea.getWidth() * std::clamp(peakHold, 0.0f, 1.0f);
        if (peakHold > 0.0f) {
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.drawVerticalLine(static_cast<int>(holdX), barArea.getY(), barArea.getBottom());
        }
        return;
    }

    const float greenWidth = barArea.getWidth() * kGreenEnd;
    const float yellowWidth = barArea.getWidth() * (kYellowEnd - kGreenEnd);
    const float redStart = greenWidth + yellowWidth;

    auto greenRect = barArea.withWidth(std::min(fillWidth, greenWidth));
    if (greenRect.getWidth() > 0.0f) {
        g.setColour(colours::accentGreen.withAlpha(0.9f));
        g.fillRoundedRectangle(greenRect, 2.0f);
    }

    if (fillWidth > greenWidth) {
        auto yellowRect = juce::Rectangle<float>(
            barArea.getX() + greenWidth,
            barArea.getY(),
            std::min(fillWidth - greenWidth, yellowWidth),
            barArea.getHeight());
        if (yellowRect.getWidth() > 0.0f) {
            g.setColour(juce::Colours::gold.withAlpha(0.95f));
            g.fillRect(yellowRect);
        }
    }

    if (fillWidth > redStart) {
        auto redRect = juce::Rectangle<float>(
            barArea.getX() + redStart,
            barArea.getY(),
            fillWidth - redStart,
            barArea.getHeight());
        if (redRect.getWidth() > 0.0f) {
            g.setColour(juce::Colours::red.withAlpha(0.95f));
            g.fillRoundedRectangle(redRect, 2.0f);
        }
    }

    const float holdX = barArea.getX() + barArea.getWidth() * std::clamp(peakHold, 0.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(0.9f));
    g.drawVerticalLine(static_cast<int>(holdX), barArea.getY(), barArea.getBottom());
}

}  // namespace triggerfish

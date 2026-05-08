#include "MasterControlsComponent.h"
#include "../LookAndFeel_Radium.h"
#include "BinaryData.h"

#include <algorithm>

namespace triggerfish {
namespace {
constexpr double kTrackGainMax = 3.9810717055; // +12 dB
constexpr double kBassCutDb = -24.0;
constexpr double kBassBoostDb = 12.0;
}

MasterControlsComponent::MasterControlsComponent() {
    logoImage_ = juce::ImageCache::getFromMemory(BinaryData::triggerfish_logo_png,
                                                  BinaryData::triggerfish_logo_pngSize);

    sectionLabel_.setFont(juce::FontOptions(13.0f));
    sectionLabel_.setColour(juce::Label::textColourId, colours::textPrimary);
    addAndMakeVisible(sectionLabel_);

    reverseToggle_.setColour(juce::ToggleButton::textColourId, colours::textPrimary);
    reverseToggle_.setColour(juce::ToggleButton::tickColourId, colours::accentFocus);
    reverseToggle_.setColour(juce::TextButton::buttonOnColourId, colours::accentFocus);
    reverseToggle_.onClick = [this] {
        if (onReverseToggle) onReverseToggle(reverseToggle_.getToggleState());
    };
    addAndMakeVisible(reverseToggle_);

    stretchLabel_.setFont(juce::FontOptions(11.0f));
    stretchLabel_.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(stretchLabel_);

    stretchSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    stretchSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 18);
    stretchSlider_.setRange(1.0, 800.0, 1.0);
    stretchSlider_.setValue(100.0, juce::dontSendNotification);
    stretchSlider_.setCtrlClickResetValue(100.0);
    stretchSlider_.setSkewFactorFromMidPoint(100.0);
    stretchSlider_.setColour(juce::Slider::trackColourId, colours::accentFocus);
    stretchSlider_.onValueChange = [this] {
        if (onTimeStretchChange) onTimeStretchChange(stretchSlider_.getValue() / 100.0);
    };
    addAndMakeVisible(stretchSlider_);

    gainLabel_.setFont(juce::FontOptions(11.0f));
    gainLabel_.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(gainLabel_);

    trackGainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    trackGainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 18);
    trackGainSlider_.setRange(0.0, kTrackGainMax, 0.01);
    trackGainSlider_.setValue(1.0, juce::dontSendNotification);
    trackGainSlider_.setCtrlClickResetValue(1.0);
    trackGainSlider_.setColour(juce::Slider::trackColourId, colours::accentFocus);
    trackGainSlider_.onValueChange = [this] {
        if (onTrackGainChange) onTrackGainChange(static_cast<float>(trackGainSlider_.getValue()));
    };
    addAndMakeVisible(trackGainSlider_);

    bassLabel_.setFont(juce::FontOptions(11.0f));
    bassLabel_.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(bassLabel_);

    bassSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    bassSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 18);
    bassSlider_.setRange(kBassCutDb, kBassBoostDb, 0.1);
    bassSlider_.setValue(0.0, juce::dontSendNotification);
    bassSlider_.setCtrlClickResetValue(0.0);
    bassSlider_.setColour(juce::Slider::trackColourId, colours::accentFocus);
    bassSlider_.textFromValueFunction = [](double value) {
        return juce::String(value, 1) + " dB";
    };
    bassSlider_.onValueChange = [this] {
        if (onBassLfeGainChange) onBassLfeGainChange(bassSlider_.getValue());
    };
    addAndMakeVisible(bassSlider_);
    updateAutomationColours();
}

void MasterControlsComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::panel);
    g.fillRoundedRectangle(bounds, 4.0f);

    if (logoImage_.isValid() && logoBounds_.getWidth() > 16.0f && logoBounds_.getHeight() > 12.0f) {
        const auto imageAspect = static_cast<float>(logoImage_.getWidth()) /
                                 static_cast<float>(std::max(1, logoImage_.getHeight()));
        float drawWidth = logoBounds_.getWidth();
        float drawHeight = drawWidth / imageAspect;
        if (drawHeight > logoBounds_.getHeight()) {
            drawHeight = logoBounds_.getHeight();
            drawWidth = drawHeight * imageAspect;
        }

        auto drawArea = juce::Rectangle<float>(drawWidth, drawHeight).withCentre(logoBounds_.getCentre());
        g.setOpacity(0.92f);
        g.drawImage(logoImage_, drawArea, juce::RectanglePlacement::centred);
        g.setOpacity(1.0f);
    }

    g.setColour(colours::accentFocus.withAlpha(0.42f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 2.0f);
}

void MasterControlsComponent::resized() {
    auto area = getLocalBounds().reduced(6, 4);

    sectionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    reverseToggle_.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);

    const auto availableForRows = area.getHeight();
    const auto rowHeight = std::clamp((availableForRows - 4) / 3, 27, 34);

    auto placeSliderRow = [&](juce::Label& label, juce::Slider& slider) {
        auto row = area.removeFromTop(rowHeight);
        label.setBounds(row.removeFromTop(11));
        row.removeFromTop(1);
        slider.setBounds(row.removeFromTop(std::min(20, row.getHeight())));
        area.removeFromTop(2);
    };

    placeSliderRow(stretchLabel_, stretchSlider_);
    placeSliderRow(gainLabel_, trackGainSlider_);
    placeSliderRow(bassLabel_, bassSlider_);

    logoBounds_ = area.reduced(8, 5).toFloat();
    if (logoBounds_.getHeight() < 18.0f) {
        logoBounds_ = {};
    }
}

void MasterControlsComponent::setEffectState(bool reverse, double timeStretchRatio) {
    reverseToggle_.setToggleState(reverse, juce::dontSendNotification);
    stretchSlider_.setValue(timeStretchRatio * 100.0, juce::dontSendNotification);
}

void MasterControlsComponent::setBassLfeGainDb(double gainDb) {
    bassSlider_.setValue(std::clamp(gainDb, kBassCutDb, kBassBoostDb), juce::dontSendNotification);
}

void MasterControlsComponent::setTrackGain(float gain) {
    trackGainSlider_.setValue(gain, juce::dontSendNotification);
}

void MasterControlsComponent::setTrackGainEnabled(bool enabled) {
    gainLabel_.setEnabled(enabled);
    trackGainSlider_.setEnabled(enabled);
    bassLabel_.setEnabled(enabled);
    bassSlider_.setEnabled(enabled);
}

void MasterControlsComponent::setStretchAutomated(bool automated) {
    if (stretchAutomated_ == automated) {
        return;
    }
    stretchAutomated_ = automated;
    updateAutomationColours();
}

void MasterControlsComponent::setTrackGainAutomated(bool automated) {
    if (trackGainAutomated_ == automated) {
        return;
    }
    trackGainAutomated_ = automated;
    updateAutomationColours();
}

void MasterControlsComponent::updateAutomationColours() {
    const auto stretchColour = stretchAutomated_ ? colours::accentRed : colours::accentFocus;
    const auto gainColour = trackGainAutomated_ ? colours::accentRed : colours::accentFocus;

    stretchLabel_.setColour(juce::Label::textColourId,
                            stretchAutomated_ ? colours::accentRed.brighter(0.3f) : colours::textDim);
    gainLabel_.setColour(juce::Label::textColourId,
                         trackGainAutomated_ ? colours::accentRed.brighter(0.3f) : colours::textDim);

    stretchSlider_.setColour(juce::Slider::trackColourId, stretchColour);
    stretchSlider_.setColour(juce::Slider::thumbColourId,
                             stretchAutomated_ ? colours::accentRed.brighter(0.15f) : colours::textPrimary);
    trackGainSlider_.setColour(juce::Slider::trackColourId, gainColour);
    trackGainSlider_.setColour(juce::Slider::thumbColourId,
                               trackGainAutomated_ ? colours::accentRed.brighter(0.15f) : colours::textPrimary);
    repaint();
}

}  // namespace triggerfish

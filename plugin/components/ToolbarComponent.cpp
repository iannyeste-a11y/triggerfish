#include "ToolbarComponent.h"
#include "../LookAndFeel_Radium.h"

namespace triggerfish {

ToolbarComponent::ToolbarComponent() {
    for (auto* b : {&newButton, &openButton, &addAudioButton, &saveButton, &databaseButton, &pictureButton, &searchButton}) {
        addAndMakeVisible(b);
    }

    masterLabel_.setColour(juce::Label::textColourId, colours::textDim);
    masterLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(masterLabel_);

    masterGainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    masterGainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    masterGainSlider_.setRange(0.0, 2.0, 0.01);
    masterGainSlider_.setValue(1.0, juce::dontSendNotification);
    masterGainSlider_.setCtrlClickResetValue(1.0);
    masterGainSlider_.setColour(juce::Slider::trackColourId, colours::accentFocus);
    masterGainSlider_.textFromValueFunction = [](double value) {
        return juce::String(value, 2);
    };
    masterGainSlider_.onValueChange = [this] {
        if (onMasterGainChange) {
            onMasterGainChange(static_cast<float>(masterGainSlider_.getValue()));
        }
    };
    addAndMakeVisible(masterGainSlider_);

    newButton.onClick = [this] { if (onNew) onNew(); };
    openButton.onClick = [this] { if (onOpen) onOpen(); };
    addAudioButton.onClick = [this] { if (onAddAudio) onAddAudio(); };
    saveButton.onClick = [this] { if (onSave) onSave(); };
    databaseButton.onClick = [this] { if (onDatabaseMenu) onDatabaseMenu(); };
    pictureButton.onClick = [this] { if (onPicture) onPicture(); };
    searchButton.onClick = [this] { if (onSearch) onSearch(); };
}

void ToolbarComponent::paint(juce::Graphics& g) {
    if (!midiStatusVisible_) {
        return;
    }

    auto badge = getLocalBounds().reduced(4, 2).removeFromRight(56).toFloat();
    badge.setHeight(20.0f);
    badge.setY(6.0f);

    const auto border = midiActive_ ? colours::accentGreen : colours::border;
    const auto fill = midiActive_
        ? colours::accentGreen.withAlpha(0.18f)
        : colours::panel.brighter(0.05f);
    const auto dot = midiActive_ ? colours::accentGreen : colours::textDim;

    g.setColour(fill);
    g.fillRoundedRectangle(badge, 4.0f);
    g.setColour(border.withAlpha(midiActive_ ? 0.95f : 0.7f));
    g.drawRoundedRectangle(badge.reduced(0.5f), 4.0f, midiActive_ ? 1.5f : 1.0f);

    const float dotSize = 7.0f;
    const float dotX = badge.getX() + 6.0f;
    const float dotY = badge.getCentreY() - dotSize * 0.5f;
    g.setColour(dot);
    g.fillEllipse(dotX, dotY, dotSize, dotSize);

    g.setColour(midiActive_ ? colours::textPrimary : colours::textDim);
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText(midiStatusText_,
               juce::Rectangle<int>(static_cast<int>(badge.getX() + 18.0f),
                                    static_cast<int>(badge.getY()),
                                    static_cast<int>(badge.getWidth() - 20.0f),
                                    static_cast<int>(badge.getHeight())),
               juce::Justification::centredLeft,
               false);
}

void ToolbarComponent::resized() {
    auto area = getLocalBounds().reduced(4, 2);
    const int bh = 28;

    newButton.setBounds(area.removeFromLeft(50).withHeight(bh));
    area.removeFromLeft(4);
    openButton.setBounds(area.removeFromLeft(50).withHeight(bh));
    area.removeFromLeft(4);
    addAudioButton.setBounds(area.removeFromLeft(86).withHeight(bh));
    area.removeFromLeft(4);
    saveButton.setBounds(area.removeFromLeft(50).withHeight(bh));
    area.removeFromLeft(4);
    databaseButton.setBounds(area.removeFromLeft(74).withHeight(bh));
    area.removeFromLeft(4);
    pictureButton.setBounds(area.removeFromLeft(58).withHeight(bh));
    area.removeFromLeft(4);
    searchButton.setBounds(area.removeFromLeft(58).withHeight(bh));
    area.removeFromLeft(8);

    masterLabel_.setBounds(area.removeFromLeft(40).withHeight(bh));
    masterGainSlider_.setBounds(area.removeFromLeft(124).withHeight(bh));
    area.removeFromLeft(10);
    juce::ignoreUnused(area);
}

void ToolbarComponent::setProjectName(const juce::String& name) {
    juce::ignoreUnused(name);
}

void ToolbarComponent::setMasterGain(float gain) {
    masterGainSlider_.setValue(gain, juce::dontSendNotification);
}

void ToolbarComponent::setMidiStatusVisible(bool visible) {
    if (midiStatusVisible_ == visible) {
        return;
    }
    midiStatusVisible_ = visible;
    resized();
    repaint();
}

void ToolbarComponent::setMidiStatus(bool active) {
    midiActive_ = active;
    repaint();
}

}  // namespace triggerfish

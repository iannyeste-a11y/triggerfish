#include "PianoKeyboardComponent.h"
#include "../LookAndFeel_Radium.h"

namespace triggerfish {

PianoKeyboardComponent::PianoKeyboardComponent() {
    for (int i = 0; i < kNumKeys; ++i) {
        auto& key = keys_[i];
        key.setButtonText(kNoteNames[i]);
        if (isBlackKey(i)) {
            key.setColour(juce::TextButton::buttonColourId, juce::Colour(35, 36, 40));
        } else {
            key.setColour(juce::TextButton::buttonColourId, juce::Colour(70, 72, 76));
        }
        key.onClick = [this, i] {
            if (onNoteOn) onNoteOn(midiNoteForKey(i));
        };
        addAndMakeVisible(key);
    }

    octaveDown_.onClick = [this] { setOctave(octave_ - 1); };
    octaveUp_.onClick = [this] { setOctave(octave_ + 1); };
    addAndMakeVisible(octaveDown_);
    addAndMakeVisible(octaveUp_);
    loopToggle_.setClickingTogglesState(true);
    loopToggle_.onClick = [this] {
        loopMode_ = loopToggle_.getToggleState();
        if (onLoopModeChanged) {
            onLoopModeChanged(loopMode_);
        }
    };
    addAndMakeVisible(loopToggle_);

    octaveLabel_.setFont(juce::FontOptions(13.0f));
    octaveLabel_.setColour(juce::Label::textColourId, colours::textPrimary);
    octaveLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(octaveLabel_);

    updateKeyLabels();
}

bool PianoKeyboardComponent::isBlackKey(int index) const {
    return index == 1 || index == 3 || index == 6 || index == 8 || index == 10;
}

void PianoKeyboardComponent::setOctave(int oct) {
    octave_ = std::clamp(oct, 0, 8);
    updateKeyLabels();
}

void PianoKeyboardComponent::stepOctave(int delta) {
    if (delta == 0) {
        return;
    }

    auto& button = delta > 0 ? octaveUp_ : octaveDown_;
    button.triggerClick();
}

void PianoKeyboardComponent::triggerVirtualKey(int keyIndex) {
    if (keyIndex < 0 || keyIndex >= kNumKeys) {
        return;
    }

    keys_[keyIndex].triggerClick();
}

void PianoKeyboardComponent::setLoopMode(bool enabled) {
    loopMode_ = enabled;
    loopToggle_.setToggleState(enabled, juce::dontSendNotification);
}

void PianoKeyboardComponent::updateKeyLabels() {
    octaveLabel_.setText("Octave " + juce::String(octave_), juce::dontSendNotification);
}

void PianoKeyboardComponent::resized() {
    auto area = getLocalBounds().reduced(4, 2);

    // Octave controls on the left
    auto octaveArea = area.removeFromLeft(120);
    octaveDown_.setBounds(octaveArea.removeFromLeft(30).withHeight(32));
    octaveArea.removeFromLeft(2);
    octaveLabel_.setBounds(octaveArea.removeFromLeft(56).withHeight(32));
    octaveArea.removeFromLeft(2);
    octaveUp_.setBounds(octaveArea.removeFromLeft(30).withHeight(32));

    area.removeFromLeft(8);
    loopToggle_.setBounds(area.removeFromLeft(66).withHeight(32));

    area.removeFromLeft(8);

    // Piano keys
    const int keyWidth = std::min(52, area.getWidth() / kNumKeys);
    for (int i = 0; i < kNumKeys; ++i) {
        keys_[i].setBounds(area.removeFromLeft(keyWidth).withHeight(40));
        area.removeFromLeft(1);
    }
}

}  // namespace triggerfish

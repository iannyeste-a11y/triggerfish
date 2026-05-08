#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace triggerfish {

class PianoKeyboardComponent : public juce::Component {
public:
    PianoKeyboardComponent();
    void resized() override;

    std::function<void(int midiNote)> onNoteOn;
    std::function<void(int midiNote)> onNoteOff;
    std::function<void(bool enabled)> onLoopModeChanged;

    int octave() const { return octave_; }
    void setOctave(int oct);
    void stepOctave(int delta);
    void triggerVirtualKey(int keyIndex);
    bool loopMode() const { return loopMode_; }
    void setLoopMode(bool enabled);

private:
    static constexpr int kNumKeys = 12;
    static constexpr const char* kNoteNames[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    int octave_ = 4;
    juce::TextButton keys_[kNumKeys];
    juce::TextButton octaveDown_{"-"};
    juce::TextButton octaveUp_{"+"};
    juce::ToggleButton loopToggle_{"Loop"};
    juce::Label octaveLabel_;
    bool loopMode_ = false;

    int midiNoteForKey(int keyIndex) const { return (octave_ + 1) * 12 + keyIndex; }
    bool isBlackKey(int index) const;
    void updateKeyLabels();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PianoKeyboardComponent)
};

}  // namespace triggerfish

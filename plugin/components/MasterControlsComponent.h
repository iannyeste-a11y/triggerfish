#pragma once

#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace triggerfish {

class MasterControlsComponent : public juce::Component {
public:
    MasterControlsComponent();

    void paint(juce::Graphics&) override;
    void resized() override;

    // Update displayed state for the selected layer
    void setEffectState(bool reverse, double timeStretchRatio);
    void setBassLfeGainDb(double gainDb);
    void setTrackGain(float gain);
    void setTrackGainEnabled(bool enabled);
    void setStretchAutomated(bool automated);
    void setTrackGainAutomated(bool automated);

    // Callbacks
    std::function<void(bool)> onReverseToggle;
    std::function<void(double)> onTimeStretchChange;
    std::function<void(double)> onBassLfeGainChange;
    std::function<void(float)> onTrackGainChange;
private:
    juce::Label sectionLabel_{"", "Layer FX"};
    juce::ToggleButton reverseToggle_{"Reverse"};
    juce::Label stretchLabel_{"", "Stretch %"};
    FineTuneSlider stretchSlider_;

    juce::Label gainLabel_{"", "Volume"};
    FineTuneSlider trackGainSlider_;
    juce::Label bassLabel_{"", "Bass dB"};
    FineTuneSlider bassSlider_;
    juce::Image logoImage_;
    juce::Rectangle<float> logoBounds_;
    bool stretchAutomated_ = false;
    bool trackGainAutomated_ = false;

    void updateAutomationColours();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterControlsComponent)
};

}  // namespace triggerfish

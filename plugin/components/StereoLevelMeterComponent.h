#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace triggerfish {

class StereoLevelMeterComponent : public juce::Component {
public:
    StereoLevelMeterComponent() = default;

    void paint(juce::Graphics& g) override;

    void setLevels(float left, float right);
    void setLevels(const std::vector<float>& levels);

private:
    struct MeterBarState {
        float level = 0.0f;
        float peakHold = 0.0f;
        int peakHoldFrames = 0;
    };

    std::vector<MeterBarState> bars_{{}, {}};

    static void updateMeterState(float inputLevel, float& displayedLevel, float& peakHold, int& peakHoldFrames);
    static void drawMeterBar(juce::Graphics& g,
                             juce::Rectangle<float> area,
                             float level,
                             float peakHold,
                             const juce::String& label);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StereoLevelMeterComponent)
};

}  // namespace triggerfish

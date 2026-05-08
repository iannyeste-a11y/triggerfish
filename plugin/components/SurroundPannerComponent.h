#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace triggerfish {

class SurroundPannerComponent : public juce::Component {
public:
    SurroundPannerComponent();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

    // Set the current pan positions. x: [-1,1] L-R, y: [-1,1] rear-front.
    void setPan(double x, double y);
    void setPanRight(double x, double y);
    void setStereo(bool stereo);
    void setAutomationHighlight(bool leftAutomated, bool rightAutomated);

    // Callbacks when the user drags a dot
    std::function<void(double x, double y)> onPanChanged;
    std::function<void(double x, double y)> onPanRightChanged;

private:
    juce::Point<float> panToPixel(double x, double y) const;
    std::pair<double, double> pixelToPan(float px, float py) const;

    double panX_ = 0.0, panY_ = 1.0;
    double panXR_ = 0.0, panYR_ = 1.0;
    bool stereo_ = false;
    bool draggingRight_ = false;
    bool leftAutomated_ = false;
    bool rightAutomated_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SurroundPannerComponent)
};

}  // namespace triggerfish

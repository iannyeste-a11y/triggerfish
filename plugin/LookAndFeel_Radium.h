#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace triggerfish {

// Color palette matching the original dark theme
namespace colours {
    const juce::Colour background     { 24,  24,  28};
    const juce::Colour panel          { 30,  31,  35};
    const juce::Colour inputField     { 28,  29,  32};
    const juce::Colour buttonLight    { 48,  50,  54};
    const juce::Colour buttonHover    { 62,  64,  68};
    const juce::Colour buttonPressed  { 20,  20,  22};
    const juce::Colour textPrimary    {200, 202, 208};
    const juce::Colour textDim        {110, 112, 120};
    const juce::Colour textAccent     {100, 170, 240};
    const juce::Colour border         { 50,  52,  56};
    const juce::Colour waveform       { 65, 150, 210};
    const juce::Colour loopFill       { 24,  34,  50};
    const juce::Colour regionFill     { 50,  42,  24};
    const juce::Colour accentRed      {190,  50,  50};
    const juce::Colour accentGreen    { 35, 170,  75};
    const juce::Colour accentAmber    {200, 160,  30};
    const juce::Colour waveBg         { 18,  18,  20};
    const juce::Colour regionHandle   {214, 140,  50};
    const juce::Colour sectionLayers  { 52,  68,  96};
    const juce::Colour sectionRecord  { 88,  54,  50};
    const juce::Colour sectionFocus   { 46,  82,  74};
    const juce::Colour accentRecord   {196,  88,  78};
    const juce::Colour accentFocus    { 84, 186, 138};
    const juce::Colour waveformRecord {194,  92,  82};
    const juce::Colour waveformFocus  { 78, 176, 126};
    const juce::Colour loopFillFocus  { 20,  44,  34};
}

// Slider subclass that slows drag movement when Ctrl is held for fine-tuning.
class FineTuneSlider : public juce::Slider {
public:
    using juce::Slider::Slider;

    void setCtrlClickResetValue(double resetValue) {
        ctrlClickResetEnabled_ = true;
        ctrlClickResetValue_ = resetValue;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // isCommandDown() = Cmd on macOS, Ctrl on Win/Linux — matches the
        // native fine-adjust convention of Logic / Pro Tools / Reaper.
        ctrlClickCandidate_ = ctrlClickResetEnabled_ &&
                              e.mods.isCommandDown() &&
                              e.mods.isLeftButtonDown();
        ctrlClickDragged_ = false;
        ctrlClickStartPosition_ = e.position;

        if (e.mods.isCommandDown()) {
            // Switch to velocity mode for fine control
            setVelocityBasedMode(true);
            setVelocityModeParameters(0.4, 0, 0.0, false);
        } else {
            setVelocityBasedMode(false);
        }
        juce::Slider::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (ctrlClickCandidate_ &&
            e.position.getDistanceFrom(ctrlClickStartPosition_) > 3.0f) {
            ctrlClickDragged_ = true;
        }
        juce::Slider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override {
        juce::Slider::mouseUp(e);
        setVelocityBasedMode(false);
        if (ctrlClickCandidate_ && !ctrlClickDragged_) {
            setValue(ctrlClickResetValue_, juce::sendNotificationSync);
        }
        ctrlClickCandidate_ = false;
        ctrlClickDragged_ = false;
    }

private:
    bool ctrlClickResetEnabled_ = false;
    bool ctrlClickCandidate_ = false;
    bool ctrlClickDragged_ = false;
    double ctrlClickResetValue_ = 0.0;
    juce::Point<float> ctrlClickStartPosition_;
};

class LookAndFeel_Radium : public juce::LookAndFeel_V4 {
public:
    LookAndFeel_Radium();

    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
};

}  // namespace triggerfish

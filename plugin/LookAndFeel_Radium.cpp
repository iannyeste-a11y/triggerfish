#include "LookAndFeel_Radium.h"

namespace triggerfish {

LookAndFeel_Radium::LookAndFeel_Radium() {
    setColour(juce::ResizableWindow::backgroundColourId, colours::background);
    setColour(juce::TextButton::buttonColourId, colours::buttonLight);
    setColour(juce::TextButton::textColourOffId, colours::textPrimary);
    setColour(juce::TextButton::textColourOnId, colours::textPrimary);
    setColour(juce::ComboBox::backgroundColourId, colours::inputField);
    setColour(juce::ComboBox::textColourId, colours::textPrimary);
    setColour(juce::ComboBox::outlineColourId, colours::border);
    setColour(juce::Label::textColourId, colours::textPrimary);
    setColour(juce::Slider::backgroundColourId, colours::inputField);
    setColour(juce::Slider::trackColourId, colours::textAccent);
    setColour(juce::Slider::thumbColourId, colours::textPrimary);
    setColour(juce::TextEditor::backgroundColourId, colours::inputField);
    setColour(juce::TextEditor::textColourId, colours::textPrimary);
    setColour(juce::TextEditor::outlineColourId, colours::border);
    setColour(juce::ScrollBar::thumbColourId, colours::buttonLight);
}

void LookAndFeel_Radium::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                               const juce::Colour&,
                                               bool isHighlighted, bool isDown) {
    auto bounds = button.getLocalBounds().toFloat();
    juce::Colour bg = button.findColour(juce::TextButton::buttonColourId, true);
    if (bg == juce::Colour()) {
        bg = colours::buttonLight;
    }
    if (isDown)
        bg = colours::buttonPressed;
    else if (isHighlighted)
        bg = colours::buttonHover;

    const bool toggledOn = button.getToggleState();
    if (toggledOn) {
        auto onColour = button.findColour(juce::TextButton::buttonOnColourId, true);
        auto baseOn = (onColour == juce::Colour() ? colours::textAccent : onColour);
        bg = baseOn.interpolatedWith(colours::accentAmber, 0.38f).withAlpha(0.72f);
    }

    if (toggledOn) {
        g.setColour(colours::accentAmber.withAlpha(0.22f));
        g.fillRoundedRectangle(bounds.expanded(2.0f, 2.0f), 5.0f);
        g.setColour(colours::accentAmber.withAlpha(0.12f));
        g.fillRoundedRectangle(bounds.expanded(4.0f, 4.0f), 6.0f);
    }

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(toggledOn ? colours::accentAmber.brighter(0.25f) : colours::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, toggledOn ? 2.0f : 1.0f);
}

void LookAndFeel_Radium::drawButtonText(juce::Graphics& g, juce::TextButton& button,
                                         bool, bool) {
    auto colour = button.findColour(juce::TextButton::textColourOffId);
    if (!button.isEnabled())
        colour = colours::textDim;

    g.setFont(juce::FontOptions(13.0f));
    g.setColour(colour);
    g.drawText(button.getButtonText(), button.getLocalBounds(),
               juce::Justification::centred, true);
}

void LookAndFeel_Radium::drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                                           bool isHighlighted, bool isDown) {
    auto bounds = button.getLocalBounds().toFloat();
    const bool toggledOn = button.getToggleState();
    juce::Colour bg = toggledOn
        ? button.findColour(juce::TextButton::buttonOnColourId, true).withAlpha(0.35f)
        : button.findColour(juce::TextButton::buttonColourId, true);
    if (bg == juce::Colour()) {
        bg = toggledOn ? colours::textAccent.withAlpha(0.35f) : colours::buttonLight;
    }
    if (isDown) bg = colours::buttonPressed;
    else if (isHighlighted) bg = colours::buttonHover;
    if (toggledOn) {
        auto onColour = button.findColour(juce::TextButton::buttonOnColourId, true);
        auto baseOn = (onColour == juce::Colour() ? colours::textAccent : onColour);
        bg = baseOn.interpolatedWith(colours::accentAmber, 0.38f).withAlpha(0.72f);
        g.setColour(colours::accentAmber.withAlpha(0.22f));
        g.fillRoundedRectangle(bounds.expanded(2.0f, 2.0f), 5.0f);
        g.setColour(colours::accentAmber.withAlpha(0.12f));
        g.fillRoundedRectangle(bounds.expanded(4.0f, 4.0f), 6.0f);
    }

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(toggledOn ? colours::accentAmber.brighter(0.25f) : colours::border);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, toggledOn ? 2.0f : 1.0f);
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(button.isEnabled() ? colours::textPrimary : colours::textDim);
    g.drawText(button.getButtonText(), bounds, juce::Justification::centred, true);
}

void LookAndFeel_Radium::drawLinearSlider(juce::Graphics& g, int x, int y,
                                           int width, int height,
                                           float sliderPos, float, float,
                                           juce::Slider::SliderStyle, juce::Slider& slider) {
    auto bounds = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y),
                                          static_cast<float>(width), static_cast<float>(height));
    auto background = slider.findColour(juce::Slider::backgroundColourId, true);
    auto track = slider.findColour(juce::Slider::trackColourId, true);
    auto thumb = slider.findColour(juce::Slider::thumbColourId, true);
    if (background == juce::Colour()) background = colours::inputField;
    if (track == juce::Colour()) track = colours::textAccent;
    if (thumb == juce::Colour()) thumb = colours::textPrimary;

    g.setColour(background);
    g.fillRoundedRectangle(bounds, 2.0f);

    auto filled = bounds;
    filled.setWidth(sliderPos - static_cast<float>(x));
    g.setColour(track.withAlpha(0.55f));
    g.fillRoundedRectangle(filled, 2.0f);

    // Thumb
    g.setColour(thumb);
    g.fillEllipse(sliderPos - 5.0f, bounds.getCentreY() - 5.0f, 10.0f, 10.0f);
}

}  // namespace triggerfish

#include "SurroundPannerComponent.h"
#include "../LookAndFeel_Radium.h"
#include <algorithm>

namespace triggerfish {

SurroundPannerComponent::SurroundPannerComponent() {
    setWantsKeyboardFocus(false);
}

void SurroundPannerComponent::setPan(double x, double y) {
    panX_ = x;
    panY_ = y;
    repaint();
}

void SurroundPannerComponent::setPanRight(double x, double y) {
    panXR_ = x;
    panYR_ = y;
    repaint();
}

void SurroundPannerComponent::setStereo(bool s) {
    stereo_ = s;
    repaint();
}

void SurroundPannerComponent::setAutomationHighlight(bool leftAutomated, bool rightAutomated) {
    if (leftAutomated_ == leftAutomated && rightAutomated_ == rightAutomated) {
        return;
    }
    leftAutomated_ = leftAutomated;
    rightAutomated_ = rightAutomated;
    repaint();
}

juce::Point<float> SurroundPannerComponent::panToPixel(double x, double y) const {
    auto area = getLocalBounds().toFloat().reduced(14.0f);
    float px = area.getX() + static_cast<float>((x + 1.0) * 0.5) * area.getWidth();
    // y: 1 = front (top), -1 = rear (bottom)
    float py = area.getY() + static_cast<float>((1.0 - y) * 0.5) * area.getHeight();
    return {px, py};
}

std::pair<double, double> SurroundPannerComponent::pixelToPan(float px, float py) const {
    auto area = getLocalBounds().toFloat().reduced(14.0f);
    double x = ((px - area.getX()) / area.getWidth()) * 2.0 - 1.0;
    double y = 1.0 - ((py - area.getY()) / area.getHeight()) * 2.0;
    return {std::clamp(x, -1.0, 1.0), std::clamp(y, -1.0, 1.0)};
}

void SurroundPannerComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    auto area = bounds.reduced(14.0f);

    // Background
    g.setColour(colours::waveBg);
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(colours::accentFocus.withAlpha(0.42f));
    g.drawRoundedRectangle(area, 4.0f, 2.0f);

    // Grid lines (crosshair)
    float cx = area.getCentreX();
    float cy = area.getCentreY();
    g.setColour(colours::border.withAlpha(0.5f));
    g.drawHorizontalLine(static_cast<int>(cy), area.getX(), area.getRight());
    g.drawVerticalLine(static_cast<int>(cx), area.getY(), area.getBottom());

    // Speaker labels
    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("L", area.getX(), area.getY() - 13.0f, 14.0f, 12.0f, juce::Justification::centred);
    g.drawText("C", cx - 7.0f, area.getY() - 13.0f, 14.0f, 12.0f, juce::Justification::centred);
    g.drawText("R", area.getRight() - 14.0f, area.getY() - 13.0f, 14.0f, 12.0f, juce::Justification::centred);
    g.drawText("Ls", area.getX(), area.getBottom() + 1.0f, 14.0f, 12.0f, juce::Justification::centred);
    g.drawText("Rs", area.getRight() - 14.0f, area.getBottom() + 1.0f, 14.0f, 12.0f, juce::Justification::centred);

    // Pan dot (left / mono)
    constexpr float dotRadius = 6.0f;
    auto dotPos = panToPixel(panX_, panY_);
    const auto leftFill = leftAutomated_ ? colours::accentRed : colours::accentFocus;
    const auto leftOutline = leftAutomated_ ? colours::accentRed.brighter(0.25f) : colours::accentFocus.brighter(0.3f);
    if (leftAutomated_) {
        g.setColour(colours::accentRed.withAlpha(0.18f));
        g.fillEllipse(dotPos.x - (dotRadius + 4.0f), dotPos.y - (dotRadius + 4.0f),
                      (dotRadius + 4.0f) * 2.0f, (dotRadius + 4.0f) * 2.0f);
    }
    g.setColour(leftFill);
    g.fillEllipse(dotPos.x - dotRadius, dotPos.y - dotRadius, dotRadius * 2, dotRadius * 2);
    g.setColour(leftOutline);
    g.drawEllipse(dotPos.x - dotRadius, dotPos.y - dotRadius, dotRadius * 2, dotRadius * 2, 2.0f);

    // Right channel dot (stereo only)
    if (stereo_) {
        auto dotR = panToPixel(panXR_, panYR_);
        const auto rightFill = rightAutomated_ ? colours::accentRed.withAlpha(0.75f)
                                               : colours::accentFocus.withAlpha(0.6f);
        const auto rightOutline = rightAutomated_ ? colours::accentRed.brighter(0.25f).withAlpha(0.95f)
                                                  : colours::accentFocus.brighter(0.3f).withAlpha(0.6f);
        if (rightAutomated_) {
            g.setColour(colours::accentRed.withAlpha(0.16f));
            g.fillEllipse(dotR.x - (dotRadius + 4.0f), dotR.y - (dotRadius + 4.0f),
                          (dotRadius + 4.0f) * 2.0f, (dotRadius + 4.0f) * 2.0f);
        }
        g.setColour(rightFill);
        g.fillEllipse(dotR.x - dotRadius, dotR.y - dotRadius, dotRadius * 2, dotRadius * 2);
        g.setColour(rightOutline);
        g.drawEllipse(dotR.x - dotRadius, dotR.y - dotRadius, dotRadius * 2, dotRadius * 2, 2.0f);

        // Label
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(9.0f));
        g.drawText("L", dotPos.x - 4.0f, dotPos.y - dotRadius - 11.0f, 8.0f, 10.0f, juce::Justification::centred);
        g.drawText("R", dotR.x - 4.0f, dotR.y - dotRadius - 11.0f, 8.0f, 10.0f, juce::Justification::centred);
    }
}

void SurroundPannerComponent::mouseDown(const juce::MouseEvent& e) {
    // Cmd-click on macOS, Ctrl-click on Win/Linux — resets pan to centre.
    if (e.mods.isCommandDown() && e.mods.isLeftButtonDown()) {
        panX_ = 0.0;
        panY_ = 1.0;
        panXR_ = 0.0;
        panYR_ = 1.0;
        if (onPanChanged) onPanChanged(panX_, panY_);
        if (stereo_ && onPanRightChanged) onPanRightChanged(panXR_, panYR_);
        repaint();
        return;
    }

    // Shift + right-click resets to default
    if (e.mods.isRightButtonDown() && e.mods.isShiftDown()) {
        panX_ = 0.0; panY_ = 1.0;
        panXR_ = 0.0; panYR_ = 1.0;
        if (onPanChanged) onPanChanged(panX_, panY_);
        if (stereo_ && onPanRightChanged) onPanRightChanged(panXR_, panYR_);
        repaint();
        return;
    }

    // Determine which dot to drag (in stereo mode, pick the closest)
    if (stereo_) {
        auto posL = panToPixel(panX_, panY_);
        auto posR = panToPixel(panXR_, panYR_);
        float distL = posL.getDistanceFrom(e.position);
        float distR = posR.getDistanceFrom(e.position);
        draggingRight_ = distR < distL;
    } else {
        draggingRight_ = false;
    }

    mouseDrag(e);
}

void SurroundPannerComponent::mouseDrag(const juce::MouseEvent& e) {
    auto [x, y] = pixelToPan(e.position.x, e.position.y);

    if (draggingRight_) {
        panXR_ = x; panYR_ = y;
        if (onPanRightChanged) onPanRightChanged(x, y);
    } else {
        panX_ = x; panY_ = y;
        if (onPanChanged) onPanChanged(x, y);
    }
    repaint();
}

}  // namespace triggerfish

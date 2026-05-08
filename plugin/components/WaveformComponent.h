#pragma once

#include "app_controller.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <utility>

namespace triggerfish {

class WaveformComponent : public juce::Component {
public:
    enum class AutomationLaneType {
        None,
        Volume,
        Stretch,
        PanPosition,
        PanFrontBack,
        PanRightPosition,
        PanRightFrontBack,
        Doppler
    };

    WaveformComponent();

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void setWaveform(const radium::LayerWaveformOverview& overview);
    void clearWaveform();
    void setPlayheadPosition(double normalizedPos); // -1 = no playhead
    void setCrossfadeSelectionMode(bool enabled) { crossfadeSelectionMode_ = enabled; }
    void setAutomationLaneVisible(bool visible);
    void setAutomationLaneType(AutomationLaneType type);
    void setAutomationFreehandDrawEnabled(bool enabled);
    void previewAutomationPointMove(std::size_t pointId, double normalizedTimeline, double value);
    std::optional<std::pair<double, double>> selectedEditRange() const { return editSelection_; }
    std::optional<std::pair<std::size_t, bool>> selectedFadeHandle() const { return selectedFadeHandle_; }
    std::vector<std::size_t> selectedClipIndices() const { return selectedClipIndices_; }
    void clearEditSelection();
    void clearSelectedFadeHandle();
    void clearSelectedClips();
    void clearSelectedAutomationSegment();

    std::function<void(double)> onAuditionStart;
    std::function<void(double, double)> onRegionCreated;
    std::function<void(std::size_t, double, double)> onRegionUpdated;
    std::function<void()> onRegionsCleared;
    std::function<void()> onEditGestureBegan;
    std::function<void(std::size_t, double)> onEditClipMoved;
    std::function<void(const std::vector<std::size_t>&, double)> onEditClipsMoved;
    std::function<void(std::size_t, bool, double)> onEditClipTrimmed;
    std::function<void(std::size_t, bool, double)> onEditClipFadeChanged;
    std::function<void(std::optional<std::size_t>)> onEditGestureFinished;
    std::function<void(const std::vector<std::size_t>&)> onGroupedEditGestureFinished;
    std::function<void(double, double)> onCrossfadeRequested;
    std::function<void()> onAutomationGestureBegan;
    std::function<std::optional<std::size_t>(double)> onAutomationPointCreated;
    std::function<void(std::size_t, double, double)> onAutomationPointMoved;
    std::function<void(std::size_t)> onAutomationPointDeleted;
    std::function<void()> onAutomationGestureFinished;
    std::function<void(std::optional<std::size_t>)> onAutomationSegmentSelected;
    std::function<void(const std::vector<std::pair<double, double>>&)> onAutomationFreehandDraw;
    std::function<void(double, double)> onViewRangeChanged; // (viewStart, viewEnd)

    double viewStart() const { return viewStart_; }
    double viewEnd() const { return viewEnd_; }

private:
    double xToNormalized(float x) const;
    float normalizedToX(double norm) const;
    double visualToSourceNormalized(double norm) const;
    double sourceToVisualNormalized(double norm) const;
    juce::Rectangle<float> automationLaneBounds() const;
    float automationValueToY(double value) const;
    double automationYToValue(float y) const;
    int findAutomationPoint(float x, float y) const;
    int findAutomationSegment(float x, float y) const;
    double currentAutomationValueAt(double normalizedTimeline) const;
    bool automationLaneEnabled() const;
    void appendAutomationFreehandSample(double sourceNorm, double value);
    int findRegionHandle(float x) const;
    int findEditableClipBody(double norm) const;
    int findEditableClipEdge(float x, bool& isLeftEdge) const;
    int findEditableFadeHandle(float x, bool& isFadeIn) const;
    bool canStartEditSelection(double norm) const;
    bool isClipSelected(std::size_t clipIndex) const;

    radium::LayerWaveformOverview overview_;
    bool hasData_ = false;

    // View range for zoom/pan
    double viewStart_ = 0.0;
    double viewEnd_ = 1.0;

    // Playhead position (normalized, -1 = not playing)
    double playheadPos_ = -1.0;

    // Drag state
    enum class DragMode {
        None,
        AuditionStart,
        CreateRegion,
        DragHandle,
        MoveClipCandidate,
        MoveClip,
        TrimClip,
        FadeClip,
        EditSelection,
        AutomationPoint,
        AutomationFreehand
    };
    DragMode dragMode_ = DragMode::None;
    double dragStartNorm_ = 0.0;
    int dragHandleRegion_ = -1;
    bool dragHandleIsEnd_ = false;
    int dragClipIndex_ = -1;
    bool dragClipIsLeftEdge_ = false;
    bool dragClipFadeIsIn_ = false;
    bool dragStarted_ = false;
    double dragClipOriginalStart_ = 0.0;
    std::optional<std::pair<double, double>> editSelection_;
    std::optional<std::pair<std::size_t, bool>> selectedFadeHandle_;
    std::vector<std::size_t> selectedClipIndices_;
    std::optional<std::size_t> selectedAutomationPoint_;
    std::optional<std::size_t> selectedAutomationSegmentLeftPointId_;
    bool editGestureChanged_ = false;
    bool crossfadeSelectionMode_ = false;
    bool automationLaneVisible_ = false;
    AutomationLaneType automationLaneType_ = AutomationLaneType::None;
    bool automationFreehandDrawEnabled_ = false;
    std::vector<std::pair<double, double>> automationFreehandStroke_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
};

}  // namespace triggerfish

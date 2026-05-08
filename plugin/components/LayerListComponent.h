#pragma once

#include "app_controller.h"
#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace triggerfish {

class LayerRowComponent : public juce::Component {
public:
    LayerRowComponent();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void update(const radium::VisibleLayerState& state);
    void setMiniPeaks(const std::vector<float>& peaksMax, const std::vector<float>& peaksMin,
                      const std::vector<float>& peaksRightMax = {},
                      const std::vector<float>& peaksRightMin = {});
    void setMiniRegions(const std::vector<radium::LayerWaveformOverview::AuthoredRegion>& regions);
    void setPlayheadPosition(double normalizedPos);

    std::function<void(std::size_t)> onSelect;
    std::function<void(std::size_t, bool)> onMuteToggle;
    std::function<void(std::size_t, bool)> onSoloToggle;
    std::function<void(std::size_t, double)> onGainChange;
    std::function<void(std::size_t)> onAutoSplit;
    std::function<void(std::size_t)> onClearAudio;
    std::function<void(std::size_t, bool)> onLockToggle;
    std::function<void(std::size_t, const juce::String&)> onRename;

private:
    class EditableNameLabel : public juce::Label {
    public:
        std::function<void(const juce::MouseEvent&)> onDoubleClick;

        void mouseDoubleClick(const juce::MouseEvent& event) override {
            if (onDoubleClick) onDoubleClick(event);
        }
    };

    radium::VisibleLayerState layerState_;
    juce::ToggleButton muteButton{"M"};
    juce::ToggleButton soloButton{"S"};
    triggerfish::FineTuneSlider gainSlider;
    EditableNameLabel nameLabel;
    juce::TextButton autoSplitButton_{"Split"};
    juce::TextButton clearAudioButton_{"-"};
    juce::ToggleButton lockButton_{"Lock"};
    bool editingName_ = false;
    std::vector<float> waveformPeaks_;
    std::vector<float> waveformPeaksMin_;
    std::vector<float> waveformPeaksRight_;
    std::vector<float> waveformPeaksRightMin_;
    std::vector<radium::LayerWaveformOverview::AuthoredRegion> miniRegions_;
    double playheadPos_ = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LayerRowComponent)
};

class LayerListComponent : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            public juce::ScrollBar::Listener {
public:
    LayerListComponent();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // ScrollBar::Listener
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void updateFromController(radium::AppController& controller);
    void updatePlayheadPositions(radium::AppController& controller);
    void setProjectName(const juce::String& name);

    std::function<void(std::size_t)> onLayerSelect;
    std::function<void(std::size_t, bool)> onMuteToggle;
    std::function<void(std::size_t, bool)> onSoloToggle;
    std::function<void(std::size_t, double)> onGainChange;
    std::function<void(std::size_t)> onAutoSplit;
    std::function<void(std::size_t)> onClearLayer;
    std::function<void(std::size_t, bool)> onLayerLockToggle;
    std::function<void(std::size_t, const juce::String&)> onLayerRename;
    std::function<void(std::size_t, const juce::String&)> onAudioDropped;

private:
    static constexpr int kVisibleRows = 5;
    std::vector<std::unique_ptr<LayerRowComponent>> rows_;
    juce::Label headerLabel;
    juce::Label infoLabel;
    juce::Label projectNameLabel;
    juce::ScrollBar scrollBar_{true}; // vertical
    int scrollOffset_ = 0;
    int totalLayers_ = 0;
    radium::AppController* lastController_ = nullptr;

    void ensureRowCount(std::size_t count);
    void applyScroll();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LayerListComponent)
};

}  // namespace triggerfish

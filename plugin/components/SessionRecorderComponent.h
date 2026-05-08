#pragma once

#include "app_controller.h"
#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>

namespace triggerfish {

class SessionRecorderComponent : public juce::Component,
                                  public juce::ListBoxModel,
                                  public juce::DragAndDropContainer {
public:
    SessionRecorderComponent();

    void paint(juce::Graphics&) override;
    void resized() override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override;

    // Refresh from controller state
    void updateFromController(radium::AppController& controller);

    // Called on timer to update recording waveform
    void updateRecordingPeaks(radium::StreamingMixer& mixer);
    void updatePunchPreview(radium::StreamingMixer& mixer, double cueStartNorm,
                            std::pair<double, double> punchRegion, double playheadNorm,
                            double takeDurationSeconds);
    void clearRecordingPreview();
    void setRecordingState(bool recording, bool punching);

    // Callbacks
    std::function<void(bool armed)> onNewToggle;
    std::function<void(std::size_t index)> onTakeSelect;
    std::function<void()> onExport;
    std::function<void(const std::string& newName)> onRename;
    std::function<void(std::size_t takeIndex, std::size_t layerIndex)> onDropOnLayer;
    std::function<juce::File(std::size_t takeIndex)> onGetTakeFile;
    std::function<void(std::size_t takeIndex)> onPlayTake;
    std::function<void(bool active)> onPunchToggle;
    std::function<void()> onStopTake;
    std::function<void(std::size_t takeIndex)> onDeleteTake;
    std::function<void(std::size_t takeIndex, double normPos)> onTakeScrub;

    // Set normalized playhead position (0..1) for take playback, or -1 to hide
    void setTakePlayhead(double normPos);
    std::optional<std::pair<double, double>> punchInRegion() const;
    void clearPunchInRegion();
    double punchCuePosition() const;
    void setPunchCuePosition(double normPos);
    double selectedTakeDurationSeconds() const;

private:
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    double xToWaveformNormalized(int x) const;

    juce::Label titleLabel_{"", "RECORDER"};
    juce::ToggleButton newButton_{"New"};
    juce::ToggleButton punchButton_{"Punch"};
    juce::ListBox takeList_{"Takes", this};
    juce::TextButton playButton_{"Play"};
    juce::TextButton exportButton_{"Export"};
    juce::TextButton renameButton_{"Rename"};

    std::vector<radium::SessionRecordingInfo> takes_;
    std::optional<std::size_t> selectedIndex_;
    bool isRecording_ = false;
    bool isPunching_ = false;

    // Real-time recording waveform peaks
    std::vector<std::vector<float>> recChannelPeaks_;
    std::vector<std::vector<float>> punchPreviewChannelPeaks_;
    double punchPreviewStartNorm_ = 0.0;
    double punchPreviewEndNorm_ = 0.0;

    // Selected take waveform peaks (decoded)
    std::vector<std::vector<float>> takeChannelPeaks_;
    std::size_t takePeaksTakeIndex_ = ~std::size_t(0);
    std::filesystem::path takePeaksCachedPath_;
    std::atomic<unsigned int> waveformLoadGeneration_{0};

    // Playhead for take playback
    double takePlayhead_ = -1.0;
    bool isTakePlaying_ = false;
    double cuePlayhead_ = 0.0;

    // Punch-in selection on the currently selected take waveform
    std::optional<std::pair<double, double>> punchInRegion_;
    std::size_t punchInTakeIndex_ = ~std::size_t(0);
    bool selectingPunchIn_ = false;
    double punchDragStartNorm_ = 0.0;
    double selectedTakeDurationSeconds_ = 0.0;

    // Drag state
    bool dragStarted_ = false;
    bool externalDragStarted_ = false;
    juce::Point<int> dragStartPos_;

    juce::Rectangle<int> getWaveformArea() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionRecorderComponent)
};

}  // namespace triggerfish

#pragma once

#include "PluginProcessor.h"
#include "LookAndFeel_Radium.h"
#include "components/ToolbarComponent.h"
#include "components/LayerListComponent.h"
#include "components/PianoKeyboardComponent.h"
#include "components/WaveformComponent.h"
#include "Vst3PluginScanner.h"
#include "components/Vst3InsertRack.h"
#include "components/SurroundPannerComponent.h"
#include "components/MasterControlsComponent.h"
#include "components/SessionRecorderComponent.h"
#include "components/StereoLevelMeterComponent.h"
#include "components/PictureWindow.h"
#include "components/LibrarySearchWindow.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <chrono>
#include <atomic>
#include <limits>
#include <map>
#include <unordered_map>

class TriggerfishEditor : public juce::AudioProcessorEditor,
                          public juce::Timer {
public:
    explicit TriggerfishEditor(TriggerfishProcessor&);
    ~TriggerfishEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void timerCallback() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;

private:
    struct DimOverlayComponent : public juce::Component {
        void paint(juce::Graphics& g) override {
            g.fillAll(juce::Colours::black.withAlpha(0.82f));
        }
    };

    enum class PlaybackTarget {
        FocusLayer,
        RecorderTake,
        Picture
    };

    enum class FocusModule {
        None,
        FocusTrack
    };

    enum class FocusAutomationTarget {
        Volume,
        Stretch,
        PanPosition,
        PanFrontBack,
        PanRightPosition,
        PanRightFrontBack,
        Doppler
    };

    void refreshUI();
    void refreshFocusWaveform();
    void invalidateFocusWaveformCache();
    void addAudioToSelectedLayer();
    void showDatabaseMenu();
    void createLibraryDatabase();
    void appendFolderToLibraryDatabase(const juce::String& libraryId);
    void renameLibraryDatabase(const juce::String& libraryId);
    void deleteLibraryDatabase(const juce::String& libraryId);
    void placeFileOnSelectedLayer(const juce::String& filePath);
    void toggleFocusTrackPlayback();
    void finalizePunchRecording();
    void refreshRecorderPictureControls();
    void setRecorderBusMode(radium::RecordBusMode mode);
    void syncRecorderSurroundModeFromSelection();
    std::optional<double> selectedTakePictureStartSeconds();
    void syncPictureToSelectedTake();
    void stopTakeOwnedPicturePlayback(bool rewindToStart);
    triggerfish::PictureWindow& ensurePictureWindow();
    triggerfish::LibrarySearchWindow& ensureLibrarySearchWindow();
    void showPictureWindow();
    void showLibrarySearchWindow();
    void loadVideoFile(const juce::File& file);
    void stopRecordingOwnedPicturePlayback(bool rewindToStart);
    void wirePluginSessions(bool resetProcessing = false);
    void clearLivePluginSessionPointers();
    void resetHostedPluginsForSessionChange();
    void ensureLayerHosts(std::size_t layerIndex);
    void ensureAuxHosts();
    void beginAsyncLayerHostLoad(std::size_t layerIndex);
    void beginAsyncAuxHostLoad();
    void captureLayerEditUndoSnapshot(std::size_t layerIndex);
    void clearLayerEditHistory(std::size_t layerIndex);
    void clearAllLayerEditHistory();
    bool undoLayerEdit();
    bool redoLayerEdit();
    void resetTrackAutomationWriteState();
    void showFocusAutomationMenu();
    void toggleFocusedModule(FocusModule module);
    void updateFocusedModuleButtons();
    juce::String currentAutomationTargetLabel() const;
    bool isCurrentAutomationTargetEnabled(std::size_t layer_index) const;
    bool resetCurrentAutomationTarget(std::size_t layer_index);
    bool removeCurrentAutomationTarget(std::size_t layer_index);
    bool enableAllPanAutomationTargets(std::size_t layer_index, bool is_stereo);
    bool resetAllPanAutomationTargets(std::size_t layer_index, bool is_stereo);
    bool removeAllPanAutomationTargets(std::size_t layer_index, bool is_stereo);
    void enableStandaloneMidiInputs();
    void refreshStandaloneMidiStatus();
    void refreshDopplerControls();
    void applyFreehandDopplerStroke(const std::vector<std::pair<double, double>>& strokePoints);
    void refreshVolumeRandomControls();
    void refreshPanRandomControls();
    void refreshStretchRandomControls();

    TriggerfishProcessor& processorRef;
    triggerfish::LookAndFeel_Radium lookAndFeel_;
    triggerfish::ToolbarComponent toolbar_;
    DimOverlayComponent dimOverlay_;
    triggerfish::LayerListComponent layerList_;
    triggerfish::PianoKeyboardComponent keyboard_;
    triggerfish::WaveformComponent waveform_;
    triggerfish::Vst3PluginScanner vst3Scanner_;
    triggerfish::Vst3InsertRack vst3Rack_;
    triggerfish::SurroundPannerComponent surroundPanner_;
    triggerfish::MasterControlsComponent masterControls_;
    triggerfish::SessionRecorderComponent sessionRecorder_;
    triggerfish::Vst3InsertRack auxVst3Rack_;
    juce::Label auxTitleLabel_{"", "REC AUX"};
    juce::ComboBox recorderBusModeBox_;
    juce::Label auxGainLabel_{"", "Volume"};
    triggerfish::FineTuneSlider auxGain_;
    juce::Label auxBassGainLabel_{"", "Bass dB"};
    triggerfish::FineTuneSlider auxBassGain_;
    triggerfish::StereoLevelMeterComponent auxMeter_;
    juce::TextButton focusTrackFocusButton_{"Focus"};
    juce::TextButton addAutomationButton_{"Automation"};
    juce::Label focusAutomationTargetLabel_{"", "VOL"};
    juce::GroupComponent volumeRandomControlsPanel_{"", "Volume Randomize"};
    juce::ToggleButton volumeRandomizeToggle_{"Randomize"};
    juce::Label volumeRandomLoudestLabel_{"", "Loudest"};
    triggerfish::FineTuneSlider volumeRandomLoudest_;
    juce::Label volumeRandomQuietestLabel_{"", "Quietest"};
    triggerfish::FineTuneSlider volumeRandomQuietest_;
    juce::Label volumeRandomPeriodLongestLabel_{"", "Wave Long"};
    triggerfish::FineTuneSlider volumeRandomPeriodLongest_;
    juce::Label volumeRandomPeriodShortestLabel_{"", "Wave Short"};
    triggerfish::FineTuneSlider volumeRandomPeriodShortest_;
    juce::Label volumeRandomSmoothingLabel_{"", "Smoothing"};
    triggerfish::FineTuneSlider volumeRandomSmoothing_;
    juce::GroupComponent panRandomControlsPanel_{"", "Pan Randomize"};
    juce::ToggleButton panRandomizeToggle_{"Randomize"};
    juce::Label panRandomLeftLabel_{"", "Left"};
    triggerfish::FineTuneSlider panRandomLeft_;
    juce::Label panRandomRightLabel_{"", "Right"};
    triggerfish::FineTuneSlider panRandomRight_;
    juce::Label panRandomFrontLabel_{"", "Front"};
    triggerfish::FineTuneSlider panRandomFront_;
    juce::Label panRandomBackLabel_{"", "Back"};
    triggerfish::FineTuneSlider panRandomBack_;
    juce::Label panRandomSpeedLabel_{"", "Speed"};
    triggerfish::FineTuneSlider panRandomSpeed_;
    juce::Label panRandomSmoothingLabel_{"", "Smoothing"};
    triggerfish::FineTuneSlider panRandomSmoothing_;
    juce::GroupComponent stretchRandomControlsPanel_{"", "Stretch Randomize"};
    juce::ToggleButton stretchRandomizeToggle_{"Randomize"};
    juce::Label stretchRandomLowestLabel_{"", "Lowest %"};
    triggerfish::FineTuneSlider stretchRandomLowest_;
    juce::Label stretchRandomHighestLabel_{"", "Highest %"};
    triggerfish::FineTuneSlider stretchRandomHighest_;
    juce::Label stretchRandomSpeedLabel_{"", "Speed"};
    triggerfish::FineTuneSlider stretchRandomSpeed_;
    juce::Label stretchRandomSmoothingLabel_{"", "Smoothing"};
    triggerfish::FineTuneSlider stretchRandomSmoothing_;
    juce::GroupComponent dopplerControlsPanel_{"", "Doppler"};
    juce::Label dopplerEdgeGainLabel_{"", "Edge Vol dB"};
    triggerfish::FineTuneSlider dopplerEdgeGain_;
    juce::Label dopplerCenterGainLabel_{"", "Center Vol dB"};
    triggerfish::FineTuneSlider dopplerCenterGain_;
    juce::Label dopplerEdgePitchLabel_{"", "Edge Pitch st"};
    triggerfish::FineTuneSlider dopplerEdgePitch_;
    juce::Label dopplerCenterPitchLabel_{"", "Center Pitch st"};
    triggerfish::FineTuneSlider dopplerCenterPitch_;
    juce::ToggleButton dopplerDrawModeToggle_{"Draw"};
    juce::Label dopplerCurveTypeLabel_{"", "Segment Curve"};
    juce::ComboBox dopplerCurveType_;
    juce::Label dopplerCurveAmountLabel_{"", "Curve Amount"};
    triggerfish::FineTuneSlider dopplerCurveAmount_;
    juce::GroupComponent layerEqPanel_{"", "Layer EQ"};
    juce::Label layerEqLowLabel_{"", "Low"};
    triggerfish::FineTuneSlider layerEqLow_;
    juce::Label layerEqMidLabel_{"", "Mid"};
    triggerfish::FineTuneSlider layerEqMid_;
    juce::Label layerEqHighLabel_{"", "High"};
    triggerfish::FineTuneSlider layerEqHigh_;
    juce::Label recorderPictureTimeLabel_{"", "00:00.00 / 00:00.00"};
    juce::Slider recorderPictureTimeline_;
    juce::Label recorderPictureVolumeLabel_{"", "Volume"};
    triggerfish::FineTuneSlider recorderPictureVolume_;
    juce::TextButton vstFolderButton_{"Set VST3 Folder..."};
    std::unique_ptr<triggerfish::PictureWindow> pictureWindow_;
    std::unique_ptr<triggerfish::LibrarySearchWindow> librarySearchWindow_;
    bool isStandalone_ = false;
    bool isTakePlaying_ = false;
    bool pictureTransportOwnedByRecorder_ = false;
    bool pictureTransportOwnedByTakePlayback_ = false;
    PlaybackTarget playbackTarget_ = PlaybackTarget::FocusLayer;
    bool newRecordingActive_ = false;
    bool punchInActive_ = false;
    bool suppressRecorderPictureControls_ = false;
    bool suppressRecorderBusModeBox_ = false;
    bool suppressVolumeRandomControls_ = false;
    bool suppressPanRandomControls_ = false;
    bool suppressStretchRandomControls_ = false;
    bool suppressDopplerControls_ = false;
    bool suppressDopplerSegmentControls_ = false;
    FocusModule focusedModule_ = FocusModule::None;
    int standaloneMidiPollCounter_ = 0;
    std::uint64_t lastMidiActivityCounter_ = 0;
    std::chrono::steady_clock::time_point lastMidiActivityAt_{};
    double recorderPictureCueSeconds_ = 0.0;
    double punchInCueStart_ = 0.0;
    double punchInTakeDurationSeconds_ = 0.0;
    std::optional<std::pair<double, double>> activePunchRegion_;
    std::chrono::steady_clock::time_point punchInStartedAt_{};
    bool trackAutomationWriteActive_ = false;
    std::optional<std::size_t> trackAutomationWriteLayerIndex_;
    std::optional<FocusAutomationTarget> trackAutomationWriteTarget_;
    std::optional<double> trackAutomationWriteOriginalGain_;
    std::optional<double> trackAutomationWriteOriginalStretch_;
    std::optional<double> trackAutomationWriteOriginalPanX_;
    std::optional<double> trackAutomationWriteOriginalPanY_;
    std::optional<double> trackAutomationWriteOriginalPanXRight_;
    std::optional<double> trackAutomationWriteOriginalPanYRight_;
    std::optional<std::size_t> selectedDopplerSegmentLeftPointId_;
    FocusAutomationTarget focusAutomationTarget_ = FocusAutomationTarget::Volume;

    // Per-layer plugin host instances — persist across layer switches.
    std::map<std::size_t, triggerfish::Vst3InsertRack::HostArray> layerHosts_;
    triggerfish::Vst3InsertRack::HostArray auxHosts_;
    std::atomic<std::size_t> pendingLayerHostLoad_{std::numeric_limits<std::size_t>::max()};
    std::atomic<bool> pendingAuxHostLoad_{false};
    std::atomic<std::uint64_t> hostLoadGeneration_{0};
    bool focusWaveformDirty_ = true;
    std::optional<std::size_t> cachedFocusWaveformLayerIndex_;
    std::uint64_t cachedFocusWaveformRevision_ = 0;
    int cachedFocusWaveformWidth_ = -1;
    bool focusLayoutDirty_ = true;
    FocusModule cachedLayoutFocusedModule_ = FocusModule::None;
    FocusAutomationTarget cachedLayoutAutomationTarget_ = FocusAutomationTarget::Volume;
    std::unordered_map<std::size_t, std::vector<radium::AppController::LayerEditState>> layerEditUndoHistory_;
    std::unordered_map<std::size_t, std::vector<radium::AppController::LayerEditState>> layerEditRedoHistory_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TriggerfishEditor)
};

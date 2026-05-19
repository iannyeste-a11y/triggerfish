#include "PluginEditor.h"
#include <cctype>
#include <cmath>
#include <filesystem>
#include <thread>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace {

double dopplerGainMultiplier(double dopplerValue, const radium::DopplerSettings& settings) {
    const double magnitude = std::clamp(std::abs(dopplerValue), 0.0, 1.0);
    const double edgeDb = std::clamp(settings.edge_gain_db, -70.0, 0.0);
    const double centerDb = std::clamp(settings.center_gain_db, -24.0, 12.0);
    const double falloffDb = centerDb + (edgeDb - centerDb) * std::pow(magnitude, 2.2);
    return std::pow(10.0, falloffDb / 20.0);
}

double dopplerPitchRatio(double dopplerValue, const radium::DopplerSettings& settings) {
    const double magnitude = std::clamp(std::abs(dopplerValue), 0.0, 1.0);
    const double edgeSemitoneShift = std::clamp(settings.edge_pitch_semitones, -24.0, 0.0);
    const double centerSemitoneShift = std::clamp(settings.center_pitch_semitones, 0.0, 24.0);
    const double semitoneOffset =
        edgeSemitoneShift + (1.0 - magnitude) * (centerSemitoneShift - edgeSemitoneShift);
    return std::pow(2.0, semitoneOffset / 12.0);
}

class LibraryIndexProgressTask : public juce::ThreadWithProgressWindow {
public:
    using Work = std::function<bool(const triggerfish::LibraryDatabase::ProgressCallback&, juce::String&)>;
    using Finished = std::function<void(bool success, const juce::String& error)>;

    LibraryIndexProgressTask(const juce::String& title, Work work, Finished finished)
        : juce::ThreadWithProgressWindow(title, true, false),
          work_(std::move(work)),
          finished_(std::move(finished)) {}

    void run() override {
        setProgress(-1.0);
        setStatusMessage("Preparing...");
        success_ = work_([this](double progress, const juce::String& status) {
            setProgress(progress);
            setStatusMessage(status);
        }, error_);
    }

    bool success() const { return success_; }
    const juce::String& error() const { return error_; }

    void threadComplete(bool /*userPressedCancel*/) override {
        if (finished_) {
            finished_(success_, error_);
        }
        delete this;
    }

private:
    Work work_;
    Finished finished_;
    bool success_ = false;
    juce::String error_;
};

}  // namespace

TriggerfishEditor::TriggerfishEditor(TriggerfishProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p), vst3Rack_(vst3Scanner_), auxVst3Rack_(vst3Scanner_) {
    setLookAndFeel(&lookAndFeel_);
    setSize(900, 800);
    setResizable(true, true);
    setResizeLimits(700, 500, 1600, 1200);
    setWantsKeyboardFocus(true);
    addMouseListener(this, true);

    addAndMakeVisible(toolbar_);
    addAndMakeVisible(dimOverlay_);
    addAndMakeVisible(layerList_);
    addAndMakeVisible(waveform_);
    addAndMakeVisible(vst3Rack_);
    addAndMakeVisible(surroundPanner_);
    addAndMakeVisible(masterControls_);
    addAndMakeVisible(focusTrackFocusButton_);
    addAndMakeVisible(addAutomationButton_);
    addAndMakeVisible(focusAutomationTargetLabel_);
    addAndMakeVisible(volumeRandomControlsPanel_);
    addAndMakeVisible(volumeRandomizeToggle_);
    addAndMakeVisible(volumeRandomLoudestLabel_);
    addAndMakeVisible(volumeRandomLoudest_);
    addAndMakeVisible(volumeRandomQuietestLabel_);
    addAndMakeVisible(volumeRandomQuietest_);
    addAndMakeVisible(volumeRandomPeriodLongestLabel_);
    addAndMakeVisible(volumeRandomPeriodLongest_);
    addAndMakeVisible(volumeRandomPeriodShortestLabel_);
    addAndMakeVisible(volumeRandomPeriodShortest_);
    addAndMakeVisible(volumeRandomSmoothingLabel_);
    addAndMakeVisible(volumeRandomSmoothing_);
    addAndMakeVisible(panRandomControlsPanel_);
    addAndMakeVisible(panRandomizeToggle_);
    addAndMakeVisible(panRandomLeftLabel_);
    addAndMakeVisible(panRandomLeft_);
    addAndMakeVisible(panRandomRightLabel_);
    addAndMakeVisible(panRandomRight_);
    addAndMakeVisible(panRandomFrontLabel_);
    addAndMakeVisible(panRandomFront_);
    addAndMakeVisible(panRandomBackLabel_);
    addAndMakeVisible(panRandomBack_);
    addAndMakeVisible(panRandomSpeedLabel_);
    addAndMakeVisible(panRandomSpeed_);
    addAndMakeVisible(panRandomSmoothingLabel_);
    addAndMakeVisible(panRandomSmoothing_);
    addAndMakeVisible(stretchRandomControlsPanel_);
    addAndMakeVisible(stretchRandomizeToggle_);
    addAndMakeVisible(stretchRandomLowestLabel_);
    addAndMakeVisible(stretchRandomLowest_);
    addAndMakeVisible(stretchRandomHighestLabel_);
    addAndMakeVisible(stretchRandomHighest_);
    addAndMakeVisible(stretchRandomSpeedLabel_);
    addAndMakeVisible(stretchRandomSpeed_);
    addAndMakeVisible(stretchRandomSmoothingLabel_);
    addAndMakeVisible(stretchRandomSmoothing_);
    addAndMakeVisible(dopplerControlsPanel_);
    addAndMakeVisible(dopplerEdgeGainLabel_);
    addAndMakeVisible(dopplerEdgeGain_);
    addAndMakeVisible(dopplerCenterGainLabel_);
    addAndMakeVisible(dopplerCenterGain_);
    addAndMakeVisible(dopplerEdgePitchLabel_);
    addAndMakeVisible(dopplerEdgePitch_);
    addAndMakeVisible(dopplerCenterPitchLabel_);
    addAndMakeVisible(dopplerCenterPitch_);
    addAndMakeVisible(dopplerDrawModeToggle_);
    addAndMakeVisible(dopplerCurveTypeLabel_);
    addAndMakeVisible(dopplerCurveType_);
    addAndMakeVisible(dopplerCurveAmountLabel_);
    addAndMakeVisible(dopplerCurveAmount_);
    addAndMakeVisible(layerEqPanel_);
    addAndMakeVisible(layerEqLowLabel_);
    addAndMakeVisible(layerEqLow_);
    addAndMakeVisible(layerEqMidLabel_);
    addAndMakeVisible(layerEqMid_);
    addAndMakeVisible(layerEqHighLabel_);
    addAndMakeVisible(layerEqHigh_);

    auto configureLayerEqSlider = [](juce::Label& label, triggerfish::FineTuneSlider& slider) {
        label.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
        label.setFont(juce::FontOptions(11.0f));
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 18);
        slider.setRange(-24.0, 12.0, 0.1);
        slider.setValue(0.0, juce::dontSendNotification);
        slider.setCtrlClickResetValue(0.0);
        slider.setColour(juce::Slider::trackColourId, triggerfish::colours::accentFocus);
        slider.textFromValueFunction = [](double value) {
            return juce::String(value, 1) + " dB";
        };
    };
    configureLayerEqSlider(layerEqLowLabel_, layerEqLow_);
    configureLayerEqSlider(layerEqMidLabel_, layerEqMid_);
    configureLayerEqSlider(layerEqHighLabel_, layerEqHigh_);

    auto pushLayerEqChange = [this] {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }
        auto fx = processorRef.controller().layer_effect_state(*sel);
        if (!fx.has_value()) {
            return;
        }
        fx->eq_low_gain_db = layerEqLow_.getValue();
        fx->eq_mid_gain_db = layerEqMid_.getValue();
        fx->eq_high_gain_db = layerEqHigh_.getValue();
        processorRef.controller().set_layer_effect_state(*sel, *fx);
        processorRef.controller().push_live_layer_eq(*sel);
    };
    layerEqLow_.onValueChange = pushLayerEqChange;
    layerEqMid_.onValueChange = pushLayerEqChange;
    layerEqHigh_.onValueChange = pushLayerEqChange;

    auxTitleLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textPrimary);
    auxTitleLabel_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    addAndMakeVisible(auxTitleLabel_);

    recorderBusModeBox_.addItem("ST", 1);
    recorderBusModeBox_.addItem("5.0", 2);
    recorderBusModeBox_.addItem("5.1", 3);
    recorderBusModeBox_.addItem("7.0", 4);
    recorderBusModeBox_.addItem("7.1", 5);
    recorderBusModeBox_.setSelectedId(1, juce::dontSendNotification);
    recorderBusModeBox_.setTooltip("Choose stereo, 5.0, 5.1, 7.0, or 7.1 recorder monitoring/recording");
    recorderBusModeBox_.onChange = [this] {
        if (suppressRecorderBusModeBox_) {
            return;
        }
        radium::RecordBusMode mode = radium::RecordBusMode::Stereo;
        if (recorderBusModeBox_.getSelectedId() == 2) {
            mode = radium::RecordBusMode::Surround50;
        } else if (recorderBusModeBox_.getSelectedId() == 3) {
            mode = radium::RecordBusMode::Surround51;
        } else if (recorderBusModeBox_.getSelectedId() == 4) {
            mode = radium::RecordBusMode::Surround70;
        } else if (recorderBusModeBox_.getSelectedId() == 5) {
            mode = radium::RecordBusMode::Surround71;
        }
        setRecorderBusMode(mode);
    };
    addAndMakeVisible(recorderBusModeBox_);

    auxGainLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
    auxGainLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(auxGainLabel_);

    auxGain_.setSliderStyle(juce::Slider::LinearHorizontal);
    auxGain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    auxGain_.setRange(0.0, 2.0, 0.01);
    auxGain_.setValue(1.0, juce::dontSendNotification);
    auxGain_.setCtrlClickResetValue(1.0);
    auxGain_.setColour(juce::Slider::trackColourId, triggerfish::colours::accentRecord);
    auxGain_.textFromValueFunction = [](double value) {
        return juce::String(value, 2);
    };
    auxGain_.onValueChange = [this] {
        processorRef.controller().set_aux_gain(auxGain_.getValue());
    };
    addAndMakeVisible(auxGain_);

    auxBassGainLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
    auxBassGainLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(auxBassGainLabel_);

    auxBassGain_.setSliderStyle(juce::Slider::LinearHorizontal);
    auxBassGain_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 18);
    auxBassGain_.setRange(-24.0, 12.0, 0.1);
    auxBassGain_.setValue(0.0, juce::dontSendNotification);
    auxBassGain_.setCtrlClickResetValue(0.0);
    auxBassGain_.setColour(juce::Slider::trackColourId, triggerfish::colours::accentRecord);
    auxBassGain_.textFromValueFunction = [](double value) {
        return juce::String(value, 1) + " dB";
    };
    auxBassGain_.onValueChange = [this] {
        processorRef.controller().set_aux_bass_gain_db(auxBassGain_.getValue());
    };
    addAndMakeVisible(auxBassGain_);

    addAndMakeVisible(auxMeter_);
    auxVst3Rack_.setAccentColour(triggerfish::colours::accentRecord);
    auxVst3Rack_.setLayoutMetrics(14, 1);
    addAndMakeVisible(auxVst3Rack_);

    addAndMakeVisible(sessionRecorder_);
    recorderPictureTimeLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
    recorderPictureTimeLabel_.setFont(juce::FontOptions(11.0f));
    recorderPictureTimeLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(recorderPictureTimeLabel_);

    recorderPictureTimeline_.setSliderStyle(juce::Slider::LinearHorizontal);
    recorderPictureTimeline_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    recorderPictureTimeline_.setRange(0.0, 1.0, 0.0);
    recorderPictureTimeline_.setColour(juce::Slider::trackColourId, triggerfish::colours::accentRecord);
    recorderPictureTimeline_.onValueChange = [this] {
        if (suppressRecorderPictureControls_) {
            return;
        }
        playbackTarget_ = PlaybackTarget::Picture;
        recorderPictureCueSeconds_ = recorderPictureTimeline_.getValue();
        if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
            pictureWindow_->content().seek(recorderPictureCueSeconds_);
        }
        refreshRecorderPictureControls();
    };
    addAndMakeVisible(recorderPictureTimeline_);

    recorderPictureVolumeLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
    recorderPictureVolumeLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(recorderPictureVolumeLabel_);

    recorderPictureVolume_.setSliderStyle(juce::Slider::LinearHorizontal);
    recorderPictureVolume_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 20);
    recorderPictureVolume_.setRange(0.0, 1.0, 0.01);
    recorderPictureVolume_.setValue(1.0, juce::dontSendNotification);
    recorderPictureVolume_.setCtrlClickResetValue(1.0);
    recorderPictureVolume_.setColour(juce::Slider::trackColourId, triggerfish::colours::accentRecord);
    recorderPictureVolume_.textFromValueFunction = [](double value) {
        return juce::String(juce::roundToInt(value * 100.0)) + "%";
    };
    recorderPictureVolume_.onValueChange = [this] {
        if (suppressRecorderPictureControls_) {
            return;
        }
        playbackTarget_ = PlaybackTarget::Picture;
        ensurePictureWindow().content().setVolume(recorderPictureVolume_.getValue());
        refreshRecorderPictureControls();
    };
    addAndMakeVisible(recorderPictureVolume_);
    dimOverlay_.setVisible(false);

    isStandalone_ = (processorRef.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    toolbar_.setMidiStatusVisible(isStandalone_);
    if (isStandalone_) {
        toolbar_.setProjectName("Untitled Project");
        layerList_.setProjectName("Untitled Project");
        juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<TriggerfishEditor>(this)] {
            if (safeThis != nullptr) {
                safeThis->enableStandaloneMidiInputs();
                safeThis->refreshStandaloneMidiStatus();
            }
        });
    }

    addAndMakeVisible(keyboard_);
    vst3Rack_.setAccentColour(triggerfish::colours::accentFocus);

    // Toolbar callbacks
    toolbar_.onAddAudio = [this] { addAudioToSelectedLayer(); };
    toolbar_.onDatabaseMenu = [this] { showDatabaseMenu(); };
    toolbar_.onPicture = [this] { showPictureWindow(); };
    toolbar_.onSearch = [this] { showLibrarySearchWindow(); };
    toolbar_.onMasterGainChange = [this](float gain) {
        processorRef.setMasterGain(gain);
    };
    focusTrackFocusButton_.onClick = [this] { toggleFocusedModule(FocusModule::FocusTrack); };
    addAutomationButton_.onClick = [this] { showFocusAutomationMenu(); };
    updateFocusedModuleButtons();
    focusAutomationTargetLabel_.setColour(juce::Label::textColourId, triggerfish::colours::textPrimary);
    focusAutomationTargetLabel_.setColour(juce::Label::backgroundColourId, triggerfish::colours::panel);
    focusAutomationTargetLabel_.setColour(juce::Label::outlineColourId, triggerfish::colours::accentFocus.withAlpha(0.75f));
    focusAutomationTargetLabel_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    focusAutomationTargetLabel_.setJustificationType(juce::Justification::centred);
    focusAutomationTargetLabel_.setBorderSize(juce::BorderSize<int>(1, 8, 1, 8));

    auto configureDopplerLabel = [](juce::Label& label) {
        label.setColour(juce::Label::textColourId, triggerfish::colours::textDim);
        label.setFont(juce::FontOptions(11.0f));
    };
    configureDopplerLabel(volumeRandomLoudestLabel_);
    configureDopplerLabel(volumeRandomQuietestLabel_);
    configureDopplerLabel(volumeRandomPeriodLongestLabel_);
    configureDopplerLabel(volumeRandomPeriodShortestLabel_);
    configureDopplerLabel(volumeRandomSmoothingLabel_);
    configureDopplerLabel(panRandomLeftLabel_);
    configureDopplerLabel(panRandomRightLabel_);
    configureDopplerLabel(panRandomFrontLabel_);
    configureDopplerLabel(panRandomBackLabel_);
    configureDopplerLabel(panRandomSpeedLabel_);
    configureDopplerLabel(panRandomSmoothingLabel_);
    configureDopplerLabel(stretchRandomLowestLabel_);
    configureDopplerLabel(stretchRandomHighestLabel_);
    configureDopplerLabel(stretchRandomSpeedLabel_);
    configureDopplerLabel(stretchRandomSmoothingLabel_);
    configureDopplerLabel(dopplerEdgeGainLabel_);
    configureDopplerLabel(dopplerCenterGainLabel_);
    configureDopplerLabel(dopplerEdgePitchLabel_);
    configureDopplerLabel(dopplerCenterPitchLabel_);
    configureDopplerLabel(dopplerCurveTypeLabel_);
    configureDopplerLabel(dopplerCurveAmountLabel_);
    dopplerDrawModeToggle_.setColour(juce::ToggleButton::textColourId, triggerfish::colours::textPrimary);
    dopplerDrawModeToggle_.setTooltip("Draw a Doppler path freehand and convert it to automation points.");
    dopplerDrawModeToggle_.onClick = [this] {
        waveform_.setAutomationFreehandDrawEnabled(
            focusAutomationTarget_ == FocusAutomationTarget::Doppler &&
            dopplerDrawModeToggle_.getToggleState());
    };

    auto configureDopplerSlider = [](triggerfish::FineTuneSlider& slider,
                                     double minValue,
                                     double maxValue,
                                     double defaultValue,
                                     const juce::Colour& colour,
                                     std::function<juce::String(double)> formatter) {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        slider.setRange(minValue, maxValue, 0.01);
        slider.setValue(defaultValue, juce::dontSendNotification);
        slider.setColour(juce::Slider::trackColourId, colour);
        slider.textFromValueFunction = std::move(formatter);
    };

    configureDopplerSlider(
        dopplerEdgeGain_, -70.0, 0.0, -24.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " dB"; });
    configureDopplerSlider(
        dopplerCenterGain_, -24.0, 12.0, 0.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " dB"; });
    configureDopplerSlider(
        dopplerEdgePitch_, -24.0, 0.0, -4.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " st"; });
    configureDopplerSlider(
        dopplerCenterPitch_, 0.0, 24.0, 4.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " st"; });
    configureDopplerSlider(
        dopplerCurveAmount_, 0.0, 1.0, 0.5, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });

    configureDopplerSlider(
        volumeRandomLoudest_, 0.0, 12.0, 0.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " dB"; });
    configureDopplerSlider(
        volumeRandomQuietest_, -70.0, 0.0, -12.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 1) + " dB"; });
    configureDopplerSlider(
        volumeRandomPeriodLongest_, 0.02, 20.0, 2.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, value < 1.0 ? 2 : 1) + " s"; });
    configureDopplerSlider(
        volumeRandomPeriodShortest_, 0.02, 20.0, 0.35, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, value < 1.0 ? 2 : 1) + " s"; });
    configureDopplerSlider(
        volumeRandomSmoothing_, 0.0, 1.0, 0.7, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });
    volumeRandomizeToggle_.setColour(juce::ToggleButton::textColourId, triggerfish::colours::textPrimary);

    configureDopplerSlider(
        panRandomLeft_, -1.0, 0.0, -1.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 2); });
    configureDopplerSlider(
        panRandomRight_, 0.0, 1.0, 1.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 2); });
    configureDopplerSlider(
        panRandomFront_, -1.0, 1.0, 1.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 2); });
    configureDopplerSlider(
        panRandomBack_, -1.0, 1.0, -1.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(value, 2); });
    configureDopplerSlider(
        panRandomSpeed_, 0.0, 1.0, 0.5, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });
    configureDopplerSlider(
        panRandomSmoothing_, 0.0, 1.0, 0.7, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });
    panRandomizeToggle_.setColour(juce::ToggleButton::textColourId, triggerfish::colours::textPrimary);
    configureDopplerSlider(
        stretchRandomLowest_, 1.0, 800.0, 100.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value)) + "%"; });
    configureDopplerSlider(
        stretchRandomHighest_, 1.0, 800.0, 100.0, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value)) + "%"; });
    configureDopplerSlider(
        stretchRandomSpeed_, 0.0, 1.0, 0.5, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });
    configureDopplerSlider(
        stretchRandomSmoothing_, 0.0, 1.0, 0.7, triggerfish::colours::accentFocus,
        [](double value) { return juce::String(std::round(value * 100.0)) + "%"; });
    stretchRandomizeToggle_.setColour(juce::ToggleButton::textColourId, triggerfish::colours::textPrimary);

    auto volumeRandomSettingsChanged = [this] {
        if (suppressVolumeRandomControls_) {
            return;
        }
        const auto selectedLayer = processorRef.controller().selected_layer_index();
        if (!selectedLayer.has_value()) {
            return;
        }
        radium::VolumeRandomSettings settings;
        settings.enabled = volumeRandomizeToggle_.getToggleState();
        settings.loudest_db = volumeRandomLoudest_.getValue();
        settings.quietest_db = volumeRandomQuietest_.getValue();
        settings.period_longest_seconds = volumeRandomPeriodLongest_.getValue();
        settings.period_shortest_seconds = volumeRandomPeriodShortest_.getValue();
        settings.smoothing = volumeRandomSmoothing_.getValue();
        if (processorRef.controller().set_layer_volume_random_settings(*selectedLayer, settings)) {
            refreshVolumeRandomControls();
        }
    };
    volumeRandomizeToggle_.onClick = volumeRandomSettingsChanged;
    volumeRandomLoudest_.onValueChange = volumeRandomSettingsChanged;
    volumeRandomQuietest_.onValueChange = volumeRandomSettingsChanged;
    volumeRandomPeriodLongest_.onValueChange = volumeRandomSettingsChanged;
    volumeRandomPeriodShortest_.onValueChange = volumeRandomSettingsChanged;
    volumeRandomSmoothing_.onValueChange = volumeRandomSettingsChanged;

    auto panRandomSettingsChanged = [this] {
        if (suppressPanRandomControls_) {
            return;
        }
        const auto selectedLayer = processorRef.controller().selected_layer_index();
        if (!selectedLayer.has_value()) {
            return;
        }
        radium::PanRandomSettings settings;
        settings.enabled = panRandomizeToggle_.getToggleState();
        settings.farthest_left = panRandomLeft_.getValue();
        settings.farthest_right = panRandomRight_.getValue();
        settings.farthest_front = panRandomFront_.getValue();
        settings.farthest_back = panRandomBack_.getValue();
        settings.speed = panRandomSpeed_.getValue();
        settings.smoothing = panRandomSmoothing_.getValue();
        if (processorRef.controller().set_layer_pan_random_settings(*selectedLayer, settings)) {
            refreshPanRandomControls();
        }
    };
    panRandomizeToggle_.onClick = panRandomSettingsChanged;
    panRandomLeft_.onValueChange = panRandomSettingsChanged;
    panRandomRight_.onValueChange = panRandomSettingsChanged;
    panRandomFront_.onValueChange = panRandomSettingsChanged;
    panRandomBack_.onValueChange = panRandomSettingsChanged;
    panRandomSpeed_.onValueChange = panRandomSettingsChanged;
    panRandomSmoothing_.onValueChange = panRandomSettingsChanged;

    auto stretchRandomSettingsChanged = [this] {
        if (suppressStretchRandomControls_) {
            return;
        }
        const auto selectedLayer = processorRef.controller().selected_layer_index();
        if (!selectedLayer.has_value()) {
            return;
        }
        radium::StretchRandomSettings settings;
        settings.enabled = stretchRandomizeToggle_.getToggleState();
        settings.lowest_percent = stretchRandomLowest_.getValue();
        settings.highest_percent = stretchRandomHighest_.getValue();
        settings.speed = stretchRandomSpeed_.getValue();
        settings.smoothing = stretchRandomSmoothing_.getValue();
        if (processorRef.controller().set_layer_stretch_random_settings(*selectedLayer, settings)) {
            refreshStretchRandomControls();
        }
    };
    stretchRandomizeToggle_.onClick = stretchRandomSettingsChanged;
    stretchRandomLowest_.onValueChange = stretchRandomSettingsChanged;
    stretchRandomHighest_.onValueChange = stretchRandomSettingsChanged;
    stretchRandomSpeed_.onValueChange = stretchRandomSettingsChanged;
    stretchRandomSmoothing_.onValueChange = stretchRandomSettingsChanged;

    dopplerCurveType_.addItem("Linear", 1);
    dopplerCurveType_.addItem("S-Curve", 2);
    dopplerCurveType_.addItem("Convex", 3);
    dopplerCurveType_.addItem("Concave", 4);

    auto dopplerSettingsChanged = [this] {
        if (suppressDopplerControls_) {
            return;
        }
        const auto selectedLayer = processorRef.controller().selected_layer_index();
        if (!selectedLayer.has_value()) {
            return;
        }
        radium::DopplerSettings settings;
        settings.edge_gain_db = dopplerEdgeGain_.getValue();
        settings.center_gain_db = dopplerCenterGain_.getValue();
        settings.edge_pitch_semitones = dopplerEdgePitch_.getValue();
        settings.center_pitch_semitones = dopplerCenterPitch_.getValue();
        if (processorRef.controller().set_layer_doppler_settings(*selectedLayer, settings)) {
            refreshUI();
        }
    };
    dopplerEdgeGain_.onValueChange = dopplerSettingsChanged;
    dopplerCenterGain_.onValueChange = dopplerSettingsChanged;
    dopplerEdgePitch_.onValueChange = dopplerSettingsChanged;
    dopplerCenterPitch_.onValueChange = dopplerSettingsChanged;

    auto dopplerSegmentShapeChanged = [this] {
        if (suppressDopplerSegmentControls_) {
            return;
        }
        const auto selectedLayer = processorRef.controller().selected_layer_index();
        if (!selectedLayer.has_value() || !selectedDopplerSegmentLeftPointId_.has_value()) {
            return;
        }
        radium::DopplerCurveType curveType = radium::DopplerCurveType::Linear;
        switch (dopplerCurveType_.getSelectedId()) {
            case 2:
                curveType = radium::DopplerCurveType::SCurve;
                break;
            case 3:
                curveType = radium::DopplerCurveType::Convex;
                break;
            case 4:
                curveType = radium::DopplerCurveType::Concave;
                break;
            case 1:
            default:
                curveType = radium::DopplerCurveType::Linear;
                break;
        }
        if (processorRef.controller().set_layer_doppler_segment_shape(
                *selectedLayer,
                *selectedDopplerSegmentLeftPointId_,
                curveType,
                dopplerCurveAmount_.getValue())) {
            refreshUI();
        }
    };
    dopplerCurveType_.onChange = dopplerSegmentShapeChanged;
    dopplerCurveAmount_.onValueChange = dopplerSegmentShapeChanged;
    auto snapshotPluginStates = [this] {
        // Snapshot ALL layers' VST3 plugin state into the controller so the
        // save path can write whatever plugins are currently loaded.
        for (auto& [layerIdx, hosts] : layerHosts_) {
            auto plugins = processorRef.controller().layer_vst3_plugins(layerIdx);
            for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
                auto* host = hosts[i].get();
                if (host && host->is_configured() && plugins[i].has_value()) {
                    auto ps = *plugins[i];
                    juce::MemoryBlock stateData;
                    host->getState(stateData);
                    ps.component_state.assign(
                        static_cast<const std::uint8_t*>(stateData.getData()),
                        static_cast<const std::uint8_t*>(stateData.getData()) + stateData.getSize());
                    processorRef.controller().set_layer_vst3_plugin(layerIdx, i, ps);
                }
            }
        }

        auto auxPlugins = processorRef.controller().aux_vst3_plugins();
        for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
            auto* host = auxHosts_[i].get();
            if (host && host->is_configured() && auxPlugins[i].has_value()) {
                auto ps = *auxPlugins[i];
                juce::MemoryBlock stateData;
                host->getState(stateData);
                ps.component_state.assign(
                    static_cast<const std::uint8_t*>(stateData.getData()),
                    static_cast<const std::uint8_t*>(stateData.getData()) + stateData.getSize());
                processorRef.controller().set_aux_vst3_plugin(i, ps);
            }
        }
    };

    toolbar_.onSave = [this, snapshotPluginStates] {
        snapshotPluginStates();
        auto chooser = std::make_shared<juce::FileChooser>(
            "Save Project", juce::File(), "*.tfproj");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode, [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file != juce::File()) {
                std::string err;
                processorRef.controller().save_project(file.getFullPathName().toStdString(), &err);
                toolbar_.setProjectName(file.getFileNameWithoutExtension());
                layerList_.setProjectName(file.getFileNameWithoutExtension());
            }
        });
    };

    toolbar_.onSaveWithAudio = [this, snapshotPluginStates] {
        snapshotPluginStates();
        auto chooser = std::make_shared<juce::FileChooser>(
            "Save Project with Audio Embedded", juce::File(), "*.tfproj");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode, [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            std::string err;
            // Embedding can take noticeable time on large sessions because every
            // source buffer is FLAC-encoded; running on the message thread is
            // OK for now since saves are user-initiated and infrequent.
            const bool ok = processorRef.controller().save_project_with_audio(
                file.getFullPathName().toStdString(), &err);
            if (!ok) {
                juce::AlertWindow::showAsync(
                    juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Save Failed")
                        .withMessage(juce::String("Could not save project: ") + juce::String(err))
                        .withButton("OK"),
                    nullptr);
                return;
            }
            toolbar_.setProjectName(file.getFileNameWithoutExtension());
            layerList_.setProjectName(file.getFileNameWithoutExtension());
        });
    };
    toolbar_.onOpen = [this] {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Open", juce::File(), "*.radium;*.tfproj;*.riproj");
        chooser->launchAsync(juce::FileBrowserComponent::openMode, [this, chooser](const juce::FileChooser& fc) {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            auto ext = file.getFileExtension().toLowerCase();
            resetHostedPluginsForSessionChange();
            if (ext == ".radium") {
                processorRef.controller().import_file(file.getFullPathName().toStdString());
            } else {
                std::string err;
                processorRef.controller().load_project(file.getFullPathName().toStdString(), &err);
            }
            focusedModule_ = FocusModule::None;
            updateFocusedModuleButtons();
            clearAllLayerEditHistory();
            toolbar_.setProjectName(file.getFileNameWithoutExtension());
            layerList_.setProjectName(file.getFileNameWithoutExtension());
            stopRecordingOwnedPicturePlayback(true);
            stopTakeOwnedPicturePlayback(false);
            if (const auto picturePath = processorRef.controller().project_picture_path();
                picturePath.has_value() && std::filesystem::exists(*picturePath)) {
                loadVideoFile(juce::File(juce::String(picturePath->string())));
            } else {
                processorRef.controller().clear_project_picture_path();
                recorderPictureCueSeconds_ = 0.0;
                if (pictureWindow_) {
                    pictureWindow_->content().clearVideo();
                }
            }
            refreshUI();
        });
    };
    toolbar_.onNew = [this] {
        resetHostedPluginsForSessionChange();
        stopRecordingOwnedPicturePlayback(true);
        stopTakeOwnedPicturePlayback(false);
        if (pictureWindow_) {
            pictureWindow_->content().clearVideo();
        }
        processorRef.controller().clear_project_picture_path();
        recorderPictureCueSeconds_ = 0.0;
        newRecordingActive_ = false;
        punchInActive_ = false;
        isTakePlaying_ = false;
        sessionRecorder_.clearPunchInRegion();
        sessionRecorder_.setTakePlayhead(-1.0);
        processorRef.controller().new_empty_project();
        focusedModule_ = FocusModule::None;
        updateFocusedModuleButtons();
        clearAllLayerEditHistory();
        toolbar_.setProjectName("Untitled Project");
        layerList_.setProjectName("Untitled Project");
        refreshUI();
    };

    // Layer list callbacks
    layerList_.onLayerSelect = [this](std::size_t idx) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        processorRef.controller().select_layer(idx);
        refreshUI();
    };
    layerList_.onMuteToggle = [this](std::size_t idx, bool muted) {
        processorRef.controller().set_layer_mute(idx, muted);
        processorRef.controller().push_live_mute(idx);
        refreshUI();
    };
    layerList_.onSoloToggle = [this](std::size_t idx, bool soloed) {
        processorRef.controller().set_layer_solo(idx, soloed);
        processorRef.controller().push_live_solo();
        refreshUI();
    };
    layerList_.onGainChange = [this](std::size_t idx, double gain) {
        processorRef.controller().set_layer_gain(idx, gain);
        processorRef.controller().push_live_gain(idx);
    };
    layerList_.onAutoSplit = [this](std::size_t idx) {
        std::string err;
        if (!processorRef.controller().auto_split_layer_regions(idx, &err) && !err.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Auto Split Failed",
                err);
        }
        refreshUI();
    };
    layerList_.onAudioDropped = [this](std::size_t idx, const juce::String& filePath) {
        std::string err;
        if (!processorRef.controller().add_audio_file_to_layer(idx, filePath.toStdString(), &err) && !err.empty()) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Add Audio Failed",
                err);
        }
        clearLayerEditHistory(idx);
        invalidateFocusWaveformCache();
        refreshUI();
    };
    layerList_.onClearLayer = [this](std::size_t idx) {
        if (processorRef.controller().selected_layer_index().has_value() &&
            *processorRef.controller().selected_layer_index() == idx) {
            vst3Rack_.clearDisplay();
        }

        auto hostIt = layerHosts_.find(idx);
        if (hostIt != layerHosts_.end()) {
            for (auto& host : hostIt->second) {
                if (host) {
                    host->unloadPlugin();
                }
            }
        }

        std::string err;
        processorRef.controller().clear_layer_audio(idx, &err);
        clearLayerEditHistory(idx);
        invalidateFocusWaveformCache();
        refreshUI();
    };
    layerList_.onLayerLockToggle = [this](std::size_t idx, bool locked) {
        processorRef.controller().set_layer_locked(idx, locked);
        refreshUI();
    };
    layerList_.onLayerRename = [this](std::size_t idx, const juce::String& name) {
        processorRef.controller().set_layer_custom_name(idx, name.toStdString());
        refreshUI();
    };

    // Piano keyboard callbacks — interrupt any active playback before triggering
    // Register callback to wire plugin hosts whenever streaming layers are rebuilt
    processorRef.controller().on_streaming_layers_rebuilt = [this] {
        wirePluginSessions(true);
    };

    keyboard_.onNoteOn = [this](int midiNote) {
        auto& ctrl = processorRef.controller();
        if (ctrl.trigger_mode() == radium::TriggerMode::kContinuous) {
            if (ctrl.is_streaming()) {
                const auto heldMidi = ctrl.held_note_midi();
                ctrl.stop_streaming_playback();
                if (heldMidi.has_value() && *heldMidi == midiNote) {
                    return;
                }
            }
        } else if (ctrl.is_streaming()) {
            ctrl.stop_streaming_playback();
        }
        std::string err;
        ctrl.start_streaming_playback(midiNote, &err);
    };
    keyboard_.onNoteOff = [this](int midiNote) {
        if (processorRef.controller().trigger_mode() == radium::TriggerMode::kContinuous) {
            return;
        }
        processorRef.controller().trigger_note_off(midiNote);
    };
    keyboard_.onLoopModeChanged = [this](bool enabled) {
        processorRef.controller().set_trigger_mode(
            enabled ? radium::TriggerMode::kContinuous : radium::TriggerMode::kOneShot);
        if (!enabled && processorRef.controller().is_streaming()) {
            processorRef.controller().stop_streaming_playback();
            refreshUI();
        }
    };

    // Waveform callbacks
    waveform_.onAuditionStart = [this](double norm) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            processorRef.controller().set_layer_audition_start(*sel, norm);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    waveform_.onRegionCreated = [this](double start, double end) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            processorRef.controller().add_layer_trigger_region(*sel, start, end);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    waveform_.onRegionUpdated = [this](std::size_t regionIdx, double start, double end) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            processorRef.controller().update_layer_trigger_region(*sel, regionIdx, start, end);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    waveform_.onRegionsCleared = [this] {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            processorRef.controller().clear_layer_trigger_regions(*sel);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    waveform_.onEditGestureBegan = [this] {
        if (const auto sel = processorRef.controller().selected_layer_index(); sel.has_value()) {
            captureLayerEditUndoSnapshot(*sel);
        }
    };
    waveform_.onEditClipMoved = [this](std::size_t clipIndex, double deltaNorm) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        const auto durationSeconds = processorRef.controller().layer_timeline_duration_seconds(*sel);
        if (!durationSeconds.has_value() || *durationSeconds <= 0.0) {
            return;
        }

        if (processorRef.controller().move_layer_edit_clip(*sel, clipIndex, deltaNorm * *durationSeconds, false)) {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onEditClipsMoved = [this](const std::vector<std::size_t>& clipIndices, double deltaNorm) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        const auto durationSeconds = processorRef.controller().layer_timeline_duration_seconds(*sel);
        if (!durationSeconds.has_value() || *durationSeconds <= 0.0) {
            return;
        }
        if (processorRef.controller().move_layer_edit_clips(*sel, clipIndices, deltaNorm * *durationSeconds, false)) {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onEditClipTrimmed = [this](std::size_t clipIndex, bool trimLeftEdge, double normalizedTimeline) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        if (processorRef.controller().trim_layer_edit_clip_edge(
                *sel, clipIndex, trimLeftEdge, normalizedTimeline, false)) {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onEditClipFadeChanged = [this](std::size_t clipIndex, bool fadeIn, double normalizedTimeline) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        if (processorRef.controller().set_layer_edit_clip_fade(
                *sel, clipIndex, fadeIn, normalizedTimeline, false)) {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onEditGestureFinished = [this](std::optional<std::size_t> clipIndex) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        if (processorRef.controller().commit_layer_edit_changes(*sel, clipIndex)) {
            invalidateFocusWaveformCache();
            refreshUI();
        } else {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onGroupedEditGestureFinished = [this](const std::vector<std::size_t>& clipIndices) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        if (processorRef.controller().commit_layer_edit_changes(*sel, clipIndices)) {
            invalidateFocusWaveformCache();
            refreshUI();
        } else {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onAutomationGestureBegan = [this] {
        if (const auto sel = processorRef.controller().selected_layer_index(); sel.has_value()) {
            captureLayerEditUndoSnapshot(*sel);
        }
    };
    waveform_.onAutomationPointCreated = [this](double normalizedTimeline) -> std::optional<std::size_t> {
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return std::nullopt;
        }
        std::optional<std::size_t> index;
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                index = processorRef.controller().add_layer_stretch_automation_point(*sel, normalizedTimeline);
                break;
            case FocusAutomationTarget::Doppler:
                index = processorRef.controller().add_layer_doppler_automation_point(*sel, normalizedTimeline);
                break;
            case FocusAutomationTarget::PanPosition:
                index = processorRef.controller().add_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::Position, normalizedTimeline);
                break;
            case FocusAutomationTarget::PanFrontBack:
                index = processorRef.controller().add_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::FrontBack, normalizedTimeline);
                break;
            case FocusAutomationTarget::PanRightPosition:
                index = processorRef.controller().add_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightPosition, normalizedTimeline);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                index = processorRef.controller().add_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightFrontBack, normalizedTimeline);
                break;
            case FocusAutomationTarget::Volume:
            default:
                index = processorRef.controller().add_layer_volume_automation_point(*sel, normalizedTimeline);
                break;
        }
        if (index.has_value()) {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
        return index;
    };
    waveform_.onAutomationPointMoved = [this](std::size_t pointIndex, double normalizedTimeline, double value) {
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }
        bool moved = false;
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                moved = processorRef.controller().move_layer_stretch_automation_point(
                    *sel, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::Doppler:
                moved = processorRef.controller().move_layer_doppler_automation_point(
                    *sel, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::PanPosition:
                moved = processorRef.controller().move_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::Position, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::PanFrontBack:
                moved = processorRef.controller().move_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::FrontBack, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::PanRightPosition:
                moved = processorRef.controller().move_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightPosition, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                moved = processorRef.controller().move_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightFrontBack, pointIndex, normalizedTimeline, value);
                break;
            case FocusAutomationTarget::Volume:
            default:
                moved = processorRef.controller().move_layer_volume_automation_point(
                    *sel, pointIndex, normalizedTimeline, value, false);
                break;
        }
        if (moved) {
            waveform_.previewAutomationPointMove(pointIndex, normalizedTimeline, value);
        }
    };
    waveform_.onAutomationPointDeleted = [this](std::size_t pointIndex) {
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }
        bool deleted = false;
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                deleted = processorRef.controller().delete_layer_stretch_automation_point(*sel, pointIndex);
                break;
            case FocusAutomationTarget::Doppler:
                deleted = processorRef.controller().delete_layer_doppler_automation_point(*sel, pointIndex);
                break;
            case FocusAutomationTarget::PanPosition:
                deleted = processorRef.controller().delete_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::Position, pointIndex);
                break;
            case FocusAutomationTarget::PanFrontBack:
                deleted = processorRef.controller().delete_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::FrontBack, pointIndex);
                break;
            case FocusAutomationTarget::PanRightPosition:
                deleted = processorRef.controller().delete_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightPosition, pointIndex);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                deleted = processorRef.controller().delete_layer_pan_automation_point(
                    *sel, radium::AppController::PanAutomationTarget::RightFrontBack, pointIndex);
                break;
            case FocusAutomationTarget::Volume:
            default:
                deleted = processorRef.controller().delete_layer_volume_automation_point(*sel, pointIndex);
                break;
        }
        if (deleted) {
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    waveform_.onAutomationGestureFinished = [this] {
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                processorRef.controller().commit_layer_stretch_automation(*sel);
                break;
            case FocusAutomationTarget::Doppler:
                processorRef.controller().commit_layer_doppler_automation(*sel);
                break;
            case FocusAutomationTarget::PanPosition:
                processorRef.controller().commit_layer_pan_automation(
                    *sel, radium::AppController::PanAutomationTarget::Position);
                break;
            case FocusAutomationTarget::PanFrontBack:
                processorRef.controller().commit_layer_pan_automation(
                    *sel, radium::AppController::PanAutomationTarget::FrontBack);
                break;
            case FocusAutomationTarget::PanRightPosition:
                processorRef.controller().commit_layer_pan_automation(
                    *sel, radium::AppController::PanAutomationTarget::RightPosition);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                processorRef.controller().commit_layer_pan_automation(
                    *sel, radium::AppController::PanAutomationTarget::RightFrontBack);
                break;
            case FocusAutomationTarget::Volume:
            default:
                processorRef.controller().commit_layer_volume_automation(*sel);
                break;
        }
        invalidateFocusWaveformCache();
        refreshUI();
    };
    waveform_.onAutomationSegmentSelected = [this](std::optional<std::size_t> leftPointId) {
        selectedDopplerSegmentLeftPointId_ = leftPointId;
        refreshDopplerControls();
    };
    waveform_.onAutomationFreehandDraw = [this](const std::vector<std::pair<double, double>>& strokePoints) {
        applyFreehandDopplerStroke(strokePoints);
    };
    waveform_.onCrossfadeRequested = [this](double start, double end) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        const auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        if (processorRef.controller().apply_layer_edit_crossfade(*sel, start, end)) {
            waveform_.clearEditSelection();
            invalidateFocusWaveformCache();
            refreshUI();
        } else {
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
        }
    };
    waveform_.onViewRangeChanged = [this](double /*vStart*/, double /*vEnd*/) {
        // Re-fetch peaks at higher resolution (always full file range so
        // peak indices stay aligned with the 0..1 normalized coordinates
        // used by regions and markers)
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            int buckets = std::max(512, waveform_.getWidth() * 2);
            auto overview = processorRef.controller().layer_waveform(
                *sel, static_cast<std::size_t>(buckets));
            if (overview.has_value()) {
                waveform_.setWaveform(*overview);
                cachedFocusWaveformLayerIndex_ = *sel;
                cachedFocusWaveformRevision_ = processorRef.controller().layer_waveform_revision(*sel);
                cachedFocusWaveformWidth_ = buckets;
                focusWaveformDirty_ = false;
            }
        }
    };

    // VST folder button
    vstFolderButton_.onClick = [this] {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Select VST3 Plugin Folder", vst3Scanner_.getScanFolder());
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                 juce::FileBrowserComponent::canSelectDirectories,
            [this, chooser](const juce::FileChooser& fc) {
                auto folder = fc.getResult();
                if (folder.isDirectory()) {
                    vst3Scanner_.setScanFolder(folder);
                    vstFolderButton_.setButtonText(folder.getFullPathName());
                }
            });
    };
    if (vst3Scanner_.hasScanFolder()) {
        vstFolderButton_.setButtonText(vst3Scanner_.getScanFolder().getFullPathName());
    }
    addAndMakeVisible(vstFolderButton_);

    // VST3 rack callbacks
    vst3Rack_.onPluginLoaded = [this](std::size_t slotIndex, const juce::String& filePath,
                                       const juce::String& classId, const juce::String& name) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) return;
        radium::AppController::LayerOverride::Vst3PluginState ps;
        ps.module_path = filePath.toStdString();
        ps.class_id = classId.toStdString();
        ps.display_name = name.toStdString();
        processorRef.controller().set_layer_vst3_plugin(*sel, slotIndex, ps);
        wirePluginSessions();
    };
    vst3Rack_.onPluginRemoved = [this](std::size_t slotIndex) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) return;
        processorRef.controller().clear_layer_vst3_plugin(*sel, slotIndex);
        wirePluginSessions();
    };
    vst3Rack_.onBypassToggle = [this](std::size_t slotIndex) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) return;
        processorRef.controller().toggle_layer_vst3_bypass(*sel, slotIndex);
        wirePluginSessions();
    };

    auxVst3Rack_.onPluginLoaded = [this](std::size_t slotIndex, const juce::String& filePath,
                                         const juce::String& classId, const juce::String& name) {
        radium::AppController::LayerOverride::Vst3PluginState ps;
        ps.module_path = filePath.toStdString();
        ps.class_id = classId.toStdString();
        ps.display_name = name.toStdString();
        processorRef.controller().set_aux_vst3_plugin(slotIndex, ps);
        wirePluginSessions();
    };
    auxVst3Rack_.onPluginRemoved = [this](std::size_t slotIndex) {
        processorRef.controller().clear_aux_vst3_plugin(slotIndex);
        wirePluginSessions();
    };
    auxVst3Rack_.onBypassToggle = [this](std::size_t slotIndex) {
        processorRef.controller().toggle_aux_vst3_bypass(slotIndex);
        wirePluginSessions();
    };

    // Surround panner callbacks
    surroundPanner_.onPanChanged = [this](double x, double y) {
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            auto& ctrl = processorRef.controller();
            const bool dopplerLaneVisible = focusAutomationTarget_ == FocusAutomationTarget::Doppler;
            const bool panLaneVisible =
                focusAutomationTarget_ == FocusAutomationTarget::PanPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanFrontBack ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightFrontBack;
            const auto maybePlaybackPos = ctrl.layer_streaming_position(*sel);
            const bool shouldWriteAutomation =
                focusedModule_ == FocusModule::FocusTrack &&
                (panLaneVisible || dopplerLaneVisible) &&
                playbackTarget_ == PlaybackTarget::FocusLayer &&
                ctrl.is_streaming() &&
                !isTakePlaying_ &&
                maybePlaybackPos.has_value();
            if (shouldWriteAutomation) {
                auto fx = ctrl.layer_effect_state(*sel);
                if (!trackAutomationWriteActive_ || !trackAutomationWriteLayerIndex_.has_value() ||
                    *trackAutomationWriteLayerIndex_ != *sel || !trackAutomationWriteTarget_.has_value() ||
                    !(*trackAutomationWriteTarget_ == FocusAutomationTarget::Doppler ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanPosition ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanFrontBack ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightPosition ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightFrontBack)) {
                    captureLayerEditUndoSnapshot(*sel);
                    trackAutomationWriteActive_ = true;
                    trackAutomationWriteLayerIndex_ = *sel;
                    trackAutomationWriteTarget_ = focusAutomationTarget_;
                    const auto& overrides = ctrl.layer_overrides();
                    if (*sel < overrides.size()) {
                        trackAutomationWriteOriginalPanX_ = overrides[*sel].pan_x;
                        trackAutomationWriteOriginalPanY_ = overrides[*sel].pan_y;
                        trackAutomationWriteOriginalPanXRight_ = overrides[*sel].pan_x_right;
                        trackAutomationWriteOriginalPanYRight_ = overrides[*sel].pan_y_right;
                        trackAutomationWriteOriginalGain_ = overrides[*sel].gain;
                    }
                    if (!trackAutomationWriteOriginalGain_.has_value()) {
                        trackAutomationWriteOriginalGain_ = 1.0;
                    }
                    trackAutomationWriteOriginalStretch_ = fx.has_value() ? std::optional<double>(fx->time_stretch_ratio)
                                                                          : std::optional<double>(1.0);
                }

                double sourceNormalizedTimeline = *maybePlaybackPos;
                if (fx.has_value() && fx->reverse) {
                    sourceNormalizedTimeline = 1.0 - sourceNormalizedTimeline;
                }

                if (dopplerLaneVisible) {
                    const auto dopplerSettings =
                        ctrl.layer_doppler_settings(*sel).value_or(radium::DopplerSettings{});
                    const double basePanX = trackAutomationWriteOriginalPanX_.value_or(0.0);
                    const double basePanY = trackAutomationWriteOriginalPanY_.value_or(0.0);
                    const double basePanXRight = trackAutomationWriteOriginalPanXRight_.value_or(basePanX);
                    const double basePanYRight = trackAutomationWriteOriginalPanYRight_.value_or(basePanY);
                    const bool isStereoLayer = ctrl.layer_is_stereo(*sel);
                    const double halfWidth = isStereoLayer ? 0.5 * (basePanXRight - basePanX) : 0.0;
                    const double doppler = std::clamp(isStereoLayer ? (x + halfWidth) : x, -1.0, 1.0);
                    const double effectiveLeft = std::clamp(doppler - halfWidth, -1.0, 1.0);
                    const double effectiveRight = isStereoLayer
                                                      ? std::clamp(doppler + halfWidth, -1.0, 1.0)
                                                      : effectiveLeft;

                    ctrl.enable_layer_doppler_automation(*sel);
                    if (const auto pointId = ctrl.add_layer_doppler_automation_point(*sel, sourceNormalizedTimeline);
                        pointId.has_value()) {
                        ctrl.move_layer_doppler_automation_point(
                            *sel, *pointId, sourceNormalizedTimeline, doppler);
                    }

                    if (fx.has_value()) {
                        fx->time_stretch_ratio =
                            trackAutomationWriteOriginalStretch_.value_or(fx->time_stretch_ratio) *
                            dopplerPitchRatio(doppler, dopplerSettings);
                        ctrl.set_layer_effect_state(*sel, *fx);
                        ctrl.push_live_stretch(*sel);
                    }
                    ctrl.set_layer_gain(*sel,
                                        trackAutomationWriteOriginalGain_.value_or(1.0) *
                                            dopplerGainMultiplier(doppler, dopplerSettings));
                    ctrl.push_live_gain(*sel);
                    ctrl.set_layer_pan(*sel, effectiveLeft, basePanY);
                    ctrl.set_layer_pan_right(*sel, effectiveRight, basePanYRight);
                    ctrl.push_live_pan(*sel);
                    invalidateFocusWaveformCache();
                    refreshFocusWaveform();
                    return;
                }

                ctrl.enable_layer_pan_automation(*sel, radium::AppController::PanAutomationTarget::Position);
                ctrl.enable_layer_pan_automation(*sel, radium::AppController::PanAutomationTarget::FrontBack);
                if (const auto pointId = ctrl.add_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::Position, sourceNormalizedTimeline);
                    pointId.has_value()) {
                    ctrl.move_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::Position, *pointId, sourceNormalizedTimeline, x);
                }
                if (const auto pointId = ctrl.add_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::FrontBack, sourceNormalizedTimeline);
                    pointId.has_value()) {
                    ctrl.move_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::FrontBack, *pointId, sourceNormalizedTimeline, y);
                }
                ctrl.set_layer_pan(*sel, x, y);
                ctrl.push_live_pan(*sel);
                invalidateFocusWaveformCache();
                refreshFocusWaveform();
                return;
            }

            ctrl.set_layer_pan(*sel, x, y);
            ctrl.push_live_pan(*sel);
        }
    };
    surroundPanner_.onPanRightChanged = [this](double x, double y) {
        auto sel = processorRef.controller().selected_layer_index();
        if (sel.has_value()) {
            auto& ctrl = processorRef.controller();
            const bool dopplerLaneVisible = focusAutomationTarget_ == FocusAutomationTarget::Doppler;
            const bool panLaneVisible =
                focusAutomationTarget_ == FocusAutomationTarget::PanPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanFrontBack ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightFrontBack;
            const auto maybePlaybackPos = ctrl.layer_streaming_position(*sel);
            const bool shouldWriteAutomation =
                focusedModule_ == FocusModule::FocusTrack &&
                (panLaneVisible || dopplerLaneVisible) &&
                playbackTarget_ == PlaybackTarget::FocusLayer &&
                ctrl.is_streaming() &&
                !isTakePlaying_ &&
                maybePlaybackPos.has_value();
            if (shouldWriteAutomation) {
                auto fx = ctrl.layer_effect_state(*sel);
                if (!trackAutomationWriteActive_ || !trackAutomationWriteLayerIndex_.has_value() ||
                    *trackAutomationWriteLayerIndex_ != *sel || !trackAutomationWriteTarget_.has_value() ||
                    !(*trackAutomationWriteTarget_ == FocusAutomationTarget::Doppler ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanPosition ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanFrontBack ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightPosition ||
                      *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightFrontBack)) {
                    captureLayerEditUndoSnapshot(*sel);
                    trackAutomationWriteActive_ = true;
                    trackAutomationWriteLayerIndex_ = *sel;
                    trackAutomationWriteTarget_ = focusAutomationTarget_;
                    const auto& overrides = ctrl.layer_overrides();
                    if (*sel < overrides.size()) {
                        trackAutomationWriteOriginalPanX_ = overrides[*sel].pan_x;
                        trackAutomationWriteOriginalPanY_ = overrides[*sel].pan_y;
                        trackAutomationWriteOriginalPanXRight_ = overrides[*sel].pan_x_right;
                        trackAutomationWriteOriginalPanYRight_ = overrides[*sel].pan_y_right;
                        trackAutomationWriteOriginalGain_ = overrides[*sel].gain;
                    }
                    if (!trackAutomationWriteOriginalGain_.has_value()) {
                        trackAutomationWriteOriginalGain_ = 1.0;
                    }
                    trackAutomationWriteOriginalStretch_ = fx.has_value() ? std::optional<double>(fx->time_stretch_ratio)
                                                                          : std::optional<double>(1.0);
                }

                double sourceNormalizedTimeline = *maybePlaybackPos;
                if (fx.has_value() && fx->reverse) {
                    sourceNormalizedTimeline = 1.0 - sourceNormalizedTimeline;
                }

                if (dopplerLaneVisible) {
                    const auto dopplerSettings =
                        ctrl.layer_doppler_settings(*sel).value_or(radium::DopplerSettings{});
                    const double basePanX = trackAutomationWriteOriginalPanX_.value_or(0.0);
                    const double basePanY = trackAutomationWriteOriginalPanY_.value_or(0.0);
                    const double basePanXRight = trackAutomationWriteOriginalPanXRight_.value_or(basePanX);
                    const double basePanYRight = trackAutomationWriteOriginalPanYRight_.value_or(basePanY);
                    const bool isStereoLayer = ctrl.layer_is_stereo(*sel);
                    const double halfWidth = isStereoLayer ? 0.5 * (basePanXRight - basePanX) : 0.0;
                    const double doppler = std::clamp(isStereoLayer ? (x - halfWidth) : x, -1.0, 1.0);
                    const double effectiveLeft = std::clamp(doppler - halfWidth, -1.0, 1.0);
                    const double effectiveRight = isStereoLayer
                                                      ? std::clamp(doppler + halfWidth, -1.0, 1.0)
                                                      : effectiveLeft;

                    ctrl.enable_layer_doppler_automation(*sel);
                    if (const auto pointId = ctrl.add_layer_doppler_automation_point(*sel, sourceNormalizedTimeline);
                        pointId.has_value()) {
                        ctrl.move_layer_doppler_automation_point(
                            *sel, *pointId, sourceNormalizedTimeline, doppler);
                    }

                    if (fx.has_value()) {
                        fx->time_stretch_ratio =
                            trackAutomationWriteOriginalStretch_.value_or(fx->time_stretch_ratio) *
                            dopplerPitchRatio(doppler, dopplerSettings);
                        ctrl.set_layer_effect_state(*sel, *fx);
                        ctrl.push_live_stretch(*sel);
                    }
                    ctrl.set_layer_gain(*sel,
                                        trackAutomationWriteOriginalGain_.value_or(1.0) *
                                            dopplerGainMultiplier(doppler, dopplerSettings));
                    ctrl.push_live_gain(*sel);
                    ctrl.set_layer_pan(*sel, effectiveLeft, basePanY);
                    ctrl.set_layer_pan_right(*sel, effectiveRight, basePanYRight);
                    ctrl.push_live_pan(*sel);
                    invalidateFocusWaveformCache();
                    refreshFocusWaveform();
                    return;
                }

                ctrl.enable_layer_pan_automation(*sel, radium::AppController::PanAutomationTarget::RightPosition);
                ctrl.enable_layer_pan_automation(*sel, radium::AppController::PanAutomationTarget::RightFrontBack);
                if (const auto pointId = ctrl.add_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::RightPosition, sourceNormalizedTimeline);
                    pointId.has_value()) {
                    ctrl.move_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::RightPosition, *pointId, sourceNormalizedTimeline, x);
                }
                if (const auto pointId = ctrl.add_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::RightFrontBack, sourceNormalizedTimeline);
                    pointId.has_value()) {
                    ctrl.move_layer_pan_automation_point(
                        *sel, radium::AppController::PanAutomationTarget::RightFrontBack, *pointId, sourceNormalizedTimeline, y);
                }
                ctrl.set_layer_pan_right(*sel, x, y);
                ctrl.push_live_pan(*sel);
                invalidateFocusWaveformCache();
                refreshFocusWaveform();
                return;
            }

            ctrl.set_layer_pan_right(*sel, x, y);
            ctrl.push_live_pan(*sel);
        }
    };

    // Master controls callbacks
    masterControls_.onReverseToggle = [this](bool reverse) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) return;
        auto fx = processorRef.controller().layer_effect_state(*sel);
        if (fx.has_value()) {
            fx->reverse = reverse;
            processorRef.controller().set_layer_effect_state(*sel, *fx);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    };
    masterControls_.onTimeStretchChange = [this](double ratio) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) return;
        auto& ctrl = processorRef.controller();
        const auto maybePlaybackPos = ctrl.layer_streaming_position(*sel);
        const bool shouldWriteAutomation =
            focusedModule_ == FocusModule::FocusTrack &&
            playbackTarget_ == PlaybackTarget::FocusLayer &&
            ctrl.is_streaming() &&
            !isTakePlaying_ &&
            maybePlaybackPos.has_value();

        auto fx = ctrl.layer_effect_state(*sel);
        if (!fx.has_value()) {
            return;
        }

        if (shouldWriteAutomation) {
            focusAutomationTarget_ = FocusAutomationTarget::Stretch;
            if (!trackAutomationWriteActive_ || !trackAutomationWriteLayerIndex_.has_value() ||
                *trackAutomationWriteLayerIndex_ != *sel ||
                !trackAutomationWriteTarget_.has_value() ||
                *trackAutomationWriteTarget_ != FocusAutomationTarget::Stretch) {
                captureLayerEditUndoSnapshot(*sel);
                trackAutomationWriteActive_ = true;
                trackAutomationWriteLayerIndex_ = *sel;
                trackAutomationWriteTarget_ = FocusAutomationTarget::Stretch;
                trackAutomationWriteOriginalGain_.reset();
                trackAutomationWriteOriginalStretch_ = fx->time_stretch_ratio;
            }

            double sourceNormalizedTimeline = *maybePlaybackPos;
            if (fx->reverse) {
                sourceNormalizedTimeline = 1.0 - sourceNormalizedTimeline;
            }

            ctrl.enable_layer_stretch_automation(*sel);
            if (const auto pointId = ctrl.add_layer_stretch_automation_point(*sel, sourceNormalizedTimeline);
                pointId.has_value()) {
                ctrl.move_layer_stretch_automation_point(*sel, *pointId, sourceNormalizedTimeline, ratio);
            }

            fx->time_stretch_ratio = ratio;
            ctrl.set_layer_effect_state(*sel, *fx);
            ctrl.push_live_stretch(*sel);
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
            return;
        }

        fx->time_stretch_ratio = ratio;
        ctrl.set_layer_effect_state(*sel, *fx);
        ctrl.push_live_stretch(*sel);
    };
    masterControls_.onBassLfeGainChange = [this](double gainDb) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }
        auto fx = processorRef.controller().layer_effect_state(*sel);
        if (!fx.has_value()) {
            return;
        }
        fx->bass_lfe_gain_db = gainDb;
        processorRef.controller().set_layer_effect_state(*sel, *fx);
        processorRef.controller().push_live_bass_lfe_gain(*sel);
    };
    masterControls_.onTrackGainChange = [this](float gain) {
        auto sel = processorRef.controller().selected_layer_index();
        if (!sel.has_value()) {
            return;
        }

        auto& ctrl = processorRef.controller();
        const auto maybePlaybackPos = ctrl.layer_streaming_position(*sel);
        const bool shouldWriteAutomation =
            focusedModule_ == FocusModule::FocusTrack &&
            playbackTarget_ == PlaybackTarget::FocusLayer &&
            ctrl.is_streaming() &&
            !isTakePlaying_ &&
            maybePlaybackPos.has_value();

        if (shouldWriteAutomation) {
            focusAutomationTarget_ = FocusAutomationTarget::Volume;
            if (!trackAutomationWriteActive_ || !trackAutomationWriteLayerIndex_.has_value() ||
                *trackAutomationWriteLayerIndex_ != *sel ||
                !trackAutomationWriteTarget_.has_value() ||
                *trackAutomationWriteTarget_ != FocusAutomationTarget::Volume) {
                captureLayerEditUndoSnapshot(*sel);
                trackAutomationWriteActive_ = true;
                trackAutomationWriteLayerIndex_ = *sel;
                trackAutomationWriteTarget_ = FocusAutomationTarget::Volume;
                const auto& overrides = ctrl.layer_overrides();
                if (*sel < overrides.size()) {
                    trackAutomationWriteOriginalGain_ = overrides[*sel].gain;
                } else {
                    trackAutomationWriteOriginalGain_ = 1.0;
                }
                trackAutomationWriteOriginalStretch_.reset();
            }

            const auto fx = ctrl.layer_effect_state(*sel);
            double sourceNormalizedTimeline = *maybePlaybackPos;
            if (fx.has_value() && fx->reverse) {
                sourceNormalizedTimeline = 1.0 - sourceNormalizedTimeline;
            }

            ctrl.enable_layer_volume_automation(*sel);
            if (const auto pointId = ctrl.add_layer_volume_automation_point(*sel, sourceNormalizedTimeline);
                pointId.has_value()) {
                ctrl.move_layer_volume_automation_point(*sel, *pointId, sourceNormalizedTimeline, gain, false);
            }
            ctrl.set_layer_gain(*sel, gain);
            ctrl.push_live_gain(*sel);
            invalidateFocusWaveformCache();
            refreshFocusWaveform();
            return;
        }

        processorRef.controller().set_layer_gain(*sel, gain);
        processorRef.controller().push_live_gain(*sel);
        refreshUI();
    };
    // Session recorder callbacks
    sessionRecorder_.onNewToggle = [this](bool armed) {
        if (armed) {
            if (processorRef.controller().is_streaming()) {
                processorRef.controller().stop_streaming_playback();
            }
            isTakePlaying_ = false;
            stopTakeOwnedPicturePlayback(false);
            newRecordingActive_ = true;
            punchInActive_ = false;
            activePunchRegion_.reset();
            sessionRecorder_.clearPunchInRegion();
            sessionRecorder_.setPunchCuePosition(0.0);
            processorRef.controller().streaming_mixer().set_recording(true);
            processorRef.controller().set_session_recording_armed(true);
            sessionRecorder_.clearRecordingPreview();
            sessionRecorder_.setRecordingState(true, false);
            sessionRecorder_.setTakePlayhead(-1.0);
            if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
                auto& picture = pictureWindow_->content();
                picture.seek(recorderPictureCueSeconds_);
                picture.play();
                pictureTransportOwnedByRecorder_ = true;
            }
        } else {
            processorRef.controller().set_session_recording_armed(false);
            auto recording = processorRef.controller().streaming_mixer().take_recording();
            if (recording.has_value()) {
                std::string err;
                processorRef.controller().commit_session_recording(
                    *recording, "take", std::nullopt,
                    (pictureWindow_ && pictureWindow_->content().hasLoadedVideo())
                        ? std::make_optional(recorderPictureCueSeconds_)
                        : std::nullopt,
                    &err);
            }
            newRecordingActive_ = false;
            stopRecordingOwnedPicturePlayback(false);
            sessionRecorder_.clearRecordingPreview();
            sessionRecorder_.setRecordingState(false, false);
            sessionRecorder_.setTakePlayhead(-1.0);
        }
        refreshUI();
        if (!armed) {
            syncRecorderSurroundModeFromSelection();
            syncPictureToSelectedTake();
        }
    };
    sessionRecorder_.onPunchToggle = [this](bool active) {
        if (active) {
            if (processorRef.controller().is_streaming()) {
                processorRef.controller().stop_streaming_playback();
            }
            isTakePlaying_ = false;
            stopTakeOwnedPicturePlayback(false);
            activePunchRegion_ = sessionRecorder_.punchInRegion();
            punchInCueStart_ = sessionRecorder_.punchCuePosition();
            punchInTakeDurationSeconds_ = sessionRecorder_.selectedTakeDurationSeconds();
            if (!activePunchRegion_.has_value() || punchInTakeDurationSeconds_ <= 0.0) {
                sessionRecorder_.setRecordingState(false, false);
                return;
            }

            newRecordingActive_ = false;
            punchInStartedAt_ = std::chrono::steady_clock::now();
            punchInActive_ = true;
            processorRef.controller().streaming_mixer().set_recording(true);
            processorRef.controller().set_session_recording_armed(true);
            sessionRecorder_.clearRecordingPreview();
            sessionRecorder_.setRecordingState(true, true);
            sessionRecorder_.setTakePlayhead(punchInCueStart_);
            if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
                auto& picture = pictureWindow_->content();
                picture.seek(recorderPictureCueSeconds_);
                picture.play();
                pictureTransportOwnedByRecorder_ = true;
            }
        } else {
            finalizePunchRecording();
        }
        refreshUI();
    };
    sessionRecorder_.onTakeSelect = [this](std::size_t index) {
        playbackTarget_ = PlaybackTarget::RecorderTake;
        stopTakeOwnedPicturePlayback(false);
        processorRef.controller().select_session_recording(index);
        newRecordingActive_ = false;
        punchInActive_ = false;
        activePunchRegion_.reset();
        sessionRecorder_.clearRecordingPreview();
        sessionRecorder_.setRecordingState(false, false);
        isTakePlaying_ = false;
        sessionRecorder_.setTakePlayhead(-1.0);
        syncRecorderSurroundModeFromSelection();
        refreshUI();
        syncPictureToSelectedTake();
    };
    sessionRecorder_.onExport = [this] {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Export Take", juce::File(), "*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode,
            [this, chooser](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file != juce::File()) {
                    std::string err;
                    processorRef.controller().export_selected_session_recording(
                        file.getFullPathName().toStdString(), &err);
                }
            });
    };
    sessionRecorder_.onRename = [this](const std::string& newName) {
        std::string err;
        processorRef.controller().rename_selected_session_recording(newName, &err);
        refreshUI();
    };
    sessionRecorder_.onPlayTake = [this](std::size_t takeIndex) {
        playbackTarget_ = PlaybackTarget::RecorderTake;
        if (processorRef.controller().is_streaming()) {
            processorRef.controller().stop_streaming_playback();
        }
        stopTakeOwnedPicturePlayback(false);
        const double startNorm = sessionRecorder_.punchCuePosition();
        std::string err;
        if (processorRef.controller().play_session_take(takeIndex, &err, startNorm)) {
            isTakePlaying_ = true;
            if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
                if (const auto pictureStart = selectedTakePictureStartSeconds(); pictureStart.has_value()) {
                    auto& picture = pictureWindow_->content();
                    const double pictureCue =
                        *pictureStart + startNorm * sessionRecorder_.selectedTakeDurationSeconds();
                    picture.seek(pictureCue);
                    picture.play();
                    pictureTransportOwnedByTakePlayback_ = true;
                }
            }
        }
    };
    sessionRecorder_.onStopTake = [this] {
        playbackTarget_ = PlaybackTarget::RecorderTake;
        processorRef.controller().stop_streaming_playback();
        isTakePlaying_ = false;
        sessionRecorder_.setTakePlayhead(-1.0);
        stopTakeOwnedPicturePlayback(false);
    };
    sessionRecorder_.onDeleteTake = [this](std::size_t takeIndex) {
        processorRef.controller().delete_session_recording(takeIndex);
        syncRecorderSurroundModeFromSelection();
        refreshUI();
    };
    sessionRecorder_.onTakeScrub = [this](std::size_t takeIndex, double normPos) {
        playbackTarget_ = PlaybackTarget::RecorderTake;
        auto selectedTakeIndex = processorRef.controller().selected_session_recording_index();
        if (!selectedTakeIndex.has_value() || *selectedTakeIndex != takeIndex) {
            processorRef.controller().select_session_recording(takeIndex);
            syncRecorderSurroundModeFromSelection();
        }

        if (isTakePlaying_) {
            processorRef.controller().stop_streaming_playback();
            isTakePlaying_ = false;
            sessionRecorder_.setTakePlayhead(-1.0);
        }
        stopTakeOwnedPicturePlayback(false);

        if (const auto pictureStart = selectedTakePictureStartSeconds(); pictureStart.has_value()) {
            recorderPictureCueSeconds_ =
                *pictureStart + std::clamp(normPos, 0.0, 1.0) * sessionRecorder_.selectedTakeDurationSeconds();
            if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
                pictureWindow_->content().seek(recorderPictureCueSeconds_);
            }
        }

        refreshRecorderPictureControls();
    };
    sessionRecorder_.onGetTakeFile = [this](std::size_t takeIndex) -> juce::File {
        const auto& takes = processorRef.controller().session_recordings();
        if (takeIndex < takes.size()) {
            return juce::File(juce::String(takes[takeIndex].path.string()));
        }
        return {};
    };

    // Refresh UI periodically for playback position updates
    startTimerHz(30);
    refreshUI();
    syncRecorderSurroundModeFromSelection();
}

TriggerfishEditor::~TriggerfishEditor() {
    processorRef.controller().on_streaming_layers_rebuilt = nullptr;
    stopTimer();
    setLookAndFeel(nullptr);
}

bool TriggerfishEditor::keyPressed(const juce::KeyPress& key) {
    auto& ctrl = processorRef.controller();
    auto triggerKeyboardNote = [this](int keyIndex) {
        if (keyIndex < 0 || keyIndex >= 12) {
            return false;
        }
        playbackTarget_ = PlaybackTarget::FocusLayer;
        if (processorRef.controller().is_streaming()) {
            processorRef.controller().stop_streaming_playback();
        }
        keyboard_.triggerVirtualKey(keyIndex);
        return true;
    };
    auto selectedLayer = ctrl.selected_layer_index();
    auto currentFocusCursor = [&]() -> std::optional<double> {
        if (!selectedLayer.has_value() || *selectedLayer >= ctrl.layer_overrides().size()) {
            return std::nullopt;
        }
        return ctrl.layer_overrides()[*selectedLayer].audition_start;
    };
    auto tryHandleFocusEditKey = [&](juce::juce_wchar ch) -> bool {
        if (!selectedLayer.has_value()) {
            return false;
        }

        const auto cursor = currentFocusCursor();
        if (!cursor.has_value()) {
            return false;
        }

        const auto lower = static_cast<juce::juce_wchar>(std::tolower(static_cast<unsigned char>(ch)));
        bool changed = false;
        if (lower == 'e') {
            captureLayerEditUndoSnapshot(*selectedLayer);
            changed = ctrl.split_layer_edit_clip(*selectedLayer, *cursor);
        } else if (lower == 'a') {
            captureLayerEditUndoSnapshot(*selectedLayer);
            changed = ctrl.trim_layer_edit_left(*selectedLayer, *cursor);
        } else if (lower == 's') {
            captureLayerEditUndoSnapshot(*selectedLayer);
            changed = ctrl.trim_layer_edit_right(*selectedLayer, *cursor);
        } else if (lower == 'd') {
            captureLayerEditUndoSnapshot(*selectedLayer);
            changed = ctrl.set_layer_edit_fade_in(*selectedLayer, *cursor);
        } else if (lower == 'g') {
            captureLayerEditUndoSnapshot(*selectedLayer);
            changed = ctrl.set_layer_edit_fade_out(*selectedLayer, *cursor);
        } else if (lower == 'f') {
            const auto selection = waveform_.selectedEditRange();
            if (selection.has_value()) {
                captureLayerEditUndoSnapshot(*selectedLayer);
                double start = selection->first;
                double end = selection->second;
                const auto overview = ctrl.layer_waveform(*selectedLayer, 16);
                if (overview.has_value() && overview->reversed) {
                    const double flippedStart = 1.0 - end;
                    const double flippedEnd = 1.0 - start;
                    start = std::min(flippedStart, flippedEnd);
                    end = std::max(flippedStart, flippedEnd);
                }
                changed = ctrl.apply_layer_edit_crossfade(*selectedLayer, start, end);
                if (changed) {
                    waveform_.clearEditSelection();
                }
            }
        }

        if (changed) {
            playbackTarget_ = PlaybackTarget::FocusLayer;
            invalidateFocusWaveformCache();
            refreshUI();
        }
        return changed;
    };

    const auto modifiers = key.getModifiers();
    const auto keyCode = key.getKeyCode();
    // isCommandDown() = Cmd on macOS, Ctrl on Windows/Linux — gives the
    // platform-native undo/redo shortcut instead of literal Ctrl on Mac.
    if (modifiers.isCommandDown() &&
        (keyCode == 'z' || keyCode == 'Z' || keyCode == 'y' || keyCode == 'Y')) {
        if (modifiers.isShiftDown() || keyCode == 'y' || keyCode == 'Y') {
            return redoLayerEdit();
        }
        return undoLayerEdit();
    }

    const auto focusEditChar = key.getTextCharacter();
    if (focusEditChar != 0 && tryHandleFocusEditKey(focusEditChar)) {
        return true;
    }

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (selectedLayer.has_value()) {
            const auto selectedFade = waveform_.selectedFadeHandle();
            if (selectedFade.has_value()) {
                captureLayerEditUndoSnapshot(*selectedLayer);
            }
            if (selectedFade.has_value() &&
                ctrl.clear_layer_edit_clip_fade(*selectedLayer, selectedFade->first, selectedFade->second)) {
                waveform_.clearSelectedFadeHandle();
                playbackTarget_ = PlaybackTarget::FocusLayer;
                invalidateFocusWaveformCache();
                refreshUI();
                return true;
            }
        }
    }

    // Space: toggle audition of selected layer solo
    if (key == juce::KeyPress::spaceKey) {
        if (playbackTarget_ == PlaybackTarget::Picture) {
            if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
                auto& picture = pictureWindow_->content();
                if (picture.isPlaying()) {
                    picture.stop(false);
                } else {
                    picture.play();
                }
                pictureTransportOwnedByRecorder_ = false;
                pictureTransportOwnedByTakePlayback_ = false;
                refreshRecorderPictureControls();
            }
        } else if (ctrl.is_streaming()) {
            ctrl.stop_streaming_playback();
            if (isTakePlaying_) {
                isTakePlaying_ = false;
                sessionRecorder_.setTakePlayhead(-1.0);
                stopTakeOwnedPicturePlayback(false);
            }
        } else {
            if (playbackTarget_ == PlaybackTarget::RecorderTake) {
                auto takeIndex = ctrl.selected_session_recording_index();
                if (takeIndex.has_value() && sessionRecorder_.onPlayTake) {
                    sessionRecorder_.onPlayTake(*takeIndex);
                }
            } else {
                toggleFocusTrackPlayback();
            }
        }
        return true;
    }

    const auto textChar = key.getTextCharacter();

    if (key == juce::KeyPress::numberPadSubtract || keyCode == '-' || textChar == '-') {
        keyboard_.stepOctave(-1);
        ctrl.set_octave(keyboard_.octave());
        return true;
    }

    if (key == juce::KeyPress::numberPadAdd || keyCode == '+' || textChar == '+') {
        keyboard_.stepOctave(1);
        ctrl.set_octave(keyboard_.octave());
        return true;
    }

    if (key == juce::KeyPress::numberPad7) return triggerKeyboardNote(0);   // C
    if (key == juce::KeyPress::numberPad8) return triggerKeyboardNote(1);   // C#
    if (key == juce::KeyPress::numberPad9) return triggerKeyboardNote(2);   // D
    if (key == juce::KeyPress::numberPadDivide) return triggerKeyboardNote(3); // D#
    if (key == juce::KeyPress::numberPad4) return triggerKeyboardNote(4);   // E
    if (key == juce::KeyPress::numberPad5) return triggerKeyboardNote(5);   // F
    if (key == juce::KeyPress::numberPad6) return triggerKeyboardNote(6);   // F#
    if (key == juce::KeyPress::numberPadMultiply) return triggerKeyboardNote(7); // G
    if (key == juce::KeyPress::numberPad1) return triggerKeyboardNote(8);   // G#
    if (key == juce::KeyPress::numberPad2) return triggerKeyboardNote(9);   // A
    if (key == juce::KeyPress::numberPad3) return triggerKeyboardNote(10);  // A#
    if (key == juce::KeyPress::numberPad0) return triggerKeyboardNote(11);  // B

    // Letter-row chromatic mapping for laptop keyboards (no numpad on MacBooks).
    // Bottom row Z X C V B N M = naturals C D E F G A B
    // Row above  S D G H J     = sharps   C# D# F# G# A#
    // Skipped when a command modifier is held so Cmd-Z (undo) etc still work.
    if (!modifiers.isCommandDown() && !modifiers.isAltDown()) {
        const auto letterChar = juce::CharacterFunctions::toLowerCase(textChar);
        if (letterChar == 'z') return triggerKeyboardNote(0);   // C
        if (letterChar == 's') return triggerKeyboardNote(1);   // C#
        if (letterChar == 'x') return triggerKeyboardNote(2);   // D
        if (letterChar == 'd') return triggerKeyboardNote(3);   // D#
        if (letterChar == 'c') return triggerKeyboardNote(4);   // E
        if (letterChar == 'v') return triggerKeyboardNote(5);   // F
        if (letterChar == 'g') return triggerKeyboardNote(6);   // F#
        if (letterChar == 'b') return triggerKeyboardNote(7);   // G
        if (letterChar == 'h') return triggerKeyboardNote(8);   // G#
        if (letterChar == 'n') return triggerKeyboardNote(9);   // A
        if (letterChar == 'j') return triggerKeyboardNote(10);  // A#
        if (letterChar == 'm') return triggerKeyboardNote(11);  // B
    }

    // Up/Down arrows: navigate layer selection
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
        auto sel = ctrl.selected_layer_index();
        auto count = ctrl.layer_count();
        if (count == 0) return true;

        std::size_t next = 0;
        if (sel.has_value()) {
            if (key == juce::KeyPress::upKey && *sel > 0)
                next = *sel - 1;
            else if (key == juce::KeyPress::downKey && *sel + 1 < count)
                next = *sel + 1;
            else
                next = *sel;
        }
        ctrl.select_layer(next);
        refreshUI();
        return true;
    }

    return false;
}

bool TriggerfishEditor::keyStateChanged(bool /*isKeyDown*/) {
    const bool crossfadeHeld =
        juce::KeyPress::isKeyCurrentlyDown('f') ||
        juce::KeyPress::isKeyCurrentlyDown('F');
    waveform_.setCrossfadeSelectionMode(crossfadeHeld);
    return false;
}

void TriggerfishEditor::mouseDown(const juce::MouseEvent& e) {
    if (e.originalComponent != &waveform_ && e.eventComponent != &waveform_) {
        waveform_.clearSelectedClips();
    }
}

void TriggerfishEditor::paint(juce::Graphics& g) {
    g.fillAll(triggerfish::colours::background);

    auto expandedUnion = [](juce::Rectangle<int> area, const juce::Rectangle<int>& other) {
        if (area.isEmpty()) {
            return other;
        }
        return area.getUnion(other);
    };

    auto drawSection = [&g](juce::Rectangle<int> bounds, juce::Colour tint) {
        if (bounds.isEmpty()) {
            return;
        }
        auto panelBounds = bounds.expanded(8, 8).toFloat();
        g.setColour(tint.withAlpha(0.12f));
        g.fillRoundedRectangle(panelBounds, 10.0f);
        g.setColour(tint.withAlpha(0.18f));
        g.drawRoundedRectangle(panelBounds.reduced(0.5f), 10.0f, 1.0f);
    };

    juce::Rectangle<int> layerSection = layerList_.getBounds();

    juce::Rectangle<int> recordSection = sessionRecorder_.getBounds();
    recordSection = expandedUnion(recordSection, recorderPictureTimeLabel_.getBounds());
    recordSection = expandedUnion(recordSection, recorderPictureTimeline_.getBounds());
    recordSection = expandedUnion(recordSection, recorderPictureVolumeLabel_.getBounds());
    recordSection = expandedUnion(recordSection, recorderPictureVolume_.getBounds());
    recordSection = expandedUnion(recordSection, auxTitleLabel_.getBounds());
    recordSection = expandedUnion(recordSection, recorderBusModeBox_.getBounds());
    recordSection = expandedUnion(recordSection, auxGainLabel_.getBounds());
    recordSection = expandedUnion(recordSection, auxGain_.getBounds());
    recordSection = expandedUnion(recordSection, auxBassGainLabel_.getBounds());
    recordSection = expandedUnion(recordSection, auxBassGain_.getBounds());
    recordSection = expandedUnion(recordSection, auxMeter_.getBounds());
    recordSection = expandedUnion(recordSection, auxVst3Rack_.getBounds());

    juce::Rectangle<int> focusSection = waveform_.getBounds();
    focusSection = expandedUnion(focusSection, focusTrackFocusButton_.getBounds());
    focusSection = expandedUnion(focusSection, vst3Rack_.getBounds());
    focusSection = expandedUnion(focusSection, surroundPanner_.getBounds());
    focusSection = expandedUnion(focusSection, masterControls_.getBounds());
    focusSection = expandedUnion(focusSection, layerEqPanel_.getBounds());

    drawSection(layerSection, triggerfish::colours::sectionLayers);
    drawSection(recordSection, triggerfish::colours::sectionRecord);
    drawSection(focusSection, triggerfish::colours::sectionFocus);

    g.setColour(triggerfish::colours::textDim.withAlpha(0.55f));
    g.setFont(juce::FontOptions(10.0f));
    g.drawText(juce::String::fromUTF8("\xc2\xa9 2026 Ian Nyeste \xe2\x80\x94 Triggerfish 1.0"),
               getLocalBounds().reduced(8, 4),
               juce::Justification::bottomRight,
               false);
}

void TriggerfishEditor::resized() {
    const int previousWaveformWidth = cachedFocusWaveformWidth_;
    auto area = getLocalBounds();
    auto toolbarRow = area.removeFromTop(40);
    vstFolderButton_.setBounds(toolbarRow.removeFromRight(172).reduced(4, 6));
    toolbar_.setBounds(toolbarRow);
    keyboard_.setBounds(area.removeFromBottom(52));

    auto bottomPanel = area.removeFromBottom(100);
    auto waveformHeader = bottomPanel.removeFromTop(26);
    focusTrackFocusButton_.setBounds(waveformHeader.removeFromLeft(72).reduced(0, 2));
    const auto normalWaveformBounds = bottomPanel;

    auto rightPanel = area.removeFromRight(220);
    const auto normalVstRackBounds = rightPanel.removeFromTop(160);
    const auto normalPannerBounds = rightPanel.removeFromTop(180);
    const auto normalMasterBounds = rightPanel;

    auto recorderPictureArea = area.removeFromBottom(44);
    const auto normalRecorderPictureHeaderBounds = recorderPictureArea.removeFromTop(18);
    recorderPictureArea.removeFromTop(4);
    const auto normalRecorderTimelineBounds = recorderPictureArea.removeFromTop(18);
    recorderPictureArea.removeFromTop(4);
    auto recorderVolumeRow = recorderPictureArea.removeFromTop(20);
    const auto normalRecorderVolumeLabelBounds = recorderVolumeRow.removeFromLeft(48);
    recorderVolumeRow.removeFromLeft(8);
    const auto normalRecorderVolumeBounds = recorderVolumeRow.removeFromLeft(std::min(280, recorderVolumeRow.getWidth()));

    auto auxArea = area.removeFromBottom(136).reduced(0, 2);
    auto auxHeader = auxArea.removeFromTop(16);
    const auto normalAuxSurroundBounds = auxHeader.removeFromRight(62);
    const auto normalAuxHeaderBounds = auxHeader;
    auto auxContent = auxArea;
    const int auxRackWidth = std::clamp(auxContent.getWidth() / 3, 168, 250);
    auto auxRackArea = auxContent.removeFromRight(auxRackWidth);
    auxContent.removeFromRight(10);
    auto auxControls = auxContent;
    auto auxSliderRow = auxControls.removeFromBottom(24);
    const auto normalAuxMeterBounds = auxControls.reduced(0, 0);
    auxSliderRow.removeFromTop(3);
    auto auxGainCell = auxSliderRow.removeFromLeft((auxSliderRow.getWidth() - 10) / 2);
    auxSliderRow.removeFromLeft(10);
    auto auxBassCell = auxSliderRow;
    const auto normalAuxGainLabelBounds = auxGainCell.removeFromLeft(48);
    auxGainCell.removeFromLeft(4);
    const auto normalAuxGainBounds = auxGainCell;
    const auto normalAuxBassGainLabelBounds = auxBassCell.removeFromLeft(58);
    auxBassCell.removeFromLeft(4);
    const auto normalAuxBassGainBounds = auxBassCell;
    const auto normalAuxRackBounds = auxRackArea;

    // Session recorder sits between layers and the bottom waveform panel
    auto recorderArea = area.removeFromBottom(120);
    const auto normalRecorderBounds = recorderArea;

    const auto normalLayerBounds = area;

    auto applyNormalLayout = [&] {
        dimOverlay_.setVisible(false);
        layerList_.setBounds(normalLayerBounds);
        waveform_.setBounds(normalWaveformBounds);
        vst3Rack_.setBounds(normalVstRackBounds);
        surroundPanner_.setBounds(normalPannerBounds);
        masterControls_.setBounds(normalMasterBounds);
        recorderPictureTimeLabel_.setBounds(normalRecorderPictureHeaderBounds);
        recorderPictureTimeline_.setBounds(normalRecorderTimelineBounds);
        recorderPictureVolumeLabel_.setBounds(normalRecorderVolumeLabelBounds);
        recorderPictureVolume_.setBounds(normalRecorderVolumeBounds);
        auxTitleLabel_.setBounds(normalAuxHeaderBounds);
        recorderBusModeBox_.setBounds(normalAuxSurroundBounds);
        auxMeter_.setBounds(normalAuxMeterBounds);
        auxGainLabel_.setBounds(normalAuxGainLabelBounds);
        auxGain_.setBounds(normalAuxGainBounds);
        auxBassGainLabel_.setBounds(normalAuxBassGainLabelBounds);
        auxBassGain_.setBounds(normalAuxBassGainBounds);
        auxVst3Rack_.setBounds(normalAuxRackBounds);
        sessionRecorder_.setBounds(normalRecorderBounds);
        focusTrackFocusButton_.setBounds(waveformHeader.getX(), waveformHeader.getY(), 72, 24);
        addAutomationButton_.setBounds(juce::Rectangle<int>());
        volumeRandomControlsPanel_.setBounds(juce::Rectangle<int>());
        volumeRandomizeToggle_.setBounds(juce::Rectangle<int>());
        volumeRandomLoudestLabel_.setBounds(juce::Rectangle<int>());
        volumeRandomLoudest_.setBounds(juce::Rectangle<int>());
        volumeRandomQuietestLabel_.setBounds(juce::Rectangle<int>());
        volumeRandomQuietest_.setBounds(juce::Rectangle<int>());
        volumeRandomPeriodLongestLabel_.setBounds(juce::Rectangle<int>());
        volumeRandomPeriodLongest_.setBounds(juce::Rectangle<int>());
        volumeRandomPeriodShortestLabel_.setBounds(juce::Rectangle<int>());
        volumeRandomPeriodShortest_.setBounds(juce::Rectangle<int>());
        volumeRandomSmoothingLabel_.setBounds(juce::Rectangle<int>());
        volumeRandomSmoothing_.setBounds(juce::Rectangle<int>());
        panRandomControlsPanel_.setBounds(juce::Rectangle<int>());
        panRandomizeToggle_.setBounds(juce::Rectangle<int>());
        panRandomLeftLabel_.setBounds(juce::Rectangle<int>());
        panRandomLeft_.setBounds(juce::Rectangle<int>());
        panRandomRightLabel_.setBounds(juce::Rectangle<int>());
        panRandomRight_.setBounds(juce::Rectangle<int>());
        panRandomFrontLabel_.setBounds(juce::Rectangle<int>());
        panRandomFront_.setBounds(juce::Rectangle<int>());
        panRandomBackLabel_.setBounds(juce::Rectangle<int>());
        panRandomBack_.setBounds(juce::Rectangle<int>());
        panRandomSpeedLabel_.setBounds(juce::Rectangle<int>());
        panRandomSpeed_.setBounds(juce::Rectangle<int>());
        panRandomSmoothingLabel_.setBounds(juce::Rectangle<int>());
        panRandomSmoothing_.setBounds(juce::Rectangle<int>());
        stretchRandomControlsPanel_.setBounds(juce::Rectangle<int>());
        stretchRandomizeToggle_.setBounds(juce::Rectangle<int>());
        stretchRandomLowestLabel_.setBounds(juce::Rectangle<int>());
        stretchRandomLowest_.setBounds(juce::Rectangle<int>());
        stretchRandomHighestLabel_.setBounds(juce::Rectangle<int>());
        stretchRandomHighest_.setBounds(juce::Rectangle<int>());
        stretchRandomSpeedLabel_.setBounds(juce::Rectangle<int>());
        stretchRandomSpeed_.setBounds(juce::Rectangle<int>());
        stretchRandomSmoothingLabel_.setBounds(juce::Rectangle<int>());
        stretchRandomSmoothing_.setBounds(juce::Rectangle<int>());
        dopplerControlsPanel_.setBounds(juce::Rectangle<int>());
        dopplerEdgeGainLabel_.setBounds(juce::Rectangle<int>());
        dopplerEdgeGain_.setBounds(juce::Rectangle<int>());
        dopplerCenterGainLabel_.setBounds(juce::Rectangle<int>());
        dopplerCenterGain_.setBounds(juce::Rectangle<int>());
        dopplerEdgePitchLabel_.setBounds(juce::Rectangle<int>());
        dopplerEdgePitch_.setBounds(juce::Rectangle<int>());
        dopplerCenterPitchLabel_.setBounds(juce::Rectangle<int>());
        dopplerCenterPitch_.setBounds(juce::Rectangle<int>());
        dopplerDrawModeToggle_.setBounds(juce::Rectangle<int>());
        dopplerCurveTypeLabel_.setBounds(juce::Rectangle<int>());
        dopplerCurveType_.setBounds(juce::Rectangle<int>());
        dopplerCurveAmountLabel_.setBounds(juce::Rectangle<int>());
        dopplerCurveAmount_.setBounds(juce::Rectangle<int>());
        layerEqPanel_.setBounds(juce::Rectangle<int>());
        layerEqLowLabel_.setBounds(juce::Rectangle<int>());
        layerEqLow_.setBounds(juce::Rectangle<int>());
        layerEqMidLabel_.setBounds(juce::Rectangle<int>());
        layerEqMid_.setBounds(juce::Rectangle<int>());
        layerEqHighLabel_.setBounds(juce::Rectangle<int>());
        layerEqHigh_.setBounds(juce::Rectangle<int>());
        focusAutomationTargetLabel_.setBounds(juce::Rectangle<int>());
    };

    applyNormalLayout();

    if (focusedModule_ != FocusModule::None) {
        const auto overlayBounds = getLocalBounds();
        dimOverlay_.setBounds(overlayBounds);
        dimOverlay_.setVisible(true);
        dimOverlay_.toFront(false);

        const auto focusArea = getLocalBounds().reduced(70, 60);
        if (focusedModule_ == FocusModule::FocusTrack) {
            auto focusTrackArea = focusArea;
            auto header = focusTrackArea.removeFromTop(28);
            focusTrackFocusButton_.setBounds(header.removeFromLeft(72).reduced(0, 2));
            header.removeFromLeft(8);
            addAutomationButton_.setBounds(header.removeFromLeft(132).reduced(0, 2));
            header.removeFromLeft(10);
            focusAutomationTargetLabel_.setBounds(header.removeFromLeft(170).reduced(0, 2));
            auto topWaveformArea = focusTrackArea.removeFromTop(
                juce::jmin(180, juce::jmax(120, focusTrackArea.getHeight() / 3)));
            waveform_.setBounds(topWaveformArea);

            const bool showVolumeRandomControls = focusAutomationTarget_ == FocusAutomationTarget::Volume;
            const bool showStretchRandomControls = focusAutomationTarget_ == FocusAutomationTarget::Stretch;
            const bool showPanRandomControls =
                focusAutomationTarget_ == FocusAutomationTarget::PanPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanFrontBack ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightPosition ||
                focusAutomationTarget_ == FocusAutomationTarget::PanRightFrontBack;
            const bool showDopplerControls = focusAutomationTarget_ == FocusAutomationTarget::Doppler;
            if (showVolumeRandomControls || showStretchRandomControls || showPanRandomControls || showDopplerControls) {
                focusTrackArea.removeFromTop(12);
                const int automationHeight = showPanRandomControls
                    ? juce::jmin(224, juce::jmax(196, focusTrackArea.getHeight() / 3))
                    : juce::jmin(164, juce::jmax(132, focusTrackArea.getHeight() / 3));
                auto automationArea = focusTrackArea.removeFromTop(automationHeight);
                auto layoutPair = [](juce::Rectangle<int> area, juce::Label& label, juce::Slider& slider) {
                    auto labelArea = area.removeFromLeft(88);
                    label.setBounds(labelArea);
                    slider.setBounds(area);
                };
                const int controlGap = 10;
                if (showVolumeRandomControls) {
                    volumeRandomControlsPanel_.setBounds(automationArea);
                    auto inner = automationArea.reduced(10, 22);
                    auto toggleRow = inner.removeFromTop(24);
                    inner.removeFromTop(8);
                    auto topRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto middleRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto bottomRow = inner.removeFromTop(26);

                    volumeRandomizeToggle_.setBounds(toggleRow.removeFromLeft(120));
                    auto topCell = topRow.removeFromLeft((topRow.getWidth() - controlGap) / 2);
                    topRow.removeFromLeft(controlGap);
                    auto topCell2 = topRow;
                    auto middleCell = middleRow.removeFromLeft((middleRow.getWidth() - controlGap) / 2);
                    middleRow.removeFromLeft(controlGap);
                    auto middleCell2 = middleRow;
                    layoutPair(topCell, volumeRandomLoudestLabel_, volumeRandomLoudest_);
                    layoutPair(topCell2, volumeRandomQuietestLabel_, volumeRandomQuietest_);
                    layoutPair(middleCell, volumeRandomPeriodLongestLabel_, volumeRandomPeriodLongest_);
                    layoutPair(middleCell2, volumeRandomPeriodShortestLabel_, volumeRandomPeriodShortest_);
                    layoutPair(bottomRow, volumeRandomSmoothingLabel_, volumeRandomSmoothing_);
                } else if (showStretchRandomControls) {
                    stretchRandomControlsPanel_.setBounds(automationArea);
                    auto inner = automationArea.reduced(10, 22);
                    auto toggleRow = inner.removeFromTop(24);
                    inner.removeFromTop(8);
                    auto topRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto middleRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto bottomRow = inner.removeFromTop(26);

                    stretchRandomizeToggle_.setBounds(toggleRow.removeFromLeft(120));
                    auto topCell = topRow.removeFromLeft((topRow.getWidth() - controlGap) / 2);
                    topRow.removeFromLeft(controlGap);
                    auto topCell2 = topRow;
                    auto middleCell = middleRow.removeFromLeft((middleRow.getWidth() - controlGap) / 2);
                    middleRow.removeFromLeft(controlGap);
                    auto middleCell2 = middleRow;
                    layoutPair(topCell, stretchRandomLowestLabel_, stretchRandomLowest_);
                    layoutPair(topCell2, stretchRandomHighestLabel_, stretchRandomHighest_);
                    layoutPair(middleCell, stretchRandomSpeedLabel_, stretchRandomSpeed_);
                    layoutPair(middleCell2, stretchRandomSmoothingLabel_, stretchRandomSmoothing_);
                    bottomRow = juce::Rectangle<int>();
                } else if (showPanRandomControls) {
                    panRandomControlsPanel_.setBounds(automationArea);
                    auto inner = automationArea.reduced(10, 14);
                    auto toggleRow = inner.removeFromTop(24);
                    inner.removeFromTop(8);
                    auto topRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto middleRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto bottomRow = inner.removeFromTop(26);
                    inner.removeFromTop(8);
                    auto finalRow = inner.removeFromTop(26);

                    panRandomizeToggle_.setBounds(toggleRow.removeFromLeft(120));
                    auto topCell = topRow.removeFromLeft((topRow.getWidth() - controlGap) / 2);
                    topRow.removeFromLeft(controlGap);
                    auto topCell2 = topRow;
                    auto middleCell = middleRow.removeFromLeft((middleRow.getWidth() - controlGap) / 2);
                    middleRow.removeFromLeft(controlGap);
                    auto middleCell2 = middleRow;
                    layoutPair(topCell, panRandomLeftLabel_, panRandomLeft_);
                    layoutPair(topCell2, panRandomRightLabel_, panRandomRight_);
                    layoutPair(middleCell, panRandomFrontLabel_, panRandomFront_);
                    layoutPair(middleCell2, panRandomBackLabel_, panRandomBack_);
                    layoutPair(bottomRow, panRandomSpeedLabel_, panRandomSpeed_);
                    layoutPair(finalRow, panRandomSmoothingLabel_, panRandomSmoothing_);
                } else {
                    dopplerControlsPanel_.setBounds(automationArea);
                    auto dopplerInner = automationArea.reduced(10, 22);
                    auto topRow = dopplerInner.removeFromTop(26);
                    dopplerInner.removeFromTop(8);
                    auto bottomRowControls = dopplerInner.removeFromTop(26);
                    dopplerInner.removeFromTop(8);
                    auto segmentRow = dopplerInner.removeFromTop(26);
                    auto topCell = topRow.removeFromLeft((topRow.getWidth() - controlGap) / 2);
                    topRow.removeFromLeft(controlGap);
                    auto topCell2 = topRow;
                    auto bottomCell = bottomRowControls.removeFromLeft((bottomRowControls.getWidth() - controlGap) / 2);
                    bottomRowControls.removeFromLeft(controlGap);
                    auto bottomCell2 = bottomRowControls;
                    layoutPair(topCell, dopplerEdgeGainLabel_, dopplerEdgeGain_);
                    layoutPair(topCell2, dopplerCenterGainLabel_, dopplerCenterGain_);
                    layoutPair(bottomCell, dopplerEdgePitchLabel_, dopplerEdgePitch_);
                    layoutPair(bottomCell2, dopplerCenterPitchLabel_, dopplerCenterPitch_);
                    auto drawCell = segmentRow.removeFromLeft(108);
                    dopplerDrawModeToggle_.setBounds(drawCell.removeFromTop(22));
                    segmentRow.removeFromLeft(controlGap);
                    auto segmentCell = segmentRow.removeFromLeft((segmentRow.getWidth() - controlGap) / 2);
                    segmentRow.removeFromLeft(controlGap);
                    auto segmentCell2 = segmentRow;
                    auto curveLabelArea = segmentCell.removeFromLeft(88);
                    dopplerCurveTypeLabel_.setBounds(curveLabelArea);
                    dopplerCurveType_.setBounds(segmentCell);
                    layoutPair(segmentCell2, dopplerCurveAmountLabel_, dopplerCurveAmount_);
                }
                focusTrackArea.removeFromTop(12);
            }

            focusTrackArea.removeFromTop(8);
            const int eqHeight = juce::jmin(132, juce::jmax(104, focusTrackArea.getHeight() / 3));
            auto eqArea = focusTrackArea.removeFromTop(eqHeight);
            layerEqPanel_.setBounds(eqArea);
            auto eqInner = eqArea.reduced(10, 18);
            eqInner.removeFromTop(2);
            auto layoutEqRow = [](juce::Rectangle<int> row, juce::Label& label, juce::Slider& slider) {
                label.setBounds(row.removeFromLeft(58));
                row.removeFromLeft(8);
                slider.setBounds(row);
            };
            auto lowRow = eqInner.removeFromTop(20);
            eqInner.removeFromTop(4);
            auto midRow = eqInner.removeFromTop(20);
            eqInner.removeFromTop(4);
            auto highRow = eqInner.removeFromTop(20);
            layoutEqRow(lowRow, layerEqLowLabel_, layerEqLow_);
            layoutEqRow(midRow, layerEqMidLabel_, layerEqMid_);
            layoutEqRow(highRow, layerEqHighLabel_, layerEqHigh_);

            focusTrackArea.removeFromTop(8);
            auto bottomRow = focusTrackArea.removeFromBottom(
                juce::jmin(230, juce::jmax(170, focusTrackArea.getHeight() / 2)));
            const int sectionGap = 16;
            auto pannerArea = bottomRow.removeFromLeft(bottomRow.getWidth() / 2);
            bottomRow.removeFromLeft(sectionGap);
            auto layerFxArea = bottomRow;

            surroundPanner_.setBounds(pannerArea);
            masterControls_.setBounds(layerFxArea);
            vst3Rack_.setBounds(juce::Rectangle<int>());

            waveform_.toFront(false);
            surroundPanner_.toFront(false);
            masterControls_.toFront(false);
            focusTrackFocusButton_.toFront(false);
            addAutomationButton_.toFront(false);
            focusAutomationTargetLabel_.toFront(false);
            layerEqPanel_.toFront(false);
            layerEqLowLabel_.toFront(false);
            layerEqLow_.toFront(false);
            layerEqMidLabel_.toFront(false);
            layerEqMid_.toFront(false);
            layerEqHighLabel_.toFront(false);
            layerEqHigh_.toFront(false);
            if (showVolumeRandomControls) {
                volumeRandomControlsPanel_.toFront(false);
                volumeRandomizeToggle_.toFront(false);
                volumeRandomLoudestLabel_.toFront(false);
                volumeRandomLoudest_.toFront(false);
                volumeRandomQuietestLabel_.toFront(false);
                volumeRandomQuietest_.toFront(false);
                volumeRandomPeriodLongestLabel_.toFront(false);
                volumeRandomPeriodLongest_.toFront(false);
                volumeRandomPeriodShortestLabel_.toFront(false);
                volumeRandomPeriodShortest_.toFront(false);
                volumeRandomSmoothingLabel_.toFront(false);
                volumeRandomSmoothing_.toFront(false);
            }
            if (showPanRandomControls) {
                panRandomControlsPanel_.toFront(false);
                panRandomizeToggle_.toFront(false);
                panRandomLeftLabel_.toFront(false);
                panRandomLeft_.toFront(false);
                panRandomRightLabel_.toFront(false);
                panRandomRight_.toFront(false);
                panRandomFrontLabel_.toFront(false);
                panRandomFront_.toFront(false);
                panRandomBackLabel_.toFront(false);
                panRandomBack_.toFront(false);
                panRandomSpeedLabel_.toFront(false);
                panRandomSpeed_.toFront(false);
                panRandomSmoothingLabel_.toFront(false);
                panRandomSmoothing_.toFront(false);
            }
            if (showStretchRandomControls) {
                stretchRandomControlsPanel_.toFront(false);
                stretchRandomizeToggle_.toFront(false);
                stretchRandomLowestLabel_.toFront(false);
                stretchRandomLowest_.toFront(false);
                stretchRandomHighestLabel_.toFront(false);
                stretchRandomHighest_.toFront(false);
                stretchRandomSpeedLabel_.toFront(false);
                stretchRandomSpeed_.toFront(false);
                stretchRandomSmoothingLabel_.toFront(false);
                stretchRandomSmoothing_.toFront(false);
            }
            if (showDopplerControls) {
                dopplerControlsPanel_.toFront(false);
                dopplerEdgeGainLabel_.toFront(false);
                dopplerEdgeGain_.toFront(false);
                dopplerCenterGainLabel_.toFront(false);
                dopplerCenterGain_.toFront(false);
                dopplerEdgePitchLabel_.toFront(false);
                dopplerEdgePitch_.toFront(false);
                dopplerCenterPitchLabel_.toFront(false);
                dopplerCenterPitch_.toFront(false);
                dopplerDrawModeToggle_.toFront(false);
                dopplerCurveTypeLabel_.toFront(false);
                dopplerCurveType_.toFront(false);
                dopplerCurveAmountLabel_.toFront(false);
                dopplerCurveAmount_.toFront(false);
            }
        }
    }

    focusLayoutDirty_ = false;
    cachedLayoutFocusedModule_ = focusedModule_;
    cachedLayoutAutomationTarget_ = focusAutomationTarget_;
    if (waveform_.getWidth() > 0 && waveform_.getWidth() * 2 != previousWaveformWidth) {
        focusWaveformDirty_ = true;
    }
}

void TriggerfishEditor::timerCallback() {
    auto& ctrl = processorRef.controller();
    if (isStandalone_) {
        if (++standaloneMidiPollCounter_ >= 30) {
            standaloneMidiPollCounter_ = 0;
            enableStandaloneMidiInputs();
        }
        const auto midiCounter = processorRef.midiActivityCounter();
        if (midiCounter != lastMidiActivityCounter_) {
            lastMidiActivityCounter_ = midiCounter;
            lastMidiActivityAt_ = std::chrono::steady_clock::now();
        }
        const bool midiRecentlyActive =
            lastMidiActivityCounter_ > 0 &&
            (std::chrono::steady_clock::now() - lastMidiActivityAt_) < std::chrono::milliseconds(1500);
        toolbar_.setMidiStatus(midiRecentlyActive);
    }
    if (!ctrl.is_streaming() && trackAutomationWriteActive_) {
        resetTrackAutomationWriteState();
    }
    if (pictureWindow_ && pictureTransportOwnedByRecorder_ &&
        !pictureWindow_->content().isPlaying()) {
        pictureTransportOwnedByRecorder_ = false;
    }
    if (pictureWindow_ && pictureTransportOwnedByTakePlayback_ &&
        !pictureWindow_->content().isPlaying()) {
        pictureTransportOwnedByTakePlayback_ = false;
    }

    if (punchInActive_) {
        const double elapsedSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - punchInStartedAt_).count();
        double transportPos = punchInCueStart_;
        if (punchInTakeDurationSeconds_ > 0.0) {
            transportPos += elapsedSeconds / punchInTakeDurationSeconds_;
        }
        if (transportPos >= 1.0) {
            transportPos = 1.0;
            finalizePunchRecording();
            refreshUI();
            return;
        }
        sessionRecorder_.setTakePlayhead(transportPos);
        if (activePunchRegion_.has_value()) {
            sessionRecorder_.updatePunchPreview(ctrl.streaming_mixer(), punchInCueStart_,
                                                *activePunchRegion_, transportPos,
                                                punchInTakeDurationSeconds_);
        }
    }

    if (ctrl.is_streaming()) {
        // Check if the mixer has actually finished (all voices done)
        // Don't auto-stop while session recording is armed — we need the
        // mixer to stay alive so it keeps capturing silence between triggers.
        if (!ctrl.streaming_mixer().is_playing() && !ctrl.session_recording_armed()) {
            ctrl.stop_streaming_playback();
            waveform_.setPlayheadPosition(-1.0);
            layerList_.updatePlayheadPositions(ctrl);
            if (isTakePlaying_) {
                isTakePlaying_ = false;
                sessionRecorder_.setTakePlayhead(-1.0);
                stopTakeOwnedPicturePlayback(false);
            }
            refreshRecorderPictureControls();
            return;
        }

        if (isTakePlaying_) {
            // During take playback, drive the recorder waveform playhead
            auto pos = ctrl.layer_streaming_position(0);
            sessionRecorder_.setTakePlayhead(pos.value_or(-1.0));
        } else {
            // Update playhead on focused waveform
            auto sel = ctrl.selected_layer_index();
            if (sel.has_value()) {
                auto pos = ctrl.layer_streaming_position(*sel);
                waveform_.setPlayheadPosition(pos.value_or(-1.0));
            }

            // Lightweight update: only playhead positions, no waveform recomputation
            layerList_.updatePlayheadPositions(ctrl);
        }
    }

    // Update session recorder waveform during recording
    if (ctrl.session_recording_armed() && newRecordingActive_) {
        sessionRecorder_.updateRecordingPeaks(ctrl.streaming_mixer());
    }

    std::vector<float> auxLevels;
    ctrl.streaming_mixer().aux_meter_levels(auxLevels);
    auxMeter_.setLevels(auxLevels);

    refreshRecorderPictureControls();
}

void TriggerfishEditor::invalidateFocusWaveformCache() {
    focusWaveformDirty_ = true;
}

void TriggerfishEditor::refreshUI() {
    layerList_.updateFromController(processorRef.controller());
    waveform_.setAutomationLaneVisible(focusedModule_ == FocusModule::FocusTrack);
    waveform_.setAutomationFreehandDrawEnabled(
        focusedModule_ == FocusModule::FocusTrack &&
        focusAutomationTarget_ == FocusAutomationTarget::Doppler &&
        dopplerDrawModeToggle_.getToggleState());
    focusAutomationTargetLabel_.setText(currentAutomationTargetLabel(), juce::dontSendNotification);
    focusAutomationTargetLabel_.setVisible(focusedModule_ == FocusModule::FocusTrack);
    if (!(focusedModule_ == FocusModule::FocusTrack &&
          focusAutomationTarget_ == FocusAutomationTarget::Doppler)) {
        selectedDopplerSegmentLeftPointId_.reset();
        waveform_.clearSelectedAutomationSegment();
    }
    if (focusedModule_ == FocusModule::FocusTrack) {
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Stretch);
                break;
            case FocusAutomationTarget::Doppler:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Doppler);
                break;
            case FocusAutomationTarget::PanPosition:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanPosition);
                break;
            case FocusAutomationTarget::PanFrontBack:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanFrontBack);
                break;
            case FocusAutomationTarget::PanRightPosition:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanRightPosition);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanRightFrontBack);
                break;
            case FocusAutomationTarget::Volume:
            default:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Volume);
                break;
        }
    } else {
        waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::None);
    }

    const bool needsLayoutRefresh =
        focusLayoutDirty_ ||
        focusedModule_ != cachedLayoutFocusedModule_ ||
        focusAutomationTarget_ != cachedLayoutAutomationTarget_;
    if (needsLayoutRefresh) {
        resized();
    }

    // Update waveform and VST3 rack for selected layer
    auto sel = processorRef.controller().selected_layer_index();
    if (sel.has_value()) {
        refreshFocusWaveform();
        ensureLayerHosts(*sel);
        vst3Rack_.setHosts(&layerHosts_[*sel],
                           processorRef.getSampleRate(),
                           processorRef.getBlockSize());
        vst3Rack_.updateDisplay(processorRef.controller(), *sel);
        wirePluginSessions();

        // Update surround panner
        const auto& overrides = processorRef.controller().layer_overrides();
        if (*sel < overrides.size()) {
            const auto& ov = overrides[*sel];
            surroundPanner_.setPan(ov.pan_x, ov.pan_y);
            surroundPanner_.setPanRight(ov.pan_x_right, ov.pan_y_right);
            surroundPanner_.setStereo(processorRef.controller().layer_is_stereo(*sel));
            masterControls_.setTrackGain(static_cast<float>(ov.gain));
            masterControls_.setTrackGainEnabled(true);
            const bool volumeAutomated = processorRef.controller().layer_volume_automation_enabled(*sel);
            const bool stretchAutomated = processorRef.controller().layer_stretch_automation_enabled(*sel);
            const bool leftPanAutomated =
                processorRef.controller().layer_pan_automation_enabled(
                    *sel, radium::AppController::PanAutomationTarget::Position) ||
                processorRef.controller().layer_pan_automation_enabled(
                    *sel, radium::AppController::PanAutomationTarget::FrontBack);
            const bool rightPanAutomated =
                processorRef.controller().layer_pan_automation_enabled(
                    *sel, radium::AppController::PanAutomationTarget::RightPosition) ||
                processorRef.controller().layer_pan_automation_enabled(
                    *sel, radium::AppController::PanAutomationTarget::RightFrontBack);
            masterControls_.setTrackGainAutomated(volumeAutomated);
            masterControls_.setStretchAutomated(stretchAutomated);
            surroundPanner_.setAutomationHighlight(leftPanAutomated, rightPanAutomated);
        }
        // Update effect controls
        auto fx = processorRef.controller().layer_effect_state(*sel);
        if (fx.has_value()) {
            masterControls_.setEffectState(fx->reverse, fx->time_stretch_ratio);
            masterControls_.setBassLfeGainDb(fx->bass_lfe_gain_db);
            layerEqLow_.setValue(fx->eq_low_gain_db, juce::dontSendNotification);
            layerEqMid_.setValue(fx->eq_mid_gain_db, juce::dontSendNotification);
            layerEqHigh_.setValue(fx->eq_high_gain_db, juce::dontSendNotification);
        }
    } else {
        waveform_.clearWaveform();
        cachedFocusWaveformLayerIndex_.reset();
        cachedFocusWaveformRevision_ = 0;
        cachedFocusWaveformWidth_ = -1;
        focusWaveformDirty_ = false;
        vst3Rack_.clearDisplay();
        masterControls_.setTrackGain(1.0f);
        masterControls_.setBassLfeGainDb(0.0);
        layerEqLow_.setValue(0.0, juce::dontSendNotification);
        layerEqMid_.setValue(0.0, juce::dontSendNotification);
        layerEqHigh_.setValue(0.0, juce::dontSendNotification);
        masterControls_.setTrackGainEnabled(false);
        masterControls_.setTrackGainAutomated(false);
        masterControls_.setStretchAutomated(false);
        surroundPanner_.setAutomationHighlight(false, false);
    }

    // Always update master-level controls
    toolbar_.setMasterGain(processorRef.masterGain());
    keyboard_.setLoopMode(processorRef.controller().trigger_mode() == radium::TriggerMode::kContinuous);
    ensureAuxHosts();
    auxVst3Rack_.setHosts(&auxHosts_,
                          processorRef.getSampleRate(),
                          processorRef.getBlockSize());
    auxVst3Rack_.updateDisplay(processorRef.controller().aux_vst3_plugins());
    auxGain_.setValue(processorRef.controller().aux_gain(), juce::dontSendNotification);
    auxBassGain_.setValue(processorRef.controller().aux_bass_gain_db(), juce::dontSendNotification);
    wirePluginSessions();
    sessionRecorder_.updateFromController(processorRef.controller());
    sessionRecorder_.setRecordingState(newRecordingActive_ || punchInActive_, punchInActive_);
    syncRecorderSurroundModeFromSelection();
    refreshRecorderPictureControls();
    refreshVolumeRandomControls();
    refreshPanRandomControls();
    refreshStretchRandomControls();
    refreshDopplerControls();
}

void TriggerfishEditor::refreshFocusWaveform() {
    const auto sel = processorRef.controller().selected_layer_index();
    if (!sel.has_value()) {
        waveform_.clearWaveform();
        cachedFocusWaveformLayerIndex_.reset();
        cachedFocusWaveformRevision_ = 0;
        cachedFocusWaveformWidth_ = -1;
        focusWaveformDirty_ = false;
        return;
    }

    const int buckets = std::max(512, waveform_.getWidth() * 2);
    const auto revision = processorRef.controller().layer_waveform_revision(*sel);
    if (!focusWaveformDirty_ &&
        cachedFocusWaveformLayerIndex_.has_value() &&
        *cachedFocusWaveformLayerIndex_ == *sel &&
        cachedFocusWaveformRevision_ == revision &&
        cachedFocusWaveformWidth_ == buckets) {
        return;
    }
    auto overview = processorRef.controller().layer_waveform(*sel, static_cast<std::size_t>(buckets));
    if (overview.has_value()) {
        waveform_.setWaveform(*overview);
        cachedFocusWaveformLayerIndex_ = *sel;
        cachedFocusWaveformRevision_ = revision;
        cachedFocusWaveformWidth_ = buckets;
        focusWaveformDirty_ = false;
    } else {
        waveform_.clearWaveform();
        cachedFocusWaveformLayerIndex_ = *sel;
        cachedFocusWaveformRevision_ = revision;
        cachedFocusWaveformWidth_ = buckets;
        focusWaveformDirty_ = false;
    }
}

void TriggerfishEditor::refreshVolumeRandomControls() {
    const bool visible =
        focusedModule_ == FocusModule::FocusTrack &&
        focusAutomationTarget_ == FocusAutomationTarget::Volume &&
        processorRef.controller().selected_layer_index().has_value();
    volumeRandomControlsPanel_.setVisible(visible);
    volumeRandomizeToggle_.setVisible(visible);
    volumeRandomLoudestLabel_.setVisible(visible);
    volumeRandomLoudest_.setVisible(visible);
    volumeRandomQuietestLabel_.setVisible(visible);
    volumeRandomQuietest_.setVisible(visible);
    volumeRandomPeriodLongestLabel_.setVisible(visible);
    volumeRandomPeriodLongest_.setVisible(visible);
    volumeRandomPeriodShortestLabel_.setVisible(visible);
    volumeRandomPeriodShortest_.setVisible(visible);
    volumeRandomSmoothingLabel_.setVisible(visible);
    volumeRandomSmoothing_.setVisible(visible);

    if (!visible) {
        return;
    }

    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return;
    }

    const auto settings =
        processorRef.controller().layer_volume_random_settings(*selectedLayer).value_or(radium::VolumeRandomSettings{});
    suppressVolumeRandomControls_ = true;
    volumeRandomizeToggle_.setToggleState(settings.enabled, juce::dontSendNotification);
    volumeRandomLoudest_.setValue(settings.loudest_db, juce::dontSendNotification);
    volumeRandomQuietest_.setValue(settings.quietest_db, juce::dontSendNotification);
    volumeRandomPeriodLongest_.setValue(settings.period_longest_seconds, juce::dontSendNotification);
    volumeRandomPeriodShortest_.setValue(settings.period_shortest_seconds, juce::dontSendNotification);
    volumeRandomSmoothing_.setValue(settings.smoothing, juce::dontSendNotification);
    suppressVolumeRandomControls_ = false;
}

void TriggerfishEditor::refreshPanRandomControls() {
    const bool visible =
        focusedModule_ == FocusModule::FocusTrack &&
        (focusAutomationTarget_ == FocusAutomationTarget::PanPosition ||
         focusAutomationTarget_ == FocusAutomationTarget::PanFrontBack ||
         focusAutomationTarget_ == FocusAutomationTarget::PanRightPosition ||
         focusAutomationTarget_ == FocusAutomationTarget::PanRightFrontBack) &&
        processorRef.controller().selected_layer_index().has_value();
    panRandomControlsPanel_.setVisible(visible);
    panRandomizeToggle_.setVisible(visible);
    panRandomLeftLabel_.setVisible(visible);
    panRandomLeft_.setVisible(visible);
    panRandomRightLabel_.setVisible(visible);
    panRandomRight_.setVisible(visible);
    panRandomFrontLabel_.setVisible(visible);
    panRandomFront_.setVisible(visible);
    panRandomBackLabel_.setVisible(visible);
    panRandomBack_.setVisible(visible);
    panRandomSpeedLabel_.setVisible(visible);
    panRandomSpeed_.setVisible(visible);
    panRandomSmoothingLabel_.setVisible(visible);
    panRandomSmoothing_.setVisible(visible);

    if (!visible) {
        return;
    }

    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return;
    }

    const auto settings =
        processorRef.controller().layer_pan_random_settings(*selectedLayer).value_or(radium::PanRandomSettings{});
    suppressPanRandomControls_ = true;
    panRandomizeToggle_.setToggleState(settings.enabled, juce::dontSendNotification);
    panRandomLeft_.setValue(settings.farthest_left, juce::dontSendNotification);
    panRandomRight_.setValue(settings.farthest_right, juce::dontSendNotification);
    panRandomFront_.setValue(settings.farthest_front, juce::dontSendNotification);
    panRandomBack_.setValue(settings.farthest_back, juce::dontSendNotification);
    panRandomSpeed_.setValue(settings.speed, juce::dontSendNotification);
    panRandomSmoothing_.setValue(settings.smoothing, juce::dontSendNotification);
    suppressPanRandomControls_ = false;
}

void TriggerfishEditor::refreshStretchRandomControls() {
    const bool visible =
        focusedModule_ == FocusModule::FocusTrack &&
        focusAutomationTarget_ == FocusAutomationTarget::Stretch &&
        processorRef.controller().selected_layer_index().has_value();
    stretchRandomControlsPanel_.setVisible(visible);
    stretchRandomizeToggle_.setVisible(visible);
    stretchRandomLowestLabel_.setVisible(visible);
    stretchRandomLowest_.setVisible(visible);
    stretchRandomHighestLabel_.setVisible(visible);
    stretchRandomHighest_.setVisible(visible);
    stretchRandomSpeedLabel_.setVisible(visible);
    stretchRandomSpeed_.setVisible(visible);
    stretchRandomSmoothingLabel_.setVisible(visible);
    stretchRandomSmoothing_.setVisible(visible);

    if (!visible) {
        return;
    }

    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return;
    }

    const auto settings =
        processorRef.controller().layer_stretch_random_settings(*selectedLayer).value_or(radium::StretchRandomSettings{});
    suppressStretchRandomControls_ = true;
    stretchRandomizeToggle_.setToggleState(settings.enabled, juce::dontSendNotification);
    stretchRandomLowest_.setValue(settings.lowest_percent, juce::dontSendNotification);
    stretchRandomHighest_.setValue(settings.highest_percent, juce::dontSendNotification);
    stretchRandomSpeed_.setValue(settings.speed, juce::dontSendNotification);
    stretchRandomSmoothing_.setValue(settings.smoothing, juce::dontSendNotification);
    suppressStretchRandomControls_ = false;
}

void TriggerfishEditor::refreshDopplerControls() {
    const bool visible =
        focusedModule_ == FocusModule::FocusTrack &&
        focusAutomationTarget_ == FocusAutomationTarget::Doppler &&
        processorRef.controller().selected_layer_index().has_value();
    dopplerControlsPanel_.setVisible(visible);
    dopplerEdgeGainLabel_.setVisible(visible);
    dopplerEdgeGain_.setVisible(visible);
    dopplerCenterGainLabel_.setVisible(visible);
    dopplerCenterGain_.setVisible(visible);
    dopplerEdgePitchLabel_.setVisible(visible);
    dopplerEdgePitch_.setVisible(visible);
    dopplerCenterPitchLabel_.setVisible(visible);
    dopplerCenterPitch_.setVisible(visible);
    dopplerDrawModeToggle_.setVisible(visible);
    const bool hasSelectedSegment = visible && selectedDopplerSegmentLeftPointId_.has_value();
    dopplerCurveTypeLabel_.setVisible(hasSelectedSegment);
    dopplerCurveType_.setVisible(hasSelectedSegment);
    dopplerCurveAmountLabel_.setVisible(hasSelectedSegment);
    dopplerCurveAmount_.setVisible(hasSelectedSegment);

    if (!visible) {
        selectedDopplerSegmentLeftPointId_.reset();
        return;
    }

    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return;
    }
    const auto settings =
        processorRef.controller().layer_doppler_settings(*selectedLayer).value_or(radium::DopplerSettings{});
    suppressDopplerControls_ = true;
    dopplerEdgeGain_.setValue(settings.edge_gain_db, juce::dontSendNotification);
    dopplerCenterGain_.setValue(settings.center_gain_db, juce::dontSendNotification);
    dopplerEdgePitch_.setValue(settings.edge_pitch_semitones, juce::dontSendNotification);
    dopplerCenterPitch_.setValue(settings.center_pitch_semitones, juce::dontSendNotification);
    suppressDopplerControls_ = false;

    if (!selectedDopplerSegmentLeftPointId_.has_value()) {
        return;
    }

    const auto shape = processorRef.controller().layer_doppler_segment_shape(
        *selectedLayer, *selectedDopplerSegmentLeftPointId_);
    suppressDopplerSegmentControls_ = true;
    const auto curveType = shape.has_value() ? shape->curve_type : radium::DopplerCurveType::Linear;
    switch (curveType) {
        case radium::DopplerCurveType::SCurve:
            dopplerCurveType_.setSelectedId(2, juce::dontSendNotification);
            break;
        case radium::DopplerCurveType::Convex:
            dopplerCurveType_.setSelectedId(3, juce::dontSendNotification);
            break;
        case radium::DopplerCurveType::Concave:
            dopplerCurveType_.setSelectedId(4, juce::dontSendNotification);
            break;
        case radium::DopplerCurveType::Linear:
        default:
            dopplerCurveType_.setSelectedId(1, juce::dontSendNotification);
            break;
    }
    dopplerCurveAmount_.setValue(shape.has_value() ? shape->curve_amount : 0.5, juce::dontSendNotification);
    suppressDopplerSegmentControls_ = false;
}

void TriggerfishEditor::applyFreehandDopplerStroke(
    const std::vector<std::pair<double, double>>& strokePoints
) {
    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value() || strokePoints.empty()) {
        return;
    }

    std::vector<std::pair<double, double>> normalizedPoints;
    normalizedPoints.reserve(strokePoints.size());

    std::pair<double, double> previous = {
        std::clamp(strokePoints.front().first, 0.0, 1.0),
        std::clamp(strokePoints.front().second, -1.0, 1.0)};
    normalizedPoints.push_back(previous);

    for (std::size_t i = 1; i < strokePoints.size(); ++i) {
        const std::pair<double, double> current{
            std::clamp(strokePoints[i].first, 0.0, 1.0),
            std::clamp(strokePoints[i].second, -1.0, 1.0)};
        const bool keepPoint =
            std::abs(current.first - previous.first) >= 0.01 ||
            std::abs(current.second - previous.second) >= 0.04;
        if (keepPoint) {
            normalizedPoints.push_back(current);
            previous = current;
        }
    }

    const std::pair<double, double> lastPoint{
        std::clamp(strokePoints.back().first, 0.0, 1.0),
        std::clamp(strokePoints.back().second, -1.0, 1.0)};
    if (normalizedPoints.empty() ||
        std::abs(normalizedPoints.back().first - lastPoint.first) >= 0.0005 ||
        std::abs(normalizedPoints.back().second - lastPoint.second) >= 0.0005) {
        normalizedPoints.push_back(lastPoint);
    }

    std::sort(normalizedPoints.begin(), normalizedPoints.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    constexpr std::size_t kMaxGeneratedPoints = 48;
    if (normalizedPoints.size() > kMaxGeneratedPoints) {
        std::vector<std::pair<double, double>> thinned;
        thinned.reserve(kMaxGeneratedPoints);
        const double step = static_cast<double>(normalizedPoints.size() - 1) /
                            static_cast<double>(kMaxGeneratedPoints - 1);
        for (std::size_t i = 0; i < kMaxGeneratedPoints; ++i) {
            const std::size_t index = static_cast<std::size_t>(std::round(step * static_cast<double>(i)));
            thinned.push_back(normalizedPoints[std::min(index, normalizedPoints.size() - 1)]);
        }
        normalizedPoints.swap(thinned);
    }

    selectedDopplerSegmentLeftPointId_.reset();
    if (processorRef.controller().replace_layer_doppler_automation_points(*selectedLayer, normalizedPoints)) {
        refreshUI();
    } else {
        refreshFocusWaveform();
    }
}

void TriggerfishEditor::captureLayerEditUndoSnapshot(std::size_t layerIndex) {
    constexpr std::size_t kMaxHistoryEntries = 100;
    const auto snapshot = processorRef.controller().layer_edit_state_snapshot(layerIndex);
    if (!snapshot.has_value()) {
        return;
    }

    auto& undoStack = layerEditUndoHistory_[layerIndex];
    undoStack.push_back(*snapshot);
    if (undoStack.size() > kMaxHistoryEntries) {
        undoStack.erase(undoStack.begin());
    }
    layerEditRedoHistory_[layerIndex].clear();
}

void TriggerfishEditor::clearLayerEditHistory(std::size_t layerIndex) {
    layerEditUndoHistory_.erase(layerIndex);
    layerEditRedoHistory_.erase(layerIndex);
}

void TriggerfishEditor::clearAllLayerEditHistory() {
    layerEditUndoHistory_.clear();
    layerEditRedoHistory_.clear();
}

bool TriggerfishEditor::undoLayerEdit() {
    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return false;
    }

    auto undoIt = layerEditUndoHistory_.find(*selectedLayer);
    if (undoIt == layerEditUndoHistory_.end() || undoIt->second.empty()) {
        return false;
    }

    const auto current = processorRef.controller().layer_edit_state_snapshot(*selectedLayer);
    if (current.has_value()) {
        auto& redoStack = layerEditRedoHistory_[*selectedLayer];
        redoStack.push_back(*current);
        if (redoStack.size() > 100) {
            redoStack.erase(redoStack.begin());
        }
    }

    const auto target = undoIt->second.back();
    undoIt->second.pop_back();
    if (processorRef.controller().restore_layer_edit_state(*selectedLayer, target)) {
        waveform_.clearSelectedFadeHandle();
        waveform_.clearEditSelection();
        waveform_.clearSelectedClips();
        invalidateFocusWaveformCache();
        refreshUI();
        return true;
    }

    layerEditRedoHistory_[*selectedLayer].clear();
    return false;
}

bool TriggerfishEditor::redoLayerEdit() {
    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return false;
    }

    auto redoIt = layerEditRedoHistory_.find(*selectedLayer);
    if (redoIt == layerEditRedoHistory_.end() || redoIt->second.empty()) {
        return false;
    }

    const auto current = processorRef.controller().layer_edit_state_snapshot(*selectedLayer);
    if (current.has_value()) {
        auto& undoStack = layerEditUndoHistory_[*selectedLayer];
        undoStack.push_back(*current);
        if (undoStack.size() > 100) {
            undoStack.erase(undoStack.begin());
        }
    }

    const auto target = redoIt->second.back();
    redoIt->second.pop_back();
    if (processorRef.controller().restore_layer_edit_state(*selectedLayer, target)) {
        waveform_.clearSelectedFadeHandle();
        waveform_.clearEditSelection();
        waveform_.clearSelectedClips();
        invalidateFocusWaveformCache();
        refreshUI();
        return true;
    }

    layerEditUndoHistory_[*selectedLayer].clear();
    return false;
}

void TriggerfishEditor::toggleFocusedModule(FocusModule module) {
    focusedModule_ = (focusedModule_ == module) ? FocusModule::None : module;
    waveform_.setAutomationLaneVisible(focusedModule_ == FocusModule::FocusTrack);
    focusAutomationTargetLabel_.setText(currentAutomationTargetLabel(), juce::dontSendNotification);
    focusAutomationTargetLabel_.setVisible(focusedModule_ == FocusModule::FocusTrack);
    if (focusedModule_ == FocusModule::FocusTrack) {
        switch (focusAutomationTarget_) {
            case FocusAutomationTarget::Stretch:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Stretch);
                break;
            case FocusAutomationTarget::Doppler:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Doppler);
                break;
            case FocusAutomationTarget::PanPosition:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanPosition);
                break;
            case FocusAutomationTarget::PanFrontBack:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanFrontBack);
                break;
            case FocusAutomationTarget::PanRightPosition:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanRightPosition);
                break;
            case FocusAutomationTarget::PanRightFrontBack:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::PanRightFrontBack);
                break;
            case FocusAutomationTarget::Volume:
            default:
                waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::Volume);
                break;
        }
    } else {
        waveform_.setAutomationLaneType(triggerfish::WaveformComponent::AutomationLaneType::None);
    }
    updateFocusedModuleButtons();
    resized();
    repaint();
}

void TriggerfishEditor::updateFocusedModuleButtons() {
    focusTrackFocusButton_.setButtonText(focusedModule_ == FocusModule::FocusTrack ? "Close" : "Focus");
}

bool TriggerfishEditor::isCurrentAutomationTargetEnabled(std::size_t layer_index) const {
    switch (focusAutomationTarget_) {
        case FocusAutomationTarget::Stretch:
            return processorRef.controller().layer_stretch_automation_enabled(layer_index);
        case FocusAutomationTarget::PanPosition:
            return processorRef.controller().layer_pan_automation_enabled(
                layer_index, radium::AppController::PanAutomationTarget::Position);
        case FocusAutomationTarget::PanFrontBack:
            return processorRef.controller().layer_pan_automation_enabled(
                layer_index, radium::AppController::PanAutomationTarget::FrontBack);
        case FocusAutomationTarget::PanRightPosition:
            return processorRef.controller().layer_pan_automation_enabled(
                layer_index, radium::AppController::PanAutomationTarget::RightPosition);
        case FocusAutomationTarget::PanRightFrontBack:
            return processorRef.controller().layer_pan_automation_enabled(
                layer_index, radium::AppController::PanAutomationTarget::RightFrontBack);
        case FocusAutomationTarget::Doppler:
            return processorRef.controller().layer_doppler_automation_enabled(layer_index);
        case FocusAutomationTarget::Volume:
        default:
            return processorRef.controller().layer_volume_automation_enabled(layer_index);
    }
}

bool TriggerfishEditor::resetCurrentAutomationTarget(std::size_t layer_index) {
    switch (focusAutomationTarget_) {
        case FocusAutomationTarget::Stretch:
            return processorRef.controller().reset_layer_stretch_automation(layer_index);
        case FocusAutomationTarget::PanPosition:
            return processorRef.controller().reset_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::Position);
        case FocusAutomationTarget::PanFrontBack:
            return processorRef.controller().reset_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::FrontBack);
        case FocusAutomationTarget::PanRightPosition:
            return processorRef.controller().reset_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::RightPosition);
        case FocusAutomationTarget::PanRightFrontBack:
            return processorRef.controller().reset_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::RightFrontBack);
        case FocusAutomationTarget::Doppler:
            return processorRef.controller().reset_layer_doppler_automation(layer_index);
        case FocusAutomationTarget::Volume:
        default:
            return processorRef.controller().reset_layer_volume_automation(layer_index);
    }
}

bool TriggerfishEditor::removeCurrentAutomationTarget(std::size_t layer_index) {
    switch (focusAutomationTarget_) {
        case FocusAutomationTarget::Stretch:
            return processorRef.controller().remove_layer_stretch_automation(layer_index);
        case FocusAutomationTarget::PanPosition:
            return processorRef.controller().remove_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::Position);
        case FocusAutomationTarget::PanFrontBack:
            return processorRef.controller().remove_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::FrontBack);
        case FocusAutomationTarget::PanRightPosition:
            return processorRef.controller().remove_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::RightPosition);
        case FocusAutomationTarget::PanRightFrontBack:
            return processorRef.controller().remove_layer_pan_automation(
                layer_index, radium::AppController::PanAutomationTarget::RightFrontBack);
        case FocusAutomationTarget::Doppler:
            return processorRef.controller().remove_layer_doppler_automation(layer_index);
        case FocusAutomationTarget::Volume:
        default:
            return processorRef.controller().remove_layer_volume_automation(layer_index);
    }
}

bool TriggerfishEditor::enableAllPanAutomationTargets(std::size_t layer_index, bool is_stereo) {
    auto& ctrl = processorRef.controller();
    bool changed = false;
    changed = ctrl.enable_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::Position) || changed;
    changed = ctrl.enable_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::FrontBack) || changed;
    if (is_stereo) {
        changed = ctrl.enable_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightPosition) || changed;
        changed = ctrl.enable_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightFrontBack) || changed;
    }
    return changed;
}

bool TriggerfishEditor::resetAllPanAutomationTargets(std::size_t layer_index, bool is_stereo) {
    auto& ctrl = processorRef.controller();
    bool changed = false;
    changed = ctrl.reset_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::Position) || changed;
    changed = ctrl.reset_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::FrontBack) || changed;
    if (is_stereo) {
        changed = ctrl.reset_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightPosition) || changed;
        changed = ctrl.reset_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightFrontBack) || changed;
    }
    return changed;
}

bool TriggerfishEditor::removeAllPanAutomationTargets(std::size_t layer_index, bool is_stereo) {
    auto& ctrl = processorRef.controller();
    bool changed = false;
    changed = ctrl.remove_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::Position) || changed;
    changed = ctrl.remove_layer_pan_automation(
                  layer_index, radium::AppController::PanAutomationTarget::FrontBack) || changed;
    if (is_stereo) {
        changed = ctrl.remove_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightPosition) || changed;
        changed = ctrl.remove_layer_pan_automation(
                      layer_index, radium::AppController::PanAutomationTarget::RightFrontBack) || changed;
    }
    return changed;
}

void TriggerfishEditor::enableStandaloneMidiInputs() {
    if (!isStandalone_) {
        return;
    }

    auto* standaloneWindow = findParentComponentOfClass<juce::StandaloneFilterWindow>();
    if (standaloneWindow == nullptr) {
        standaloneWindow = dynamic_cast<juce::StandaloneFilterWindow*>(getTopLevelComponent());
    }
    if (standaloneWindow == nullptr) {
        return;
    }

    auto& deviceManager = standaloneWindow->getDeviceManager();
    for (const auto& device : juce::MidiInput::getAvailableDevices()) {
        deviceManager.setMidiInputDeviceEnabled(device.identifier, true);
    }
}

void TriggerfishEditor::refreshStandaloneMidiStatus() {
    if (!isStandalone_) {
        toolbar_.setMidiStatusVisible(false);
        return;
    }
    toolbar_.setMidiStatus(lastMidiActivityCounter_ > 0 &&
                           (std::chrono::steady_clock::now() - lastMidiActivityAt_) < std::chrono::milliseconds(1500));
}

juce::String TriggerfishEditor::currentAutomationTargetLabel() const {
    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return "No Automation";
    }

    const bool isStereo = processorRef.controller().layer_is_stereo(*selectedLayer);
    bool laneEnabled = false;

    switch (focusAutomationTarget_) {
        case FocusAutomationTarget::Stretch:
            laneEnabled = processorRef.controller().layer_stretch_automation_enabled(*selectedLayer);
            if (!laneEnabled) {
                return "No Automation";
            }
            return "Automation: Stretch";
        case FocusAutomationTarget::PanPosition:
            laneEnabled = processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::Position);
            if (!laneEnabled) {
                return "No Automation";
            }
            return isStereo ? "Automation: Pan Left" : "Automation: Pan";
        case FocusAutomationTarget::PanFrontBack:
            laneEnabled = processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::FrontBack);
            if (!laneEnabled) {
                return "No Automation";
            }
            return isStereo ? "Automation: Pan L F/R" : "Automation: Pan F/R";
        case FocusAutomationTarget::PanRightPosition:
            laneEnabled = processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::RightPosition);
            if (!laneEnabled) {
                return "No Automation";
            }
            return "Automation: Pan Right";
        case FocusAutomationTarget::PanRightFrontBack:
            laneEnabled = processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::RightFrontBack);
            if (!laneEnabled) {
                return "No Automation";
            }
            return "Automation: Pan R F/R";
        case FocusAutomationTarget::Doppler:
            laneEnabled = processorRef.controller().layer_doppler_automation_enabled(*selectedLayer);
            if (!laneEnabled) {
                return "No Automation";
            }
            return "Automation: Doppler";
        case FocusAutomationTarget::Volume:
        default:
            laneEnabled = processorRef.controller().layer_volume_automation_enabled(*selectedLayer);
            if (!laneEnabled) {
                return "No Automation";
            }
            return "Automation: Volume";
    }
}

void TriggerfishEditor::resetTrackAutomationWriteState() {
    if (trackAutomationWriteActive_ && trackAutomationWriteLayerIndex_.has_value() &&
        trackAutomationWriteTarget_.has_value()) {
        if (*trackAutomationWriteTarget_ == FocusAutomationTarget::Stretch) {
            processorRef.controller().commit_layer_stretch_automation(*trackAutomationWriteLayerIndex_);
            if (trackAutomationWriteOriginalStretch_.has_value()) {
                auto fx = processorRef.controller().layer_effect_state(*trackAutomationWriteLayerIndex_);
                if (fx.has_value()) {
                    fx->time_stretch_ratio = *trackAutomationWriteOriginalStretch_;
                    processorRef.controller().set_layer_effect_state(*trackAutomationWriteLayerIndex_, *fx);
                }
            }
        } else if (*trackAutomationWriteTarget_ == FocusAutomationTarget::Doppler) {
            processorRef.controller().commit_layer_doppler_automation(*trackAutomationWriteLayerIndex_);
            if (trackAutomationWriteOriginalPanX_.has_value() && trackAutomationWriteOriginalPanY_.has_value()) {
                processorRef.controller().set_layer_pan(*trackAutomationWriteLayerIndex_,
                                                        *trackAutomationWriteOriginalPanX_,
                                                        *trackAutomationWriteOriginalPanY_);
            }
            if (trackAutomationWriteOriginalPanXRight_.has_value() &&
                trackAutomationWriteOriginalPanYRight_.has_value()) {
                processorRef.controller().set_layer_pan_right(*trackAutomationWriteLayerIndex_,
                                                              *trackAutomationWriteOriginalPanXRight_,
                                                              *trackAutomationWriteOriginalPanYRight_);
            }
            if (trackAutomationWriteOriginalGain_.has_value()) {
                processorRef.controller().set_layer_gain(*trackAutomationWriteLayerIndex_,
                                                         *trackAutomationWriteOriginalGain_);
            }
            if (trackAutomationWriteOriginalStretch_.has_value()) {
                auto fx = processorRef.controller().layer_effect_state(*trackAutomationWriteLayerIndex_);
                if (fx.has_value()) {
                    fx->time_stretch_ratio = *trackAutomationWriteOriginalStretch_;
                    processorRef.controller().set_layer_effect_state(*trackAutomationWriteLayerIndex_, *fx);
                }
            }
            processorRef.controller().push_live_pan(*trackAutomationWriteLayerIndex_);
            processorRef.controller().push_live_gain(*trackAutomationWriteLayerIndex_);
            processorRef.controller().push_live_stretch(*trackAutomationWriteLayerIndex_);
        } else if (*trackAutomationWriteTarget_ == FocusAutomationTarget::PanPosition ||
                   *trackAutomationWriteTarget_ == FocusAutomationTarget::PanFrontBack ||
                   *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightPosition ||
                   *trackAutomationWriteTarget_ == FocusAutomationTarget::PanRightFrontBack) {
            processorRef.controller().commit_layer_pan_automation(
                *trackAutomationWriteLayerIndex_, radium::AppController::PanAutomationTarget::Position);
            processorRef.controller().commit_layer_pan_automation(
                *trackAutomationWriteLayerIndex_, radium::AppController::PanAutomationTarget::FrontBack);
            processorRef.controller().commit_layer_pan_automation(
                *trackAutomationWriteLayerIndex_, radium::AppController::PanAutomationTarget::RightPosition);
            processorRef.controller().commit_layer_pan_automation(
                *trackAutomationWriteLayerIndex_, radium::AppController::PanAutomationTarget::RightFrontBack);
            if (trackAutomationWriteOriginalPanX_.has_value() && trackAutomationWriteOriginalPanY_.has_value()) {
                processorRef.controller().set_layer_pan(*trackAutomationWriteLayerIndex_,
                                                        *trackAutomationWriteOriginalPanX_,
                                                        *trackAutomationWriteOriginalPanY_);
            }
            if (trackAutomationWriteOriginalPanXRight_.has_value() &&
                trackAutomationWriteOriginalPanYRight_.has_value()) {
                processorRef.controller().set_layer_pan_right(*trackAutomationWriteLayerIndex_,
                                                              *trackAutomationWriteOriginalPanXRight_,
                                                              *trackAutomationWriteOriginalPanYRight_);
            }
        } else {
            processorRef.controller().commit_layer_volume_automation(*trackAutomationWriteLayerIndex_);
            if (trackAutomationWriteOriginalGain_.has_value()) {
                processorRef.controller().set_layer_gain(*trackAutomationWriteLayerIndex_,
                                                         *trackAutomationWriteOriginalGain_);
            }
        }
    }

    trackAutomationWriteActive_ = false;
    trackAutomationWriteLayerIndex_.reset();
    trackAutomationWriteTarget_.reset();
    trackAutomationWriteOriginalGain_.reset();
    trackAutomationWriteOriginalStretch_.reset();
    trackAutomationWriteOriginalPanX_.reset();
    trackAutomationWriteOriginalPanY_.reset();
    trackAutomationWriteOriginalPanXRight_.reset();
    trackAutomationWriteOriginalPanYRight_.reset();
    refreshUI();
}

void TriggerfishEditor::refreshRecorderPictureControls() {
    auto formatTimecode = [](double seconds) {
        const auto clampedSeconds = juce::jmax(0.0, seconds);
        const auto totalMinutes = static_cast<int>(clampedSeconds / 60.0);
        const auto wholeSeconds = static_cast<int>(clampedSeconds) % 60;
        const auto hundredths = static_cast<int>(std::round((clampedSeconds - std::floor(clampedSeconds)) * 100.0));

        juce::String formatted;
        formatted << juce::String(totalMinutes).paddedLeft('0', 2)
                  << ":" << juce::String(wholeSeconds).paddedLeft('0', 2)
                  << "." << juce::String(juce::jmin(hundredths, 99)).paddedLeft('0', 2);
        return formatted;
    };

    const bool hasLoadedPicture = pictureWindow_ && pictureWindow_->content().hasLoadedVideo();
    double currentSeconds = recorderPictureCueSeconds_;
    double durationSeconds = 0.0;
    double volume = recorderPictureVolume_.getValue();

    if (hasLoadedPicture) {
        const auto& picture = pictureWindow_->content();
        currentSeconds = picture.currentPositionSeconds();
        durationSeconds = picture.durationSeconds();
        volume = picture.volume();
        if (!picture.isPlaying() && !newRecordingActive_ && !punchInActive_ && !isTakePlaying_) {
            recorderPictureCueSeconds_ = currentSeconds;
        }
    }

    suppressRecorderPictureControls_ = true;
    recorderPictureTimeline_.setEnabled(hasLoadedPicture);
    recorderPictureVolume_.setEnabled(hasLoadedPicture);
    recorderPictureTimeline_.setRange(0.0, durationSeconds > 0.0 ? durationSeconds : 1.0, 0.0);
    if (!recorderPictureTimeline_.isMouseButtonDown()) {
        recorderPictureTimeline_.setValue(currentSeconds, juce::dontSendNotification);
    }
    recorderPictureVolume_.setValue(volume, juce::dontSendNotification);
    recorderPictureTimeLabel_.setText(
        formatTimecode(currentSeconds) + " / " + formatTimecode(durationSeconds),
        juce::dontSendNotification);
    suppressRecorderPictureControls_ = false;
}

void TriggerfishEditor::setRecorderBusMode(radium::RecordBusMode mode) {
    processorRef.setRecordBusMode(mode);
    int selectedId = 1;
    if (mode == radium::RecordBusMode::Surround50) {
        selectedId = 2;
    } else if (mode == radium::RecordBusMode::Surround51) {
        selectedId = 3;
    } else if (mode == radium::RecordBusMode::Surround70) {
        selectedId = 4;
    } else if (mode == radium::RecordBusMode::Surround71) {
        selectedId = 5;
    }
    suppressRecorderBusModeBox_ = true;
    recorderBusModeBox_.setSelectedId(selectedId, juce::dontSendNotification);
    suppressRecorderBusModeBox_ = false;
}

void TriggerfishEditor::syncRecorderSurroundModeFromSelection() {
    if (const auto selectedTake = processorRef.controller().selected_session_recording(); selectedTake.has_value()) {
        setRecorderBusMode(selectedTake->channels >= 8
                               ? radium::RecordBusMode::Surround71
                               : (selectedTake->channels >= 7
                                      ? radium::RecordBusMode::Surround70
                                      : (selectedTake->channels >= 6
                                             ? radium::RecordBusMode::Surround51
                                             : (selectedTake->channels >= 5
                                                    ? radium::RecordBusMode::Surround50
                                                    : radium::RecordBusMode::Stereo))));
        return;
    }

    const auto mode = processorRef.recordBusMode();
    int selectedId = 1;
    if (mode == radium::RecordBusMode::Surround50) {
        selectedId = 2;
    } else if (mode == radium::RecordBusMode::Surround51) {
        selectedId = 3;
    } else if (mode == radium::RecordBusMode::Surround70) {
        selectedId = 4;
    } else if (mode == radium::RecordBusMode::Surround71) {
        selectedId = 5;
    }
    suppressRecorderBusModeBox_ = true;
    recorderBusModeBox_.setSelectedId(selectedId, juce::dontSendNotification);
    suppressRecorderBusModeBox_ = false;
}

std::optional<double> TriggerfishEditor::selectedTakePictureStartSeconds() {
    const auto selectedTake = processorRef.controller().selected_session_recording();
    if (!selectedTake.has_value()) {
        return std::nullopt;
    }

    return selectedTake->picture_start_seconds;
}

void TriggerfishEditor::syncPictureToSelectedTake() {
    const auto pictureStart = selectedTakePictureStartSeconds();
    if (!pictureStart.has_value()) {
        return;
    }

    recorderPictureCueSeconds_ = *pictureStart;
    if (pictureWindow_ && pictureWindow_->content().hasLoadedVideo()) {
        pictureWindow_->content().seek(*pictureStart);
    }
    refreshRecorderPictureControls();
}

void TriggerfishEditor::stopTakeOwnedPicturePlayback(bool rewindToStart) {
    if (!pictureWindow_) {
        pictureTransportOwnedByTakePlayback_ = false;
        return;
    }

    auto& picture = pictureWindow_->content();
    if (!picture.hasLoadedVideo()) {
        pictureTransportOwnedByTakePlayback_ = false;
        return;
    }

    if (pictureTransportOwnedByTakePlayback_ || picture.isPlaying()) {
        picture.stop(false);
    }
    if (rewindToStart) {
        if (const auto pictureStart = selectedTakePictureStartSeconds(); pictureStart.has_value()) {
            picture.seek(*pictureStart);
        }
    }
    pictureTransportOwnedByTakePlayback_ = false;
}

void TriggerfishEditor::finalizePunchRecording() {
    processorRef.controller().set_session_recording_armed(false);
    auto recording = processorRef.controller().streaming_mixer().take_recording();
    if (recording.has_value() && activePunchRegion_.has_value() &&
        punchInTakeDurationSeconds_ > 0.0) {
        std::string err;
        const double actualStart = std::max(activePunchRegion_->first, punchInCueStart_);
        const double actualEnd = activePunchRegion_->second;
        if (actualEnd > actualStart) {
            const double startDelaySeconds =
                std::max(0.0, actualStart - punchInCueStart_) * punchInTakeDurationSeconds_;
            const double endDelaySeconds =
                std::max(0.0, actualEnd - punchInCueStart_) * punchInTakeDurationSeconds_;
            const std::size_t startFrame = std::min<std::size_t>(
                recording->frame_count(),
                static_cast<std::size_t>(startDelaySeconds * static_cast<double>(recording->sample_rate)));
            const std::size_t endFrame = std::min<std::size_t>(
                recording->frame_count(),
                static_cast<std::size_t>(std::ceil(
                    endDelaySeconds * static_cast<double>(recording->sample_rate))));
            if (endFrame > startFrame) {
                radium::RenderedAudio punchSlice;
                punchSlice.sample_rate = recording->sample_rate;
                punchSlice.channels = recording->channels;
                const std::size_t startSample =
                    startFrame * static_cast<std::size_t>(recording->channels);
                const std::size_t endSample =
                    endFrame * static_cast<std::size_t>(recording->channels);
                punchSlice.samples.assign(
                    recording->samples.begin() + static_cast<std::ptrdiff_t>(startSample),
                    recording->samples.begin() + static_cast<std::ptrdiff_t>(endSample));
                processorRef.controller().commit_session_recording(
                    punchSlice, "take",
                    std::make_optional(std::make_pair(actualStart, actualEnd)),
                    std::nullopt,
                    &err);
            }
        }
    }

    punchInActive_ = false;
    stopRecordingOwnedPicturePlayback(false);
    activePunchRegion_.reset();
    sessionRecorder_.clearRecordingPreview();
    sessionRecorder_.setRecordingState(false, false);
    sessionRecorder_.setTakePlayhead(-1.0);
}

triggerfish::PictureWindow& TriggerfishEditor::ensurePictureWindow() {
    if (!pictureWindow_) {
        pictureWindow_ = std::make_unique<triggerfish::PictureWindow>();
        pictureWindow_->content().onInteraction = [this] {
            playbackTarget_ = PlaybackTarget::Picture;
        };
        pictureWindow_->content().onLoadRequested = [this] {
            // Empty filter on purpose: JUCE 8 still uses macOS's deprecated
            // setAllowedFileTypes: API which silently greys out files on
            // macOS Sonoma/Sequoia even when their extensions match. Passing
            // an empty filter makes NSOpenPanel show every file, so users can
            // actually select their videos. If they pick something the OS
            // decoder can't open, the load path shows a clear error dialog.
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load Picture", juce::File(), juce::String());
            chooser->launchAsync(juce::FileBrowserComponent::openMode,
                [this, chooser](const juce::FileChooser& fc) {
                    auto file = fc.getResult();
                    if (file != juce::File()) {
                        loadVideoFile(file);
                    }
                });
        };
        pictureWindow_->content().onVideoCleared = [this] {
            processorRef.controller().clear_project_picture_path();
            recorderPictureCueSeconds_ = 0.0;
            refreshRecorderPictureControls();
        };
    }

    return *pictureWindow_;
}

triggerfish::LibrarySearchWindow& TriggerfishEditor::ensureLibrarySearchWindow() {
    if (!librarySearchWindow_) {
        librarySearchWindow_ = std::make_unique<triggerfish::LibrarySearchWindow>();
        librarySearchWindow_->content().onResultChosen = [this](const juce::String& filePath) {
            placeFileOnSelectedLayer(filePath);
        };
        librarySearchWindow_->content().onFocusTrackTransportToggle = [this] {
            auto& ctrl = processorRef.controller();
            if (ctrl.is_streaming()) {
                ctrl.stop_streaming_playback();
                if (isTakePlaying_) {
                    isTakePlaying_ = false;
                    sessionRecorder_.setTakePlayhead(-1.0);
                    stopTakeOwnedPicturePlayback(false);
                }
            } else {
                toggleFocusTrackPlayback();
            }
        };
    }

    return *librarySearchWindow_;
}

void TriggerfishEditor::showPictureWindow() {
    auto& pictureWindow = ensurePictureWindow();
    pictureWindow.setVisible(true);
    pictureWindow.toFront(true);
}

void TriggerfishEditor::showLibrarySearchWindow() {
    auto& searchWindow = ensureLibrarySearchWindow();
    searchWindow.content().reloadLibraries();
    searchWindow.setVisible(true);
    searchWindow.toFront(true);
    searchWindow.content().focusSearchBox();
}

void TriggerfishEditor::loadVideoFile(const juce::File& file) {
    if (file == juce::File()) return;

    auto& pictureWindow = ensurePictureWindow();
    stopRecordingOwnedPicturePlayback(true);
    pictureWindow.setVisible(true);
    pictureWindow.toFront(true);
    pictureWindow.content().loadVideo(file);
    processorRef.controller().set_project_picture_path(file.getFullPathName().toStdString());
    if (const auto pictureStart = selectedTakePictureStartSeconds(); pictureStart.has_value()) {
        recorderPictureCueSeconds_ = *pictureStart;
        pictureWindow.content().seek(*pictureStart);
    } else {
        recorderPictureCueSeconds_ = pictureWindow.content().currentPositionSeconds();
    }
    refreshRecorderPictureControls();
}

void TriggerfishEditor::stopRecordingOwnedPicturePlayback(bool rewindToStart) {
    if (!pictureWindow_) {
        pictureTransportOwnedByRecorder_ = false;
        return;
    }

    auto& picture = pictureWindow_->content();
    if (!picture.hasLoadedVideo()) {
        pictureTransportOwnedByRecorder_ = false;
        return;
    }

    if (pictureTransportOwnedByRecorder_ || picture.isPlaying()) {
        picture.stop(rewindToStart);
    }
    pictureTransportOwnedByRecorder_ = false;
}

void TriggerfishEditor::addAudioToSelectedLayer() {
    auto selected = processorRef.controller().selected_layer_index();
    if (!selected.has_value()) return;

    auto chooser = std::make_shared<juce::FileChooser>(
        "Add Audio File", juce::File(), "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg");
    chooser->launchAsync(juce::FileBrowserComponent::openMode, [this, chooser, selected](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file != juce::File()) {
            std::string err;
            if (!processorRef.controller().add_audio_file_to_layer(
                    *selected, file.getFullPathName().toStdString(), &err) && !err.empty()) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Add Audio Failed",
                    err);
            }
            clearLayerEditHistory(*selected);
            invalidateFocusWaveformCache();
            refreshUI();
        }
    });
}

void TriggerfishEditor::showDatabaseMenu() {
    const auto libraries = triggerfish::LibraryDatabase::availableLibraries();

    juce::PopupMenu menu;
    menu.addItem(1, "Create New Database...");

    juce::PopupMenu addMenu;
    juce::PopupMenu renameMenu;
    juce::PopupMenu deleteMenu;

    for (int i = 0; i < libraries.size(); ++i) {
        const auto itemId = i + 100;
        const auto& library = libraries.getReference(i);
        addMenu.addItem(itemId, library.name);
        renameMenu.addItem(itemId + 1000, library.name);
        deleteMenu.addItem(itemId + 2000, library.name);
    }

    menu.addSubMenu("Add Files To Existing Database", addMenu, libraries.size() > 0);
    menu.addSubMenu("Rename Database", renameMenu, libraries.size() > 0);
    menu.addSubMenu("Delete Database", deleteMenu, libraries.size() > 0);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&toolbar_),
                       juce::ModalCallbackFunction::create([this, libraries](int result) {
        if (result == 1) {
            createLibraryDatabase();
            return;
        }

        if (result >= 100 && result < 1100) {
            const int index = result - 100;
            if (juce::isPositiveAndBelow(index, libraries.size())) {
                appendFolderToLibraryDatabase(libraries.getReference(index).id);
            }
            return;
        }

        if (result >= 1100 && result < 2100) {
            const int index = result - 1100;
            if (juce::isPositiveAndBelow(index, libraries.size())) {
                renameLibraryDatabase(libraries.getReference(index).id);
            }
            return;
        }

        if (result >= 2100 && result < 3100) {
            const int index = result - 2100;
            if (juce::isPositiveAndBelow(index, libraries.size())) {
                deleteLibraryDatabase(libraries.getReference(index).id);
            }
        }
    }));
}

void TriggerfishEditor::showFocusAutomationMenu() {
    const auto selectedLayer = processorRef.controller().selected_layer_index();
    if (!selectedLayer.has_value()) {
        return;
    }

    const auto overview = processorRef.controller().layer_waveform(*selectedLayer, 16);
    const bool isStereo = overview.has_value() && overview->channels > 1;
    const bool anyPanLaneEnabled =
        processorRef.controller().layer_pan_automation_enabled(
            *selectedLayer, radium::AppController::PanAutomationTarget::Position) ||
        processorRef.controller().layer_pan_automation_enabled(
            *selectedLayer, radium::AppController::PanAutomationTarget::FrontBack) ||
        (isStereo && processorRef.controller().layer_pan_automation_enabled(
                         *selectedLayer, radium::AppController::PanAutomationTarget::RightPosition)) ||
        (isStereo && processorRef.controller().layer_pan_automation_enabled(
                         *selectedLayer, radium::AppController::PanAutomationTarget::RightFrontBack));

    const bool currentLaneEnabled = isCurrentAutomationTargetEnabled(*selectedLayer);

    juce::PopupMenu menu;
    menu.addItem(100, "Reset Current Lane", currentLaneEnabled);
    menu.addItem(101, "Remove Current Lane", currentLaneEnabled);
    menu.addItem(102, "Reset All Panning Curves", anyPanLaneEnabled);
    menu.addItem(103, "Remove All Panning Curves", anyPanLaneEnabled);
    menu.addSeparator();
    menu.addItem(
        1,
        "Volume",
        true,
        processorRef.controller().layer_volume_automation_enabled(*selectedLayer));
    menu.addItem(
        2,
        "Stretch %",
        true,
        processorRef.controller().layer_stretch_automation_enabled(*selectedLayer));
    menu.addItem(
        3,
        "Doppler",
        true,
        processorRef.controller().layer_doppler_automation_enabled(*selectedLayer));
    menu.addSeparator();
    menu.addItem(9, "Pan: All Curves", true, anyPanLaneEnabled);
    menu.addSeparator();
    menu.addItem(
        10,
        isStereo ? "Pan: Left Position" : "Pan: Position",
        true,
        processorRef.controller().layer_pan_automation_enabled(
            *selectedLayer, radium::AppController::PanAutomationTarget::Position));
    menu.addItem(
        11,
        isStereo ? "Pan: Left F/R Position" : "Pan: F/R Position",
        true,
        processorRef.controller().layer_pan_automation_enabled(
            *selectedLayer, radium::AppController::PanAutomationTarget::FrontBack));
    if (isStereo) {
        menu.addItem(
            12,
            "Pan: Right Position",
            true,
            processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::RightPosition));
        menu.addItem(
            13,
            "Pan: Right F/R Position",
            true,
            processorRef.controller().layer_pan_automation_enabled(
                *selectedLayer, radium::AppController::PanAutomationTarget::RightFrontBack));
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addAutomationButton_),
                       juce::ModalCallbackFunction::create([this, selectedLayer, isStereo](int result) {
        if (!selectedLayer.has_value() || result == 0) {
            return;
        }

        if (result == 100) {
            captureLayerEditUndoSnapshot(*selectedLayer);
            resetCurrentAutomationTarget(*selectedLayer);
            invalidateFocusWaveformCache();
            refreshUI();
            return;
        }

        if (result == 101) {
            captureLayerEditUndoSnapshot(*selectedLayer);
            removeCurrentAutomationTarget(*selectedLayer);
            invalidateFocusWaveformCache();
            refreshUI();
            return;
        }

        if (result == 102) {
            captureLayerEditUndoSnapshot(*selectedLayer);
            resetAllPanAutomationTargets(*selectedLayer, isStereo);
            invalidateFocusWaveformCache();
            refreshUI();
            return;
        }

        if (result == 103) {
            captureLayerEditUndoSnapshot(*selectedLayer);
            removeAllPanAutomationTargets(*selectedLayer, isStereo);
            invalidateFocusWaveformCache();
            refreshUI();
            return;
        }

        auto& ctrl = processorRef.controller();
        auto selectPanLane = [&](FocusAutomationTarget uiTarget, radium::AppController::PanAutomationTarget target) {
            focusAutomationTarget_ = uiTarget;
            if (!ctrl.layer_pan_automation_enabled(*selectedLayer, target)) {
                captureLayerEditUndoSnapshot(*selectedLayer);
                ctrl.enable_layer_pan_automation(*selectedLayer, target);
            }
            invalidateFocusWaveformCache();
            refreshUI();
        };

        switch (result) {
            case 1:
                focusAutomationTarget_ = FocusAutomationTarget::Volume;
                if (!ctrl.layer_volume_automation_enabled(*selectedLayer)) {
                    captureLayerEditUndoSnapshot(*selectedLayer);
                    ctrl.enable_layer_volume_automation(*selectedLayer);
                }
                invalidateFocusWaveformCache();
                refreshUI();
                return;
            case 2:
                focusAutomationTarget_ = FocusAutomationTarget::Stretch;
                if (!ctrl.layer_stretch_automation_enabled(*selectedLayer)) {
                    captureLayerEditUndoSnapshot(*selectedLayer);
                    ctrl.enable_layer_stretch_automation(*selectedLayer);
                }
                invalidateFocusWaveformCache();
                refreshUI();
                return;
            case 3:
                focusAutomationTarget_ = FocusAutomationTarget::Doppler;
                if (!ctrl.layer_doppler_automation_enabled(*selectedLayer)) {
                    captureLayerEditUndoSnapshot(*selectedLayer);
                    ctrl.enable_layer_doppler_automation(*selectedLayer);
                }
                invalidateFocusWaveformCache();
                refreshUI();
                return;
            case 9:
                focusAutomationTarget_ = FocusAutomationTarget::PanPosition;
                if (!ctrl.layer_pan_automation_enabled(*selectedLayer, radium::AppController::PanAutomationTarget::Position) ||
                    !ctrl.layer_pan_automation_enabled(*selectedLayer, radium::AppController::PanAutomationTarget::FrontBack) ||
                    (isStereo && !ctrl.layer_pan_automation_enabled(
                                     *selectedLayer, radium::AppController::PanAutomationTarget::RightPosition)) ||
                    (isStereo && !ctrl.layer_pan_automation_enabled(
                                     *selectedLayer, radium::AppController::PanAutomationTarget::RightFrontBack))) {
                    captureLayerEditUndoSnapshot(*selectedLayer);
                    enableAllPanAutomationTargets(*selectedLayer, isStereo);
                }
                invalidateFocusWaveformCache();
                refreshUI();
                return;
            case 10:
                selectPanLane(FocusAutomationTarget::PanPosition,
                              radium::AppController::PanAutomationTarget::Position);
                return;
            case 11:
                selectPanLane(FocusAutomationTarget::PanFrontBack,
                              radium::AppController::PanAutomationTarget::FrontBack);
                return;
            case 12:
                selectPanLane(FocusAutomationTarget::PanRightPosition,
                              radium::AppController::PanAutomationTarget::RightPosition);
                return;
            case 13:
                selectPanLane(FocusAutomationTarget::PanRightFrontBack,
                              radium::AppController::PanAutomationTarget::RightFrontBack);
                return;
            default:
                return;
        }
    }));
}

void TriggerfishEditor::createLibraryDatabase() {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Choose Folder To Index", juce::File());
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories,
                         [this, chooser](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (folder == juce::File()) {
            return;
        }

        auto* prompt = new juce::AlertWindow(
            "Create Database",
            "Name this database.",
            juce::AlertWindow::NoIcon);
        prompt->addTextEditor("dbName", folder.getFileName(), "Database Name");
        prompt->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
        prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
        prompt->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, safePrompt, folder](int result) {
                if (result != 1) {
                    return;
                }

                if (safePrompt == nullptr) {
                    return;
                }

                const auto databaseName = safePrompt->getTextEditorContents("dbName").trim();
                if (databaseName.isEmpty()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Create Database Failed",
                        "Please enter a database name.");
                    return;
                }

                auto editor = juce::Component::SafePointer<TriggerfishEditor>(this);
                auto libraryId = std::make_shared<juce::String>();
                auto* task = new LibraryIndexProgressTask(
                    "Creating Database...",
                    [folder, databaseName, libraryId](const triggerfish::LibraryDatabase::ProgressCallback& progress,
                                                      juce::String& error) {
                        return triggerfish::LibraryDatabase::createOrUpdateDatabase(
                            folder, error, databaseName, libraryId.get(), progress);
                    },
                    [editor, folder, libraryId](bool success, const juce::String& error) {
                        if (editor == nullptr) {
                            return;
                        }

                        if (!success) {
                            juce::AlertWindow::showMessageBoxAsync(
                                juce::AlertWindow::WarningIcon,
                                "Create Database Failed",
                                error.isNotEmpty() ? error : "The library database could not be created.");
                            return;
                        }

                        editor->ensureLibrarySearchWindow().content().reloadLibraries(*libraryId);
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::InfoIcon,
                            "Database Ready",
                            "Indexed " + folder.getFileName() + ". You can search it from the Search window.");
                    });
                task->launchThread();
            }),
            true);
    });
}

void TriggerfishEditor::appendFolderToLibraryDatabase(const juce::String& libraryId) {
    auto chooser = std::make_shared<juce::FileChooser>(
        "Choose Folder To Add", juce::File());
    chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectDirectories,
                         [this, chooser, libraryId](const juce::FileChooser& fc) {
        auto folder = fc.getResult();
        if (folder == juce::File()) {
            return;
        }

        auto editor = juce::Component::SafePointer<TriggerfishEditor>(this);
        auto* task = new LibraryIndexProgressTask(
            "Adding Files To Database...",
            [libraryId, folder](const triggerfish::LibraryDatabase::ProgressCallback& progress,
                                juce::String& error) {
                return triggerfish::LibraryDatabase::appendFolderToDatabase(
                    libraryId, folder, error, progress);
            },
            [editor, libraryId](bool success, const juce::String& error) {
                if (editor == nullptr) {
                    return;
                }

                if (!success) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Add Files Failed",
                        error.isNotEmpty() ? error : "The folder could not be added to the database.");
                    return;
                }

                editor->ensureLibrarySearchWindow().content().reloadLibraries(libraryId);
            });
        task->launchThread();
    });
}

void TriggerfishEditor::renameLibraryDatabase(const juce::String& libraryId) {
    const auto descriptor = triggerfish::LibraryDatabase::findLibrary(libraryId);
    if (!descriptor.has_value()) {
        return;
    }

    auto* prompt = new juce::AlertWindow(
        "Rename Database",
        "Enter a new name.",
        juce::AlertWindow::NoIcon);
    prompt->addTextEditor("dbName", descriptor->name, "Database Name");
    prompt->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    prompt->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::AlertWindow> safePrompt(prompt);
    prompt->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, safePrompt, libraryId](int result) {
            if (result != 1) {
                return;
            }

            if (safePrompt == nullptr) {
                return;
            }

            juce::String error;
            if (!triggerfish::LibraryDatabase::renameDatabase(libraryId,
                                                              safePrompt->getTextEditorContents("dbName"),
                                                              error)) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Rename Failed",
                    error.isNotEmpty() ? error : "The database could not be renamed.");
                return;
            }

            ensureLibrarySearchWindow().content().reloadLibraries(libraryId);
        }),
        true);
}

void TriggerfishEditor::deleteLibraryDatabase(const juce::String& libraryId) {
    const auto descriptor = triggerfish::LibraryDatabase::findLibrary(libraryId);
    if (!descriptor.has_value()) {
        return;
    }

    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon,
        "Delete Database",
        "Delete \"" + descriptor->name + "\"?",
        "Delete",
        "Cancel",
        this,
        juce::ModalCallbackFunction::create([this, libraryId](int result) {
            if (result == 0) {
                return;
            }

            juce::String error;
            if (!triggerfish::LibraryDatabase::deleteDatabase(libraryId, error)) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Delete Failed",
                    error.isNotEmpty() ? error : "The database could not be deleted.");
                return;
            }

            ensureLibrarySearchWindow().content().reloadLibraries();
        }));
}

void TriggerfishEditor::placeFileOnSelectedLayer(const juce::String& filePath) {
    auto selected = processorRef.controller().selected_layer_index();
    if (!selected.has_value()) {
        return;
    }
    if (processorRef.controller().layer_locked(*selected)) {
        return;
    }

    if (processorRef.controller().is_streaming()) {
        processorRef.controller().stop_streaming_playback();
        if (isTakePlaying_) {
            isTakePlaying_ = false;
            sessionRecorder_.setTakePlayhead(-1.0);
            stopTakeOwnedPicturePlayback(false);
        }
    }

    std::string error;
    if (!processorRef.controller().replace_layer_audio(*selected, filePath.toStdString(), &error)) {
        return;
    }
    clearLayerEditHistory(*selected);
    processorRef.controller().clear_layer_audition_loop(*selected);
    processorRef.controller().set_layer_audition_start(*selected, 0.0);
    playbackTarget_ = PlaybackTarget::FocusLayer;
    invalidateFocusWaveformCache();
    refreshUI();
    toggleFocusTrackPlayback();
}

void TriggerfishEditor::toggleFocusTrackPlayback() {
    auto sel = processorRef.controller().selected_layer_index();
    if (sel.has_value()) {
        playbackTarget_ = PlaybackTarget::FocusLayer;
        std::string err;
        processorRef.controller().start_streaming_audition(*sel, &err);
    }
}

void TriggerfishEditor::wirePluginSessions(bool resetProcessing) {
    auto& layers = processorRef.controller().streaming_layer_states();

    for (auto& [layerIdx, hosts] : layerHosts_) {
        if (layerIdx >= layers.size()) continue;
        auto& layer = layers[layerIdx];
        auto plugins = processorRef.controller().layer_vst3_plugins(layerIdx);
        for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
            auto* host = hosts[i].get();
            if (resetProcessing && host && host->is_configured()) {
                host->reset_processing();
            }
            layer.plugin_sessions[i] = (host && host->is_configured()) ? host : nullptr;
            layer.plugin_bypassed[i] = plugins[i].has_value() && plugins[i]->bypassed;
        }
    }

    auto auxPlugins = processorRef.controller().aux_vst3_plugins();
    for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
        auto* host = auxHosts_[i].get();
        if (resetProcessing && host && host->is_configured()) {
            host->reset_processing();
        }
        processorRef.controller().streaming_mixer().set_aux_plugin_session(
            i,
            (host && host->is_configured()) ? host : nullptr,
            auxPlugins[i].has_value() && auxPlugins[i]->bypassed);
    }
}

void TriggerfishEditor::clearLivePluginSessionPointers() {
    auto& layers = processorRef.controller().streaming_layer_states();
    for (auto& layer : layers) {
        layer.plugin_sessions.fill(nullptr);
        layer.plugin_bypassed.fill(false);
    }
    processorRef.controller().streaming_mixer().clear_aux_plugin_sessions();
}

void TriggerfishEditor::resetHostedPluginsForSessionChange() {
    processorRef.controller().stop_streaming_playback();
    hostLoadGeneration_.fetch_add(1, std::memory_order_relaxed);
    pendingLayerHostLoad_.store(std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
    pendingAuxHostLoad_.store(false, std::memory_order_relaxed);
    focusWaveformDirty_ = true;
    cachedFocusWaveformLayerIndex_.reset();
    cachedFocusWaveformRevision_ = 0;
    cachedFocusWaveformWidth_ = -1;
    clearLivePluginSessionPointers();
    vst3Rack_.clearDisplay();
    auxVst3Rack_.clearDisplay();
    layerHosts_.clear();
    for (auto& host : auxHosts_) {
        host.reset();
    }
}

void TriggerfishEditor::ensureLayerHosts(std::size_t layerIndex) {
    auto plugins = processorRef.controller().layer_vst3_plugins(layerIndex);
    bool hasAnyPlugin = false;
    for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
        if (plugins[i].has_value()) {
            hasAnyPlugin = true;
            break;
        }
    }

    if (!hasAnyPlugin) {
        return;
    }

    auto& hosts = layerHosts_[layerIndex];
    bool allConfigured = true;
    for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
        if (plugins[i].has_value() && !hosts[i]) {
            allConfigured = false;
            break;
        }
    }

    if (!allConfigured) {
        beginAsyncLayerHostLoad(layerIndex);
    }
}

void TriggerfishEditor::ensureAuxHosts() {
    auto plugins = processorRef.controller().aux_vst3_plugins();
    bool hasAnyPlugin = false;
    for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
        if (plugins[i].has_value()) {
            hasAnyPlugin = true;
            break;
        }
    }

    if (!hasAnyPlugin) {
        return;
    }

    bool allConfigured = true;
    for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
        if (plugins[i].has_value() && !auxHosts_[i]) {
            allConfigured = false;
            break;
        }
    }

    if (!allConfigured) {
        beginAsyncAuxHostLoad();
    }
}

void TriggerfishEditor::beginAsyncLayerHostLoad(std::size_t layerIndex) {
    std::size_t expected = std::numeric_limits<std::size_t>::max();
    if (!pendingLayerHostLoad_.compare_exchange_strong(expected, layerIndex, std::memory_order_relaxed)) {
        return;
    }

    auto plugins = processorRef.controller().layer_vst3_plugins(layerIndex);
    const double sampleRate = processorRef.getSampleRate();
    const int blockSize = processorRef.getBlockSize();
    const auto loadGeneration = hostLoadGeneration_.load(std::memory_order_relaxed);
    auto safeThis = juce::Component::SafePointer<TriggerfishEditor>(this);

    juce::MessageManager::callAsync([safeThis, layerIndex, plugins, sampleRate, blockSize, loadGeneration] () mutable {
        if (safeThis == nullptr) {
            return;
        }
        if (safeThis->hostLoadGeneration_.load(std::memory_order_relaxed) != loadGeneration) {
            return;
        }

        auto loadedHosts = std::make_shared<triggerfish::Vst3InsertRack::HostArray>();

        for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
            if (!plugins[i].has_value()) {
                continue;
            }

            auto host = std::make_unique<triggerfish::JuceVst3Host>();
            const auto& ps = *plugins[i];
            juce::String err;
            host->loadPlugin(
                juce::String(ps.module_path),
                juce::String(ps.class_id),
                sampleRate,
                blockSize,
                err,
                juce::String(ps.display_name),
                false);
            if (host->is_configured() && !ps.component_state.empty()) {
                host->setState(ps.component_state.data(),
                               static_cast<int>(ps.component_state.size()));
            }
            if (host->is_configured()) {
                (*loadedHosts)[i] = std::move(host);
            }
        }

        if (safeThis->hostLoadGeneration_.load(std::memory_order_relaxed) != loadGeneration) {
            return;
        }
        safeThis->pendingLayerHostLoad_.store(std::numeric_limits<std::size_t>::max(),
                                              std::memory_order_relaxed);
        safeThis->layerHosts_[layerIndex] = std::move(*loadedHosts);
        safeThis->wirePluginSessions();

        if (safeThis->processorRef.controller().selected_layer_index().has_value() &&
            *safeThis->processorRef.controller().selected_layer_index() == layerIndex) {
            safeThis->vst3Rack_.setHosts(&safeThis->layerHosts_[layerIndex],
                                         safeThis->processorRef.getSampleRate(),
                                         safeThis->processorRef.getBlockSize());
            safeThis->vst3Rack_.updateDisplay(safeThis->processorRef.controller(), layerIndex);
        }
    });
}

void TriggerfishEditor::beginAsyncAuxHostLoad() {
    bool expected = false;
    if (!pendingAuxHostLoad_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }

    auto plugins = processorRef.controller().aux_vst3_plugins();
    const double sampleRate = processorRef.getSampleRate();
    const int blockSize = processorRef.getBlockSize();
    const auto loadGeneration = hostLoadGeneration_.load(std::memory_order_relaxed);
    auto safeThis = juce::Component::SafePointer<TriggerfishEditor>(this);

    juce::MessageManager::callAsync([safeThis, plugins, sampleRate, blockSize, loadGeneration] () mutable {
        if (safeThis == nullptr) {
            return;
        }
        if (safeThis->hostLoadGeneration_.load(std::memory_order_relaxed) != loadGeneration) {
            return;
        }

        auto loadedHosts = std::make_shared<triggerfish::Vst3InsertRack::HostArray>();

        for (std::size_t i = 0; i < radium::StreamingMixer::kMaxPluginSlots; ++i) {
            if (!plugins[i].has_value()) {
                continue;
            }

            auto host = std::make_unique<triggerfish::JuceVst3Host>();
            const auto& ps = *plugins[i];
            juce::String err;
            host->loadPlugin(
                juce::String(ps.module_path),
                juce::String(ps.class_id),
                sampleRate,
                blockSize,
                err,
                juce::String(ps.display_name),
                false);
            if (host->is_configured() && !ps.component_state.empty()) {
                host->setState(ps.component_state.data(),
                               static_cast<int>(ps.component_state.size()));
            }
            if (host->is_configured()) {
                (*loadedHosts)[i] = std::move(host);
            }
        }

        if (safeThis->hostLoadGeneration_.load(std::memory_order_relaxed) != loadGeneration) {
            return;
        }
        safeThis->pendingAuxHostLoad_.store(false, std::memory_order_relaxed);
        safeThis->auxHosts_ = std::move(*loadedHosts);
        safeThis->wirePluginSessions();
        safeThis->auxVst3Rack_.setHosts(&safeThis->auxHosts_,
                                        safeThis->processorRef.getSampleRate(),
                                        safeThis->processorRef.getBlockSize());
        safeThis->auxVst3Rack_.updateDisplay(safeThis->processorRef.controller().aux_vst3_plugins());
    });
}

#include "LayerListComponent.h"
#include "../LookAndFeel_Radium.h"
#include <cmath>

namespace triggerfish {
namespace {
constexpr double kTrackGainMax = 3.9810717055; // +12 dB
}

// --- LayerRowComponent ---

LayerRowComponent::LayerRowComponent() {
    nameLabel.setFont(juce::FontOptions(13.0f));
    nameLabel.setColour(juce::Label::textColourId, colours::textPrimary);
    nameLabel.setEditable(false, true, false);
    nameLabel.setInterceptsMouseClicks(true, true);
    nameLabel.onDoubleClick = [this](const juce::MouseEvent&) {
        editingName_ = true;
        nameLabel.showEditor();
    };
    nameLabel.onEditorShow = [this] {
        editingName_ = true;
        if (auto* editor = nameLabel.getCurrentTextEditor()) {
            editor->selectAll();
        }
    };
    nameLabel.onEditorHide = [this] {
        if (onRename) {
            onRename(layerState_.index, nameLabel.getText());
        }
        editingName_ = false;
    };
    addAndMakeVisible(nameLabel);

    muteButton.setClickingTogglesState(true);
    muteButton.onClick = [this] {
        if (onMuteToggle) onMuteToggle(layerState_.index, muteButton.getToggleState());
    };
    addAndMakeVisible(muteButton);

    soloButton.setClickingTogglesState(true);
    soloButton.onClick = [this] {
        if (onSoloToggle) onSoloToggle(layerState_.index, soloButton.getToggleState());
    };
    addAndMakeVisible(soloButton);

    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setRange(0.0, kTrackGainMax, 0.01);
    gainSlider.setValue(1.0, juce::dontSendNotification);
    gainSlider.setCtrlClickResetValue(1.0);
    gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.onValueChange = [this] {
        if (onGainChange) onGainChange(layerState_.index, gainSlider.getValue());
    };
    addAndMakeVisible(gainSlider);

    autoSplitButton_.onClick = [this] {
        if (onAutoSplit) onAutoSplit(layerState_.index);
    };
    addAndMakeVisible(autoSplitButton_);

    clearAudioButton_.onClick = [this] {
        if (onClearAudio) onClearAudio(layerState_.index);
    };
    addAndMakeVisible(clearAudioButton_);

    lockButton_.setClickingTogglesState(true);
    lockButton_.onClick = [this] {
        if (onLockToggle) onLockToggle(layerState_.index, lockButton_.getToggleState());
    };
    addAndMakeVisible(lockButton_);
}

void LayerRowComponent::update(const radium::VisibleLayerState& state) {
    layerState_ = state;
    juce::String displayName = state.label.empty()
        ? ("Layer " + juce::String(static_cast<int>(state.index + 1)))
        : juce::String(state.label);
    nameLabel.setText(displayName, juce::dontSendNotification);
    nameLabel.setColour(juce::Label::textColourId,
                         state.selected ? colours::textAccent : colours::textPrimary);
    muteButton.setToggleState(state.mute, juce::dontSendNotification);
    soloButton.setToggleState(state.solo, juce::dontSendNotification);
    lockButton_.setToggleState(state.locked, juce::dontSendNotification);
    gainSlider.setValue(state.gain, juce::dontSendNotification);
    muteButton.setEnabled(state.has_audio);
    soloButton.setEnabled(state.has_audio);
    gainSlider.setEnabled(state.has_audio);
    clearAudioButton_.setEnabled(state.has_audio);
    autoSplitButton_.setEnabled(state.has_audio);
    repaint();
}

void LayerRowComponent::setMiniPeaks(const std::vector<float>& peaksMax, const std::vector<float>& peaksMin,
                                     const std::vector<float>& peaksRightMax, const std::vector<float>& peaksRightMin) {
    waveformPeaks_ = peaksMax;
    waveformPeaksMin_ = peaksMin;
    waveformPeaksRight_ = peaksRightMax;
    waveformPeaksRightMin_ = peaksRightMin;
    repaint();
}

void LayerRowComponent::setMiniRegions(const std::vector<radium::LayerWaveformOverview::AuthoredRegion>& regions) {
    miniRegions_ = regions;
    repaint();
}

void LayerRowComponent::setPlayheadPosition(double normalizedPos) {
    if (std::abs(playheadPos_ - normalizedPos) > 0.0005) {
        playheadPos_ = normalizedPos;
        repaint();
    }
}

void LayerRowComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    auto rowColour = colours::panel.interpolatedWith(colours::sectionLayers, layerState_.selected ? 0.58f : 0.42f);
    g.setColour(rowColour);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(colours::sectionLayers.brighter(0.16f).withAlpha(0.38f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    // Mini waveform area
    auto waveArea = getLocalBounds().removeFromBottom(28).reduced(6, 2);
    g.setColour(colours::waveBg);
    g.fillRect(waveArea);

    if (!layerState_.has_audio) {
        g.setFont(juce::FontOptions(10.0f));
        g.setColour(colours::textDim);
        g.drawText("No audio", waveArea, juce::Justification::centred);
    } else if (!waveformPeaks_.empty()) {
        const int w = waveArea.getWidth();
        const int h = waveArea.getHeight();
        const float cx = static_cast<float>(waveArea.getX());
        const bool stereo = !waveformPeaksRight_.empty();

        auto lerp = [](const std::vector<float>& v, double fi) -> float {
            const auto mx = static_cast<double>(v.size() - 1);
            const double ci = std::clamp(fi, 0.0, mx);
            const auto i0 = static_cast<std::size_t>(ci);
            const auto i1 = std::min(i0 + 1, v.size() - 1);
            const float f = static_cast<float>(ci - static_cast<double>(i0));
            return v[i0] + (v[i1] - v[i0]) * f;
        };

        // Helper to draw one channel's envelope
        auto drawChannel = [&](const std::vector<float>& peaksMax,
                               const std::vector<float>& peaksMin,
                               float top, float channelH) {
            const float cy = top + channelH * 0.5f;
            const float amp = channelH * 0.46f;
            const auto n = static_cast<double>(peaksMax.size() - 1);
            const bool hasMin = peaksMin.size() == peaksMax.size();

            juce::Path path;
            for (int x = 0; x <= w; ++x) {
                double fi = static_cast<double>(x) / static_cast<double>(w) * n;
                float maxVal = lerp(peaksMax, fi);
                float y = cy - maxVal * amp;
                if (x == 0) path.startNewSubPath(cx, y);
                else path.lineTo(cx + static_cast<float>(x), y);
            }
            for (int x = w; x >= 0; --x) {
                double fi = static_cast<double>(x) / static_cast<double>(w) * n;
                float minVal = hasMin ? lerp(peaksMin, fi) : -lerp(peaksMax, fi);
                float y = cy - minVal * amp;
                path.lineTo(cx + static_cast<float>(x), y);
            }
            path.closeSubPath();

            g.setColour(colours::waveform.withAlpha(0.35f));
            g.fillPath(path);
            g.setColour(colours::waveform.withAlpha(0.8f));
            g.strokePath(path, juce::PathStrokeType(0.75f));

            g.setColour(colours::border.withAlpha(0.3f));
            g.drawHorizontalLine(static_cast<int>(cy), cx, cx + static_cast<float>(w));
        };

        if (stereo) {
            float halfH = static_cast<float>(h) * 0.5f;
            drawChannel(waveformPeaks_, waveformPeaksMin_,
                        static_cast<float>(waveArea.getY()), halfH);
            drawChannel(waveformPeaksRight_, waveformPeaksRightMin_,
                        static_cast<float>(waveArea.getY()) + halfH, halfH);
        } else {
            drawChannel(waveformPeaks_, waveformPeaksMin_,
                        static_cast<float>(waveArea.getY()), static_cast<float>(h));
        }

        // Draw trigger regions on mini waveform
        for (const auto& region : miniRegions_) {
            float lx = cx + static_cast<float>(region.start * w);
            float rx = cx + static_cast<float>(region.end * w);
            g.setColour(colours::regionFill.withAlpha(0.5f));
            g.fillRect(lx, static_cast<float>(waveArea.getY()), rx - lx, static_cast<float>(h));
            g.setColour(colours::regionHandle);
            g.fillRect(lx, static_cast<float>(waveArea.getY()), 2.0f, static_cast<float>(h));
            g.fillRect(rx - 1.0f, static_cast<float>(waveArea.getY()), 2.0f, static_cast<float>(h));
        }

        // Draw playhead on mini waveform
        if (playheadPos_ >= 0.0 && playheadPos_ <= 1.0) {
            int phX = waveArea.getX() + static_cast<int>(playheadPos_ * w);
            g.setColour(juce::Colours::white);
            g.drawVerticalLine(phX,
                               static_cast<float>(waveArea.getY()),
                               static_cast<float>(waveArea.getBottom()));
        }
    }
}

void LayerRowComponent::resized() {
    auto area = getLocalBounds().reduced(6, 4);
    auto top = area.removeFromTop(24);
    // Buttons on the right first, then name gets remaining space
    autoSplitButton_.setBounds(top.removeFromRight(38));
    top.removeFromRight(2);
    lockButton_.setBounds(top.removeFromRight(50));
    top.removeFromRight(2);
    clearAudioButton_.setBounds(top.removeFromRight(24));
    top.removeFromRight(4);
    gainSlider.setBounds(top.removeFromRight(std::min(100, top.getWidth() / 3)));
    top.removeFromRight(6);
    soloButton.setBounds(top.removeFromRight(28));
    top.removeFromRight(2);
    muteButton.setBounds(top.removeFromRight(28));
    top.removeFromRight(4);
    nameLabel.setBounds(top);
}

void LayerRowComponent::mouseDown(const juce::MouseEvent&) {
    if (editingName_) {
        return;
    }
    if (onSelect) onSelect(layerState_.index);
}

void LayerRowComponent::mouseWheelMove(const juce::MouseEvent& e,
                                        const juce::MouseWheelDetails& wheel) {
    // Forward to parent LayerListComponent for scrolling
    if (auto* parent = getParentComponent())
        parent->mouseWheelMove(e, wheel);
}

// --- LayerListComponent ---

LayerListComponent::LayerListComponent() {
    headerLabel.setText("LAYERS", juce::dontSendNotification);
    headerLabel.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    headerLabel.setColour(juce::Label::textColourId, colours::textPrimary);
    addAndMakeVisible(headerLabel);

    infoLabel.setFont(juce::FontOptions(11.0f));
    infoLabel.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(infoLabel);

    projectNameLabel.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    projectNameLabel.setColour(juce::Label::textColourId, colours::textPrimary.withAlpha(0.92f));
    projectNameLabel.setJustificationType(juce::Justification::centred);
    projectNameLabel.setText("Untitled Project", juce::dontSendNotification);
    addAndMakeVisible(projectNameLabel);

    scrollBar_.addListener(this);
    scrollBar_.setAutoHide(true);
    addAndMakeVisible(scrollBar_);
}

void LayerListComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    auto bg = colours::panel.interpolatedWith(colours::sectionLayers, 0.48f);
    g.setColour(bg);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(colours::sectionLayers.brighter(0.18f).withAlpha(0.48f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
}

void LayerListComponent::ensureRowCount(std::size_t count) {
    while (rows_.size() < count) {
        auto row = std::make_unique<LayerRowComponent>();
        row->setVisible(false);
        row->onSelect = [this](std::size_t idx) { if (onLayerSelect) onLayerSelect(idx); };
        row->onMuteToggle = [this](std::size_t idx, bool m) { if (onMuteToggle) onMuteToggle(idx, m); };
        row->onSoloToggle = [this](std::size_t idx, bool s) { if (onSoloToggle) onSoloToggle(idx, s); };
        row->onGainChange = [this](std::size_t idx, double g) { if (onGainChange) onGainChange(idx, g); };
        row->onAutoSplit = [this](std::size_t idx) { if (onAutoSplit) onAutoSplit(idx); };
        row->onClearAudio = [this](std::size_t idx) { if (onClearLayer) onClearLayer(idx); };
        row->onLockToggle = [this](std::size_t idx, bool locked) { if (onLayerLockToggle) onLayerLockToggle(idx, locked); };
        row->onRename = [this](std::size_t idx, const juce::String& name) { if (onLayerRename) onLayerRename(idx, name); };
        addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
}

void LayerListComponent::updateFromController(radium::AppController& controller) {
    lastController_ = &controller;
    totalLayers_ = static_cast<int>(controller.layer_count());
    ensureRowCount(static_cast<std::size_t>(totalLayers_));
    scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, totalLayers_ - kVisibleRows));

    // Update info label with layer counts
    if (totalLayers_ > 0) {
        auto all = controller.visible_layers_from(0, static_cast<std::size_t>(totalLayers_));
        int withAudio = 0;
        for (const auto& l : all) {
            if (l.has_audio) ++withAudio;
        }
        infoLabel.setText("Active: " + juce::String(withAudio) + "/" + juce::String(totalLayers_),
                          juce::dontSendNotification);
    } else {
        infoLabel.setText("", juce::dontSendNotification);
    }

    // Update scrollbar range
    scrollBar_.setRangeLimits(0.0, static_cast<double>(totalLayers_));
    scrollBar_.setCurrentRange(static_cast<double>(scrollOffset_),
                                static_cast<double>(std::min(kVisibleRows, totalLayers_)));

    applyScroll();
}

void LayerListComponent::applyScroll() {
    if (lastController_ == nullptr) return;

    auto layers = lastController_->visible_layers_from(
        static_cast<std::size_t>(scrollOffset_), kVisibleRows);

    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (i < layers.size()) {
            rows_[i]->update(layers[i]);
            // Fetch mini waveform peaks — use enough buckets for the row width
            auto rowWidth = std::max(128, rows_[i]->getWidth());
            auto overview = lastController_->layer_waveform(
                layers[i].index, static_cast<std::size_t>(rowWidth));
            if (overview.has_value() && overview->available) {
                rows_[i]->setMiniPeaks(overview->peaks, overview->peaks_min,
                                       overview->peaks_right, overview->peaks_right_min);
                rows_[i]->setMiniRegions(overview->authored_regions);
            } else {
                rows_[i]->setMiniPeaks({}, {});
                rows_[i]->setMiniRegions({});
            }
            // Update playhead position from streaming state
            auto pos = lastController_->layer_streaming_position(layers[i].index);
            rows_[i]->setPlayheadPosition(pos.value_or(-1.0));
            rows_[i]->setVisible(true);
        } else {
            rows_[i]->setVisible(false);
        }
    }
    resized();
}

void LayerListComponent::resized() {
    auto area = getLocalBounds();
    auto headerRow = area.removeFromTop(22);
    headerLabel.setBounds(headerRow.removeFromLeft(70));
    infoLabel.setBounds(headerRow.removeFromLeft(84));
    projectNameLabel.setBounds(headerRow);
    area.removeFromTop(2);

    const int rowHeight = 60;
    const int rowSpacing = 2;
    const int visibleHeight = kVisibleRows * rowHeight + (kVisibleRows - 1) * rowSpacing;

    // Scrollbar on the right, limited to the visible rows height
    if (totalLayers_ > kVisibleRows) {
        scrollBar_.setVisible(true);
        auto scrollArea = area.removeFromRight(14);
        scrollArea.setHeight(visibleHeight);
        scrollBar_.setBounds(scrollArea);
    } else {
        scrollBar_.setVisible(false);
    }

    for (auto& row : rows_) {
        if (row->isVisible()) {
            row->setBounds(area.removeFromTop(rowHeight));
            area.removeFromTop(rowSpacing);
        }
    }
}

void LayerListComponent::setProjectName(const juce::String& name) {
    projectNameLabel.setText(name.isEmpty() ? "Untitled Project" : name, juce::dontSendNotification);
}

void LayerListComponent::mouseWheelMove(const juce::MouseEvent&,
                                         const juce::MouseWheelDetails& wheel) {
    if (totalLayers_ <= kVisibleRows) return;
    int delta = wheel.deltaY > 0 ? -1 : 1;
    scrollOffset_ = std::clamp(scrollOffset_ + delta, 0, totalLayers_ - kVisibleRows);
    scrollBar_.setCurrentRange(static_cast<double>(scrollOffset_),
                                static_cast<double>(std::min(kVisibleRows, totalLayers_)));
    applyScroll();
}

void LayerListComponent::scrollBarMoved(juce::ScrollBar*, double newRangeStart) {
    scrollOffset_ = std::clamp(static_cast<int>(newRangeStart), 0,
                                std::max(0, totalLayers_ - kVisibleRows));
    applyScroll();
}

void LayerListComponent::updatePlayheadPositions(radium::AppController& controller) {
    // Lightweight update: only read atomic playhead positions, no waveform recomputation
    auto layers = controller.visible_layers_from(
        static_cast<std::size_t>(scrollOffset_), kVisibleRows);
    for (std::size_t i = 0; i < layers.size() && i < rows_.size(); ++i) {
        auto pos = controller.layer_streaming_position(layers[i].index);
        rows_[i]->setPlayheadPosition(pos.value_or(-1.0));
    }
}

bool LayerListComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files) {
        if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".flac") ||
            f.endsWithIgnoreCase(".aif") || f.endsWithIgnoreCase(".aiff") ||
            f.endsWithIgnoreCase(".mp3") || f.endsWithIgnoreCase(".ogg")) {
            return true;
        }
    }
    return false;
}

void LayerListComponent::filesDropped(const juce::StringArray& files, int /*x*/, int y) {
    // Determine target layer from drop y position
    constexpr int headerH = 24; // header row + spacing
    constexpr int rowHeight = 60;
    constexpr int rowSpacing = 2;
    int relY = y - headerH;
    int visibleRow = (relY >= 0) ? relY / (rowHeight + rowSpacing) : 0;
    std::size_t target = static_cast<std::size_t>(
        std::clamp(scrollOffset_ + visibleRow, 0, std::max(0, totalLayers_ - 1)));

    for (const auto& f : files) {
        if (onAudioDropped) {
            onAudioDropped(target, f);
        }
    }
}

}  // namespace triggerfish

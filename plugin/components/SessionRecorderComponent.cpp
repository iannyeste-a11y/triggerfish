#include "SessionRecorderComponent.h"
#include "../LookAndFeel_Radium.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <thread>

namespace triggerfish {

namespace {

struct DecodedTakeWaveform {
    std::vector<std::vector<float>> channels;
    double durationSeconds = 0.0;
};

DecodedTakeWaveform decodeTakeWaveform(const std::filesystem::path& path, int targetWidth) {
    DecodedTakeWaveform decoded;

    if (!std::filesystem::exists(path)) {
        return decoded;
    }

    juce::File file(juce::String(path.string()));
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr) {
        return decoded;
    }

    const auto totalFrames = static_cast<int>(reader->lengthInSamples);
    decoded.durationSeconds =
        static_cast<double>(reader->lengthInSamples) /
        static_cast<double>(std::max(1.0, reader->sampleRate));

    const int buckets = std::max(100, targetWidth);
    const int channelCount = std::max(1, static_cast<int>(reader->numChannels));
    decoded.channels.assign(static_cast<std::size_t>(channelCount),
                            std::vector<float>(static_cast<std::size_t>(buckets), 0.0f));

    const int framesPerBucket = std::max(1, totalFrames / buckets);
    juce::AudioBuffer<float> tempBuf(static_cast<int>(reader->numChannels), framesPerBucket);

    for (int b = 0; b < buckets; ++b) {
        const juce::int64 startSample = static_cast<juce::int64>(b) * totalFrames / buckets;
        const int numSamples = std::min(framesPerBucket,
            static_cast<int>(reader->lengthInSamples - startSample));
        if (numSamples <= 0) {
            break;
        }

        std::vector<float*> channelPtrs(static_cast<std::size_t>(channelCount));
        for (int channel = 0; channel < channelCount; ++channel) {
            channelPtrs[static_cast<std::size_t>(channel)] = tempBuf.getWritePointer(channel);
        }
        reader->read(channelPtrs.data(), channelCount, startSample, numSamples);

        for (int channel = 0; channel < channelCount; ++channel) {
            float maxValue = 0.0f;
            const float* data = tempBuf.getReadPointer(channel);
            for (int i = 0; i < numSamples; ++i) {
                const float v = std::abs(data[i]);
                if (v > maxValue) {
                    maxValue = v;
                }
            }
            decoded.channels[static_cast<std::size_t>(channel)][static_cast<std::size_t>(b)] = maxValue;
        }
    }

    return decoded;
}

}  // namespace

SessionRecorderComponent::SessionRecorderComponent() {
    titleLabel_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, colours::textPrimary);
    addAndMakeVisible(titleLabel_);

    newButton_.setColour(juce::ToggleButton::textColourId, colours::textPrimary);
    newButton_.setColour(juce::ToggleButton::tickColourId, juce::Colours::red);
    newButton_.setColour(juce::TextButton::buttonOnColourId, colours::accentRecord);
    newButton_.onClick = [this] {
        if (onNewToggle) onNewToggle(newButton_.getToggleState());
    };
    addAndMakeVisible(newButton_);

    punchButton_.setColour(juce::ToggleButton::textColourId, colours::textPrimary);
    punchButton_.setColour(juce::ToggleButton::tickColourId, colours::accentRecord);
    punchButton_.setColour(juce::TextButton::buttonOnColourId, colours::accentRecord);
    punchButton_.onClick = [this] {
        if (onPunchToggle) onPunchToggle(punchButton_.getToggleState());
    };
    addAndMakeVisible(punchButton_);

    takeList_.setColour(juce::ListBox::backgroundColourId, colours::waveBg);
    takeList_.setRowHeight(20);
    addAndMakeVisible(takeList_);

    playButton_.onClick = [this] {
        if (selectedIndex_.has_value()) {
            if (onPlayTake) onPlayTake(*selectedIndex_);
        }
    };
    addAndMakeVisible(playButton_);

    exportButton_.onClick = [this] {
        if (onExport) onExport();
    };
    addAndMakeVisible(exportButton_);

    renameButton_.onClick = [this] {
        auto* aw = new juce::AlertWindow("Rename Take", "Enter new name:", juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor("name", "", "Name:");
        aw->addButton("OK", 1);
        aw->addButton("Cancel", 0);
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw](int result) {
                if (result == 1) {
                    auto name = aw->getTextEditorContents("name").toStdString();
                    if (!name.empty() && onRename) onRename(name);
                }
                delete aw;
            }), false);
    };
    addAndMakeVisible(renameButton_);
}

juce::Rectangle<int> SessionRecorderComponent::getWaveformArea() const {
    auto area = getLocalBounds().reduced(4, 2);
    // Waveform area is the right portion after the take list
    auto leftWidth = std::min(180, area.getWidth() / 3);
    area.removeFromLeft(leftWidth + 4);
    area.removeFromTop(22); // header row
    return area;
}

double SessionRecorderComponent::xToWaveformNormalized(int x) const {
    const auto waveArea = getWaveformArea();
    if (waveArea.getWidth() <= 0) {
        return 0.0;
    }
    const double frac = static_cast<double>(x - waveArea.getX()) /
                        static_cast<double>(waveArea.getWidth());
    return std::clamp(frac, 0.0, 1.0);
}

void SessionRecorderComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::panel);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(colours::accentRecord.withAlpha(0.4f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 2.0f);

    // Draw waveform in the right area
    auto waveArea = getWaveformArea();
    g.setColour(colours::waveBg);
    g.fillRect(waveArea);
    g.setColour(colours::accentRecord.withAlpha(0.32f));
    g.drawRect(waveArea, 2);

    const bool showTakeWaveform =
        !takeChannelPeaks_.empty() && selectedIndex_.has_value() && (!isRecording_ || isPunching_);
    const auto& channelPeaks = showTakeWaveform ? takeChannelPeaks_ : recChannelPeaks_;

    if (!channelPeaks.empty()) {
        const int w = waveArea.getWidth();
        const int h = waveArea.getHeight();
        const float cx = static_cast<float>(waveArea.getX());
        const int channelCount = static_cast<int>(channelPeaks.size());
        const float gap = channelCount > 2 ? 3.0f : 0.0f;
        const float channelH = (static_cast<float>(h) - gap * static_cast<float>(std::max(0, channelCount - 1)))
            / static_cast<float>(std::max(1, channelCount));

        auto channelLabel = [channelCount](int index) -> juce::String {
            if (channelCount == 8) {
                switch (index) {
                    case 0: return "L";
                    case 1: return "R";
                    case 2: return "C";
                    case 3: return "LFE";
                    case 4: return "Ls";
                    case 5: return "Rs";
                    case 6: return "Lrs";
                    case 7: return "Rrs";
                    default: return juce::String(index + 1);
                }
            }
            if (channelCount == 7) {
                switch (index) {
                    case 0: return "L";
                    case 1: return "R";
                    case 2: return "C";
                    case 3: return "Ls";
                    case 4: return "Rs";
                    case 5: return "Lrs";
                    case 6: return "Rrs";
                    default: return juce::String(index + 1);
                }
            }
            if (channelCount == 5) {
                switch (index) {
                    case 0: return "L";
                    case 1: return "R";
                    case 2: return "C";
                    case 3: return "Ls";
                    case 4: return "Rs";
                    default: return juce::String(index + 1);
                }
            }
            switch (index) {
                case 0: return "L";
                case 1: return "R";
                case 2: return "C";
                case 3: return "LFE";
                case 4: return "Ls";
                case 5: return "Rs";
                default: return juce::String(index + 1);
            }
        };

        auto drawChannel = [&](const std::vector<float>& peaks, float top, float ch, int channelIndex) {
            const float cy = top + ch * 0.5f;
            const float amp = ch * 0.45f;
            juce::Path path;
            // Top envelope
            for (int x = 0; x < w && x < static_cast<int>(peaks.size()); ++x) {
                float y = cy - peaks[static_cast<std::size_t>(x)] * amp;
                if (x == 0) path.startNewSubPath(cx + static_cast<float>(x), y);
                else path.lineTo(cx + static_cast<float>(x), y);
            }
            // Bottom envelope (mirror)
            for (int x = std::min(w, static_cast<int>(peaks.size())) - 1; x >= 0; --x) {
                float y = cy + peaks[static_cast<std::size_t>(x)] * amp;
                path.lineTo(cx + static_cast<float>(x), y);
            }
            path.closeSubPath();

            auto colour = colours::waveformRecord;
            g.setColour(colour.withAlpha(0.35f));
            g.fillPath(path);
            g.setColour(colour.withAlpha(0.8f));
            g.strokePath(path, juce::PathStrokeType(0.75f));

            // Centre line
            g.setColour(colours::border.withAlpha(0.3f));
            g.drawHorizontalLine(static_cast<int>(cy), cx, cx + static_cast<float>(w));

            if (channelCount > 2) {
                g.setColour(colours::textDim.withAlpha(0.8f));
                g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                g.drawText(channelLabel(channelIndex),
                           waveArea.getX() + 3,
                           static_cast<int>(top),
                           28,
                           static_cast<int>(ch),
                           juce::Justification::centredLeft,
                           false);
            }
        };

        float top = static_cast<float>(waveArea.getY());
        for (int channel = 0; channel < channelCount; ++channel) {
            drawChannel(channelPeaks[static_cast<std::size_t>(channel)], top, channelH, channel);
            top += channelH + gap;
        }

        if (selectedIndex_.has_value() && punchInRegion_.has_value() &&
            punchInTakeIndex_ == *selectedIndex_) {
            const auto [start, end] = *punchInRegion_;
            const float left = static_cast<float>(waveArea.getX()) +
                               static_cast<float>(start) * static_cast<float>(waveArea.getWidth());
            const float right = static_cast<float>(waveArea.getX()) +
                                static_cast<float>(end) * static_cast<float>(waveArea.getWidth());
            g.setColour(colours::regionFill.withAlpha(0.35f));
            g.fillRect(left, static_cast<float>(waveArea.getY()),
                       right - left, static_cast<float>(waveArea.getHeight()));
            g.setColour(colours::regionHandle);
            g.drawVerticalLine(static_cast<int>(left),
                               static_cast<float>(waveArea.getY()),
                               static_cast<float>(waveArea.getBottom()));
            g.drawVerticalLine(static_cast<int>(right),
                               static_cast<float>(waveArea.getY()),
                               static_cast<float>(waveArea.getBottom()));
        }

        if (showTakeWaveform && !punchPreviewChannelPeaks_.empty() &&
            punchPreviewEndNorm_ > punchPreviewStartNorm_) {
            const float overlayLeft = static_cast<float>(waveArea.getX()) +
                                      static_cast<float>(punchPreviewStartNorm_) * static_cast<float>(waveArea.getWidth());
            const float overlayRight = static_cast<float>(waveArea.getX()) +
                                       static_cast<float>(punchPreviewEndNorm_) * static_cast<float>(waveArea.getWidth());
            const float overlayWidth = std::max(1.0f, overlayRight - overlayLeft);
            auto drawOverlayChannel = [&](const std::vector<float>& peaks, float top, float ch) {
                const float cy = top + ch * 0.5f;
                const float amp = ch * 0.45f;
                juce::Path path;
                const int overlayW = static_cast<int>(std::round(overlayWidth));
                for (int x = 0; x < overlayW && x < static_cast<int>(peaks.size()); ++x) {
                    const float y = cy - peaks[static_cast<std::size_t>(x)] * amp;
                    if (x == 0) path.startNewSubPath(overlayLeft + static_cast<float>(x), y);
                    else path.lineTo(overlayLeft + static_cast<float>(x), y);
                }
                for (int x = std::min(overlayW, static_cast<int>(peaks.size())) - 1; x >= 0; --x) {
                    const float y = cy + peaks[static_cast<std::size_t>(x)] * amp;
                    path.lineTo(overlayLeft + static_cast<float>(x), y);
                }
                path.closeSubPath();

                g.setColour(colours::accentRecord.withAlpha(0.30f));
                g.fillPath(path);
                g.setColour(colours::accentRecord.withAlpha(0.85f));
                g.strokePath(path, juce::PathStrokeType(0.8f));
            };

            float overlayTop = static_cast<float>(waveArea.getY());
            for (std::size_t channel = 0; channel < punchPreviewChannelPeaks_.size(); ++channel) {
                drawOverlayChannel(punchPreviewChannelPeaks_[channel], overlayTop, channelH);
                overlayTop += channelH + gap;
            }
        }

        if (selectedIndex_.has_value()) {
            const float cueX = static_cast<float>(waveArea.getX()) +
                               static_cast<float>(cuePlayhead_) * static_cast<float>(waveArea.getWidth());
            g.setColour(colours::accentRecord.withAlpha(0.85f));
            g.drawVerticalLine(static_cast<int>(cueX),
                               static_cast<float>(waveArea.getY()),
                               static_cast<float>(waveArea.getBottom()));
        }
    } else {
        g.setFont(juce::FontOptions(11.0f));
        g.setColour(colours::textDim);
        if (isRecording_) {
            g.drawText("Recording...", waveArea, juce::Justification::centred);
        } else if (takes_.empty()) {
            g.drawText("No takes recorded", waveArea, juce::Justification::centred);
        } else {
            g.drawText("Select a take", waveArea, juce::Justification::centred);
        }
    }

    // Draw playhead for take playback
    if (isTakePlaying_ && takePlayhead_ >= 0.0 && takePlayhead_ <= 1.0) {
        const float px = static_cast<float>(waveArea.getX()) +
            static_cast<float>(takePlayhead_) * static_cast<float>(waveArea.getWidth());
        g.setColour(juce::Colours::white);
        g.drawVerticalLine(static_cast<int>(px),
            static_cast<float>(waveArea.getY()),
            static_cast<float>(waveArea.getBottom()));
    }
}

void SessionRecorderComponent::resized() {
    auto area = getLocalBounds().reduced(4, 2);

    // Left panel: header, arm, take list, buttons
    auto leftWidth = std::min(180, area.getWidth() / 3);
    auto left = area.removeFromLeft(leftWidth);

    auto headerRow = left.removeFromTop(22);
    constexpr int titleWidth = 78;
    constexpr int headerGap = 4;
    titleLabel_.setBounds(headerRow.removeFromLeft(std::min(titleWidth, headerRow.getWidth())));
    headerRow.removeFromLeft(std::min(headerGap, headerRow.getWidth()));
    const int buttonWidth = std::max(0, (headerRow.getWidth() - headerGap) / 2);
    newButton_.setBounds(headerRow.removeFromLeft(buttonWidth));
    headerRow.removeFromLeft(std::min(headerGap, headerRow.getWidth()));
    punchButton_.setBounds(headerRow.removeFromLeft(buttonWidth));

    left.removeFromTop(2);
    auto buttons = left.removeFromBottom(24);
    auto btnW = buttons.getWidth() / 3 - 2;
    playButton_.setBounds(buttons.removeFromLeft(btnW));
    buttons.removeFromLeft(3);
    exportButton_.setBounds(buttons.removeFromLeft(btnW));
    buttons.removeFromLeft(3);
    renameButton_.setBounds(buttons);
    left.removeFromBottom(2);

    takeList_.setBounds(left);
}

void SessionRecorderComponent::updateFromController(radium::AppController& controller) {
    takes_ = controller.session_recordings();
    selectedIndex_ = controller.selected_session_recording_index();
    isRecording_ = controller.session_recording_armed();
    takeList_.updateContent();
    if (selectedIndex_.has_value()) {
        takeList_.selectRow(static_cast<int>(*selectedIndex_));
    }

    if (!selectedIndex_.has_value() || punchInTakeIndex_ >= takes_.size() ||
        (selectedIndex_.has_value() && punchInTakeIndex_ != *selectedIndex_)) {
        punchInRegion_.reset();
        punchInTakeIndex_ = ~std::size_t(0);
    }

    // Invalidate cached waveform if no selection or take list changed
    if (!selectedIndex_.has_value() ||
        (takePeaksTakeIndex_ != ~std::size_t(0) &&
         (takePeaksTakeIndex_ >= takes_.size() ||
          takes_[takePeaksTakeIndex_].path != takePeaksCachedPath_))) {
        takeChannelPeaks_.clear();
        takePeaksTakeIndex_ = ~std::size_t(0);
    }

    // Load take waveform if selection changed
    if (selectedIndex_.has_value() && *selectedIndex_ != takePeaksTakeIndex_ &&
        *selectedIndex_ < takes_.size()) {
        takePeaksTakeIndex_ = *selectedIndex_;
        takePeaksCachedPath_ = takes_[*selectedIndex_].path;
        takeChannelPeaks_.clear();
        selectedTakeDurationSeconds_ = 0.0;
        const auto path = takes_[*selectedIndex_].path;
        const auto generation = waveformLoadGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto safeThis = juce::Component::SafePointer<SessionRecorderComponent>(this);
        const int targetWidth = std::max(100, getWaveformArea().getWidth());

        std::thread([safeThis, generation, path, targetWidth] {
            DecodedTakeWaveform decoded;
            try {
                decoded = decodeTakeWaveform(path, targetWidth);
            } catch (...) {
                return;
            }

            juce::MessageManager::callAsync([safeThis, generation, decoded = std::move(decoded), path] () mutable {
                if (safeThis == nullptr) {
                    return;
                }

                if (safeThis->waveformLoadGeneration_.load(std::memory_order_relaxed) != generation) {
                    return;
                }

                if (safeThis->takePeaksCachedPath_ != path) {
                    return;
                }

                safeThis->takeChannelPeaks_ = std::move(decoded.channels);
                safeThis->selectedTakeDurationSeconds_ = decoded.durationSeconds;
                safeThis->repaint();
            });
        }).detach();
    }

    repaint();
}

void SessionRecorderComponent::updateRecordingPeaks(radium::StreamingMixer& mixer) {
    if (!isRecording_ || isPunching_) return;

    auto waveW = std::max(100, getWaveformArea().getWidth());
    mixer.recording_channel_peaks(recChannelPeaks_, static_cast<std::size_t>(waveW));
    repaint();
}

void SessionRecorderComponent::updatePunchPreview(radium::StreamingMixer& mixer, double cueStartNorm,
                                                  std::pair<double, double> punchRegion, double playheadNorm,
                                                  double takeDurationSeconds) {
    if (!isRecording_ || !isPunching_ || takeDurationSeconds <= 0.0) {
        return;
    }

    const double actualStart = std::max(cueStartNorm, punchRegion.first);
    const double actualEnd = std::min(playheadNorm, punchRegion.second);
    if (actualEnd <= actualStart) {
        punchPreviewChannelPeaks_.clear();
        punchPreviewStartNorm_ = actualStart;
        punchPreviewEndNorm_ = actualStart;
        repaint();
        return;
    }

    const auto waveArea = getWaveformArea();
    const std::size_t bucketCount = static_cast<std::size_t>(
        std::max(1, static_cast<int>((actualEnd - actualStart) * waveArea.getWidth())));
    const double sampleRate = static_cast<double>(std::max(1, mixer.sample_rate()));

    const std::size_t startFrame = static_cast<std::size_t>(
        std::max(0.0, (actualStart - cueStartNorm) * takeDurationSeconds) * sampleRate);
    const std::size_t endFrame = static_cast<std::size_t>(
        std::max(0.0, (actualEnd - cueStartNorm) * takeDurationSeconds) * sampleRate);

    mixer.recording_channel_peaks_for_range(punchPreviewChannelPeaks_,
                                            startFrame,
                                            endFrame,
                                            bucketCount);
    punchPreviewStartNorm_ = actualStart;
    punchPreviewEndNorm_ = actualEnd;
    repaint();
}

void SessionRecorderComponent::clearRecordingPreview() {
    recChannelPeaks_.clear();
    punchPreviewChannelPeaks_.clear();
    punchPreviewStartNorm_ = 0.0;
    punchPreviewEndNorm_ = 0.0;
    repaint();
}

void SessionRecorderComponent::setRecordingState(bool recording, bool punching) {
    isRecording_ = recording;
    isPunching_ = punching;
    newButton_.setToggleState(recording && !punching, juce::dontSendNotification);
    punchButton_.setToggleState(recording && punching, juce::dontSendNotification);
}

int SessionRecorderComponent::getNumRows() {
    return static_cast<int>(takes_.size());
}

void SessionRecorderComponent::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || static_cast<std::size_t>(row) >= takes_.size()) return;

    if (selected) {
        g.setColour(colours::accentGreen.withAlpha(0.2f));
        g.fillRect(0, 0, w, h);
    }

    g.setColour(colours::textPrimary);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(takes_[static_cast<std::size_t>(row)].name),
               4, 0, w - 8, h, juce::Justification::centredLeft);
}

void SessionRecorderComponent::listBoxItemClicked(int row, const juce::MouseEvent& e) {
    if (row < 0) return;
    if (e.mods.isPopupMenu()) {
        juce::PopupMenu menu;
        menu.addItem(1, "Delete Take");
        menu.showMenuAsync(juce::PopupMenu::Options(),
            [this, row](int result) {
                if (result == 1 && onDeleteTake) {
                    onDeleteTake(static_cast<std::size_t>(row));
                }
            });
        return;
    }
    if (onTakeSelect) {
        onTakeSelect(static_cast<std::size_t>(row));
    }
}

void SessionRecorderComponent::setTakePlayhead(double normPos) {
    if (takePlayhead_ != normPos) {
        takePlayhead_ = normPos;
        isTakePlaying_ = normPos >= 0.0;
        repaint();
    }
}

std::optional<std::pair<double, double>> SessionRecorderComponent::punchInRegion() const {
    return punchInRegion_;
}

void SessionRecorderComponent::clearPunchInRegion() {
    punchInRegion_.reset();
    punchInTakeIndex_ = ~std::size_t(0);
    selectingPunchIn_ = false;
    punchPreviewChannelPeaks_.clear();
    punchPreviewStartNorm_ = 0.0;
    punchPreviewEndNorm_ = 0.0;
    repaint();
}

double SessionRecorderComponent::punchCuePosition() const {
    return cuePlayhead_;
}

void SessionRecorderComponent::setPunchCuePosition(double normPos) {
    cuePlayhead_ = std::clamp(normPos, 0.0, 1.0);
    repaint();
}

double SessionRecorderComponent::selectedTakeDurationSeconds() const {
    return selectedTakeDurationSeconds_;
}

juce::var SessionRecorderComponent::getDragSourceDescription(const juce::SparseSet<int>&) {
    return "session_take";
}

void SessionRecorderComponent::mouseDown(const juce::MouseEvent& e) {
    auto waveArea = getWaveformArea();
    if (!waveArea.contains(e.getPosition()) || !selectedIndex_.has_value()) {
        return;
    }

    if (e.mods.isRightButtonDown() && !isRecording_) {
        selectingPunchIn_ = true;
        punchDragStartNorm_ = xToWaveformNormalized(e.x);
        punchInRegion_ = std::make_pair(punchDragStartNorm_, punchDragStartNorm_);
        punchInTakeIndex_ = *selectedIndex_;
        repaint();
        return;
    }

    if (e.mods.isLeftButtonDown()) {
        cuePlayhead_ = xToWaveformNormalized(e.x);
        dragStarted_ = false;
        externalDragStarted_ = false;
        dragStartPos_ = e.getPosition();
        if (!isRecording_ && onTakeScrub) {
            onTakeScrub(*selectedIndex_, cuePlayhead_);
        }
        repaint();
    }
}

void SessionRecorderComponent::mouseDrag(const juce::MouseEvent& e) {
    if (!selectedIndex_.has_value()) return;

    if (selectingPunchIn_) {
        const double currentNorm = xToWaveformNormalized(e.x);
        const double start = std::min(punchDragStartNorm_, currentNorm);
        const double end = std::max(punchDragStartNorm_, currentNorm);
        punchInRegion_ = std::make_pair(start, end);
        punchInTakeIndex_ = *selectedIndex_;
        repaint();
        return;
    }

    if (isRecording_) return;

    auto waveArea = getWaveformArea();
    if (e.mods.isLeftButtonDown() && !externalDragStarted_ && onGetTakeFile &&
        e.getDistanceFromDragStart() > 8 && !waveArea.contains(e.getPosition())) {
        auto file = onGetTakeFile(*selectedIndex_);
        if (file.existsAsFile()) {
            externalDragStarted_ = true;
            dragStarted_ = true;
            juce::StringArray files;
            files.add(file.getFullPathName());
            performExternalDragDropOfFiles(files, false, this, nullptr);
            return;
        }
    }

    if (externalDragStarted_) {
        return;
    }

    if (e.mods.isLeftButtonDown() && waveArea.contains(e.getPosition())) {
        cuePlayhead_ = xToWaveformNormalized(e.x);
        if (onTakeScrub) {
            onTakeScrub(*selectedIndex_, cuePlayhead_);
        }
        repaint();
    }
}

void SessionRecorderComponent::mouseUp(const juce::MouseEvent&) {
    externalDragStarted_ = false;
    dragStarted_ = false;
    if (!selectingPunchIn_) {
        return;
    }

    selectingPunchIn_ = false;
    if (!punchInRegion_.has_value()) {
        return;
    }

    const auto [start, end] = *punchInRegion_;
    if (end - start < 0.005) {
        punchInRegion_.reset();
        punchInTakeIndex_ = ~std::size_t(0);
    }
    repaint();
}

}  // namespace triggerfish

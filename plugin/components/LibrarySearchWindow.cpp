#include "LibrarySearchWindow.h"
#include <cmath>

namespace triggerfish {

void LibraryResultListBox::mouseWheelMove(const juce::MouseEvent& event,
                                          const juce::MouseWheelDetails& wheel) {
    if (onSelectionWheel && std::abs(wheel.deltaY) > 0.0f) {
        onSelectionWheel(wheel.deltaY < 0.0f ? 1 : -1);
        return;
    }
    juce::ListBox::mouseWheelMove(event, wheel);
}

bool LibraryResultListBox::keyPressed(const juce::KeyPress& key) {
    if (onSpecialKeyPress && onSpecialKeyPress(key)) {
        return true;
    }
    return juce::ListBox::keyPressed(key);
}

LibrarySearchContent::LibrarySearchContent() {
    addAndMakeVisible(titleLabel_);
    titleLabel_.setFont(juce::FontOptions(14.0f));
    titleLabel_.setColour(juce::Label::textColourId, colours::textPrimary);

    addAndMakeVisible(libraryLabel_);
    libraryLabel_.setColour(juce::Label::textColourId, colours::textDim);
    libraryLabel_.setFont(juce::FontOptions(11.0f));

    addAndMakeVisible(librarySelector_);
    librarySelector_.onChange = [this] {
        currentPage_ = 0;
        runSearch();
    };

    addAndMakeVisible(queryLabel_);
    queryLabel_.setColour(juce::Label::textColourId, colours::textDim);
    queryLabel_.setFont(juce::FontOptions(11.0f));

    addAndMakeVisible(queryEditor_);
    queryEditor_.setTextToShowWhenEmpty("Type keywords and press Enter", colours::textDim);
    queryEditor_.onReturnKey = [this] {
        currentPage_ = 0;
        runSearch();
        resultsList_.grabKeyboardFocus();
    };

    addAndMakeVisible(searchButton_);
    searchButton_.onClick = [this] {
        currentPage_ = 0;
        runSearch();
        resultsList_.grabKeyboardFocus();
    };

    addAndMakeVisible(sortLabel_);
    sortLabel_.setColour(juce::Label::textColourId, colours::textDim);
    sortLabel_.setFont(juce::FontOptions(11.0f));

    addAndMakeVisible(sortButton_);
    sortButton_.onClick = [this] { showSortMenu(); };
    updateSortButtonText();

    addAndMakeVisible(monoOnlyButton_);
    monoOnlyButton_.onClick = [this] {
        if (monoOnlyButton_.getToggleState()) {
            stereoOnlyButton_.setToggleState(false, juce::dontSendNotification);
        }
        currentPage_ = 0;
        runSearch();
    };

    addAndMakeVisible(stereoOnlyButton_);
    stereoOnlyButton_.onClick = [this] {
        if (stereoOnlyButton_.getToggleState()) {
            monoOnlyButton_.setToggleState(false, juce::dontSendNotification);
        }
        currentPage_ = 0;
        runSearch();
    };

    addAndMakeVisible(statusLabel_);
    statusLabel_.setColour(juce::Label::textColourId, colours::textDim);
    statusLabel_.setFont(juce::FontOptions(12.0f));
    statusLabel_.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(previousPageButton_);
    previousPageButton_.onClick = [this] {
        if (currentPage_ > 0) {
            --currentPage_;
            runSearch();
        }
    };

    addAndMakeVisible(pageLabel_);
    pageLabel_.setColour(juce::Label::textColourId, colours::textDim);
    pageLabel_.setFont(juce::FontOptions(11.0f));
    pageLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(nextPageButton_);
    nextPageButton_.onClick = [this] {
        const int pageCount = std::max(1, (totalMatches_ + kResultsPerPage - 1) / kResultsPerPage);
        if (currentPage_ + 1 < pageCount) {
            ++currentPage_;
            runSearch();
        }
    };

    addAndMakeVisible(resultsList_);
    resultsList_.setModel(this);
    resultsList_.setRowHeight(46);
    resultsList_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    resultsList_.onSelectionWheel = [this](int delta) { moveSelectionBy(delta); };
    resultsList_.onSpecialKeyPress = [this](const juce::KeyPress& key) {
        if (key == juce::KeyPress::spaceKey && !isEditingQuery() && onFocusTrackTransportToggle) {
            commitPendingSelection();
            onFocusTrackTransportToggle();
            return true;
        }
        return false;
    };

    reloadLibraries();
}

void LibrarySearchContent::paint(juce::Graphics& g) {
    g.fillAll(colours::background);
}

void LibrarySearchContent::resized() {
    auto area = getLocalBounds().reduced(10);

    titleLabel_.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);

    auto topRow = area.removeFromTop(24);
    libraryLabel_.setBounds(topRow.removeFromLeft(60));
    librarySelector_.setBounds(topRow.removeFromLeft(260));
    topRow.removeFromLeft(12);
    queryLabel_.setBounds(topRow.removeFromLeft(60));
    searchButton_.setBounds(topRow.removeFromRight(84));
    topRow.removeFromRight(8);
    queryEditor_.setBounds(topRow);

    area.removeFromTop(8);
    auto filterRow = area.removeFromTop(22);
    monoOnlyButton_.setBounds(filterRow.removeFromLeft(110));
    filterRow.removeFromLeft(8);
    stereoOnlyButton_.setBounds(filterRow.removeFromLeft(118));
    filterRow.removeFromLeft(16);
    sortLabel_.setBounds(filterRow.removeFromLeft(48));
    filterRow.removeFromLeft(8);
    sortButton_.setBounds(filterRow.removeFromLeft(130));
    area.removeFromTop(8);
    auto statusRow = area.removeFromTop(24);
    statusLabel_.setBounds(statusRow.removeFromLeft(std::max(160, statusRow.getWidth() - 172)));
    previousPageButton_.setBounds(statusRow.removeFromLeft(50));
    statusRow.removeFromLeft(6);
    pageLabel_.setBounds(statusRow.removeFromLeft(60));
    statusRow.removeFromLeft(6);
    nextPageButton_.setBounds(statusRow.removeFromLeft(50));
    area.removeFromTop(6);
    resultsList_.setBounds(area);
}

int LibrarySearchContent::getNumRows() {
    return static_cast<int>(displayRows_.size());
}

void LibrarySearchContent::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    if (row < 0 || row >= static_cast<int>(displayRows_.size())) {
        return;
    }

    const auto& displayRow = displayRows_[static_cast<std::size_t>(row)];
    const auto bounds = juce::Rectangle<int>(0, 0, width, height);

    if (displayRow.kind == DisplayRow::Kind::Separator) {
        g.setColour(colours::background);
        g.fillRect(bounds);
        g.setColour(colours::border);
        g.drawLine(10.0f, static_cast<float>(height / 2), static_cast<float>(width - 10), static_cast<float>(height / 2));
        g.setColour(colours::textDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(displayRow.label, bounds.reduced(16, 0), juce::Justification::centredLeft, true);
        return;
    }

    if (!displayRow.resultIndex.has_value() || *displayRow.resultIndex >= results_.size()) {
        return;
    }

    const auto& result = results_[*displayRow.resultIndex];

    g.setColour(selected ? colours::buttonHover : colours::panel);
    g.fillRect(bounds);

    g.setColour(colours::border);
    g.drawLine(0.0f, static_cast<float>(height - 1), static_cast<float>(width), static_cast<float>(height - 1));

    auto line1 = bounds.reduced(10, 6).removeFromTop(18);
    g.setColour(selected ? colours::textAccent : colours::textPrimary);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(result.filename, line1, juce::Justification::centredLeft, true);

    auto metaLine = bounds.reduced(10, 6);
    metaLine.removeFromTop(18);
    g.setColour(colours::textDim);
    g.setFont(juce::FontOptions(11.0f));

    juce::String subtitle = result.folder;
    if (result.metadataSummary.isNotEmpty()) {
        subtitle << "  |  " << result.metadataSummary;
    }

    g.drawText(subtitle, metaLine, juce::Justification::centredLeft, true);

    const juce::String duration = formatDuration(result.durationSeconds);
    g.drawText(duration, bounds.reduced(10, 6).removeFromRight(72), juce::Justification::centredRight, true);
}

void LibrarySearchContent::selectedRowsChanged(int lastRowSelected) {
    if (suppressSelectionApply_ || lastRowSelected < 0 || lastRowSelected >= static_cast<int>(displayRows_.size())) {
        return;
    }

    const auto& displayRow = displayRows_[static_cast<std::size_t>(lastRowSelected)];
    if (displayRow.kind != DisplayRow::Kind::Result || !displayRow.resultIndex.has_value()) {
        return;
    }

    queueSelectionApply(lastRowSelected);
}

void LibrarySearchContent::listBoxItemClicked(int row, const juce::MouseEvent&) {
    resultsList_.selectRow(row);
}

void LibrarySearchContent::reloadLibraries(const juce::String& preferredLibraryId) {
    libraries_ = LibraryDatabase::availableLibraries();

    librarySelector_.clear(juce::dontSendNotification);
    int selectedId = 0;
    for (int i = 0; i < libraries_.size(); ++i) {
        const auto displayName = libraries_.getReference(i).name + " (" + juce::String(libraries_.getReference(i).fileCount) + ")";
        const int itemId = i + 1;
        librarySelector_.addItem(displayName, itemId);
        if (preferredLibraryId.isNotEmpty() && libraries_.getReference(i).id == preferredLibraryId) {
            selectedId = itemId;
        }
    }

    if (selectedId == 0 && libraries_.size() > 0) {
        selectedId = 1;
    }

    if (selectedId > 0) {
        librarySelector_.setSelectedId(selectedId, juce::dontSendNotification);
        currentPage_ = 0;
        runSearch();
    } else {
        statusLabel_.setText("No databases yet. Use Database.", juce::dontSendNotification);
        results_.clear();
        resultsList_.updateContent();
        totalMatches_ = 0;
        updatePagingControls();
    }
}

void LibrarySearchContent::focusSearchBox() {
    queryEditor_.grabKeyboardFocus();
}

bool LibrarySearchContent::isEditingQuery() const {
    return queryEditor_.hasKeyboardFocus(true);
}

void LibrarySearchContent::runSearch() {
    stopTimer();
    pendingSelectionRow_ = -1;
    results_.clear();
    displayRows_.clear();
    resultsList_.updateContent();

    const int selected = librarySelector_.getSelectedItemIndex();
    if (selected < 0 || selected >= libraries_.size()) {
        statusLabel_.setText("Choose a database first.", juce::dontSendNotification);
        totalMatches_ = 0;
        updatePagingControls();
        return;
    }

    juce::String error;
    results_ = LibraryDatabase::search(
        libraries_.getReference(selected).id,
        queryEditor_.getText(),
        error,
        kResultsPerPage,
        currentPage_ * kResultsPerPage,
        &totalMatches_,
        monoOnlyButton_.getToggleState(),
        stereoOnlyButton_.getToggleState(),
        sortModeId_ == 2 ? LibraryDatabase::SearchSortMode::FileLength
                         : LibraryDatabase::SearchSortMode::Name,
        sortDescending_);
    if (error.isNotEmpty()) {
        statusLabel_.setText(error, juce::dontSendNotification);
        totalMatches_ = 0;
        updatePagingControls();
        return;
    }

    const int startIndex = totalMatches_ == 0 ? 0 : (currentPage_ * kResultsPerPage) + 1;
    const int endIndex = currentPage_ * kResultsPerPage + static_cast<int>(results_.size());
    statusLabel_.setText(
        totalMatches_ == 0
            ? "0 results"
            : "Showing " + juce::String(startIndex) + "-" + juce::String(endIndex)
                  + " of " + juce::String(totalMatches_) + " result"
                  + (totalMatches_ == 1 ? "" : "s"),
        juce::dontSendNotification);
    suppressSelectionApply_ = true;
    rebuildDisplayRows();
    resultsList_.updateContent();
    resultsList_.deselectAllRows();
    suppressSelectionApply_ = false;
    lastAppliedPath_.clear();
    updatePagingControls();
    repaint();
}

void LibrarySearchContent::moveSelectionBy(int delta) {
    if (displayRows_.empty()) {
        return;
    }

    int row = resultsList_.getSelectedRow();
    if (row < 0) {
        row = delta > 0 ? firstSelectableRow() : lastSelectableRow();
    } else {
        row = nextSelectableRow(row, delta);
    }

    if (row < 0) {
        return;
    }

    resultsList_.selectRow(row);
    resultsList_.scrollToEnsureRowIsOnscreen(row);
}

void LibrarySearchContent::queueSelectionApply(int row) {
    pendingSelectionRow_ = row;
    startTimer(70);
}

void LibrarySearchContent::commitPendingSelection() {
    stopTimer();
    if (pendingSelectionRow_ < 0 || pendingSelectionRow_ >= static_cast<int>(displayRows_.size())) {
        pendingSelectionRow_ = -1;
        return;
    }

    const auto& displayRow = displayRows_[static_cast<std::size_t>(pendingSelectionRow_)];
    pendingSelectionRow_ = -1;
    if (displayRow.kind != DisplayRow::Kind::Result || !displayRow.resultIndex.has_value() ||
        *displayRow.resultIndex >= results_.size()) {
        return;
    }

    const auto& path = results_[*displayRow.resultIndex].path;
    if (path.isEmpty() || path == lastAppliedPath_) {
        return;
    }

    lastAppliedPath_ = path;
    if (onResultChosen) {
        onResultChosen(path);
    }
}

void LibrarySearchContent::timerCallback() {
    commitPendingSelection();
}

void LibrarySearchContent::rebuildDisplayRows() {
    displayRows_.clear();

    std::vector<std::size_t> monoRows;
    std::vector<std::size_t> stereoRows;
    monoRows.reserve(results_.size());
    stereoRows.reserve(results_.size());

    for (std::size_t i = 0; i < results_.size(); ++i) {
        if (results_[i].channels <= 1) {
            monoRows.push_back(i);
        } else {
            stereoRows.push_back(i);
        }
    }

    const bool monoOnly = monoOnlyButton_.getToggleState();
    const bool stereoOnly = stereoOnlyButton_.getToggleState();

    if (monoOnly) {
        for (const auto index : monoRows) {
            displayRows_.push_back(DisplayRow{DisplayRow::Kind::Result, index, {}});
        }
        return;
    }

    if (stereoOnly) {
        for (const auto index : stereoRows) {
            displayRows_.push_back(DisplayRow{DisplayRow::Kind::Result, index, {}});
        }
        return;
    }

    for (const auto index : monoRows) {
        displayRows_.push_back(DisplayRow{DisplayRow::Kind::Result, index, {}});
    }

    if (!monoRows.empty() && !stereoRows.empty()) {
        displayRows_.push_back(DisplayRow{DisplayRow::Kind::Separator, std::nullopt, "Stereo"});
    }

    for (const auto index : stereoRows) {
        displayRows_.push_back(DisplayRow{DisplayRow::Kind::Result, index, {}});
    }
}

void LibrarySearchContent::showSortMenu() {
    juce::PopupMenu menu;
    menu.addItem(1, "Name");
    menu.addItem(2, "File Length");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sortButton_),
                       juce::ModalCallbackFunction::create([this](int selectedId) {
        if (selectedId == 0) {
            return;
        }

        if (sortModeId_ == selectedId) {
            sortDescending_ = !sortDescending_;
        } else {
            sortModeId_ = selectedId;
            sortDescending_ = false;
        }

        updateSortButtonText();
        currentPage_ = 0;
        runSearch();
    }));
}

void LibrarySearchContent::updateSortButtonText() {
    const juce::String mode = (sortModeId_ == 2) ? "File Length" : "Name";
    const juce::String arrow = sortDescending_ ? " v" : " ^";
    sortButton_.setButtonText(mode + arrow);
}

void LibrarySearchContent::updatePagingControls() {
    const int pageCount = std::max(1, (totalMatches_ + kResultsPerPage - 1) / kResultsPerPage);
    currentPage_ = std::clamp(currentPage_, 0, pageCount - 1);
    previousPageButton_.setEnabled(currentPage_ > 0);
    nextPageButton_.setEnabled(currentPage_ + 1 < pageCount);
    pageLabel_.setText("Page " + juce::String(currentPage_ + 1) + "/" + juce::String(pageCount),
                       juce::dontSendNotification);
}

int LibrarySearchContent::firstSelectableRow() const {
    return nextSelectableRow(-1, 1);
}

int LibrarySearchContent::lastSelectableRow() const {
    return nextSelectableRow(static_cast<int>(displayRows_.size()), -1);
}

int LibrarySearchContent::nextSelectableRow(int startRow, int delta) const {
    if (displayRows_.empty()) {
        return -1;
    }

    int row = startRow;
    while (true) {
        row += delta;
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) {
            return -1;
        }

        const auto& displayRow = displayRows_[static_cast<std::size_t>(row)];
        if (displayRow.kind == DisplayRow::Kind::Result && displayRow.resultIndex.has_value()) {
            return row;
        }
    }
}

juce::String LibrarySearchContent::formatDuration(double seconds) const {
    const auto totalSeconds = juce::roundToInt(seconds);
    const int mins = totalSeconds / 60;
    const int secs = totalSeconds % 60;
    return juce::String(mins) + ":" + juce::String(secs).paddedLeft('0', 2);
}

LibrarySearchWindow::LibrarySearchWindow()
    : juce::DocumentWindow("Triggerfish Search",
                           colours::background,
                           juce::DocumentWindow::closeButton) {
    setUsingNativeTitleBar(true);
    setContentOwned(&content_, false);
    setResizable(true, true);
    setResizeLimits(560, 360, 1400, 1000);
    centreWithSize(860, 620);
    setVisible(false);
}

LibrarySearchWindow::~LibrarySearchWindow() = default;

LibrarySearchContent& LibrarySearchWindow::content() {
    return content_;
}

const LibrarySearchContent& LibrarySearchWindow::content() const {
    return content_;
}

void LibrarySearchWindow::closeButtonPressed() {
    setVisible(false);
}

}  // namespace triggerfish

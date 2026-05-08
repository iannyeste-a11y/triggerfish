#pragma once

#include "../LibraryDatabase.h"
#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <vector>

namespace triggerfish {

class LibraryResultListBox : public juce::ListBox {
public:
    using juce::ListBox::ListBox;

    std::function<void(int delta)> onSelectionWheel;
    std::function<bool(const juce::KeyPress&)> onSpecialKeyPress;

    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
};

class LibrarySearchContent : public juce::Component,
                             public juce::ListBoxModel,
                             private juce::Timer {
public:
    LibrarySearchContent();

    void paint(juce::Graphics&) override;
    void resized() override;

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void reloadLibraries(const juce::String& preferredLibraryId = {});
    void focusSearchBox();
    bool isEditingQuery() const;

    std::function<void(const juce::String& filePath)> onResultChosen;
    std::function<void()> onFocusTrackTransportToggle;

private:
    struct DisplayRow {
        enum class Kind { Result, Separator };
        Kind kind = Kind::Result;
        std::optional<std::size_t> resultIndex;
        juce::String label;
    };

    void timerCallback() override;
    void runSearch();
    void moveSelectionBy(int delta);
    void queueSelectionApply(int row);
    void commitPendingSelection();
    void rebuildDisplayRows();
    void showSortMenu();
    void updateSortButtonText();
    void updatePagingControls();
    int firstSelectableRow() const;
    int lastSelectableRow() const;
    int nextSelectableRow(int startRow, int delta) const;
    juce::String formatDuration(double seconds) const;

    juce::Label titleLabel_{"", "LIBRARY SEARCH"};
    juce::Label libraryLabel_{"", "Database"};
    juce::ComboBox librarySelector_;
    juce::Label queryLabel_{"", "Keywords"};
    juce::TextEditor queryEditor_;
    juce::TextButton searchButton_{"Search"};
    juce::Label sortLabel_{"", "Sort By"};
    juce::TextButton sortButton_{"Name"};
    juce::ToggleButton monoOnlyButton_{"Mono Only"};
    juce::ToggleButton stereoOnlyButton_{"Stereo Only"};
    juce::Label statusLabel_{"", "No databases yet. Use Database."};
    juce::TextButton previousPageButton_{"Prev"};
    juce::Label pageLabel_{"", "Page 1/1"};
    juce::TextButton nextPageButton_{"Next"};
    LibraryResultListBox resultsList_{"Results", this};

    juce::Array<LibraryDescriptor> libraries_;
    std::vector<LibrarySearchResult> results_;
    std::vector<DisplayRow> displayRows_;
    bool suppressSelectionApply_ = false;
    int pendingSelectionRow_ = -1;
    juce::String lastAppliedPath_;
    int sortModeId_ = 1;
    bool sortDescending_ = false;
    int currentPage_ = 0;
    int totalMatches_ = 0;
    static constexpr int kResultsPerPage = 500;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibrarySearchContent)
};

class LibrarySearchWindow : public juce::DocumentWindow {
public:
    LibrarySearchWindow();
    ~LibrarySearchWindow() override;

    LibrarySearchContent& content();
    const LibrarySearchContent& content() const;

    void closeButtonPressed() override;

private:
    LibrarySearchContent content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibrarySearchWindow)
};

}  // namespace triggerfish

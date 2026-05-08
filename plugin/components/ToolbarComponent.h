#pragma once

#include "../LookAndFeel_Radium.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace triggerfish {

class ToolbarComponent : public juce::Component {
public:
    ToolbarComponent();

    void resized() override;
    void paint(juce::Graphics&) override;

    std::function<void()> onOpen;
    std::function<void()> onAddAudio;
    std::function<void()> onSave;
    std::function<void()> onSaveWithAudio;
    std::function<void()> onNew;
    std::function<void()> onPicture;
    std::function<void()> onSearch;
    std::function<void()> onDatabaseMenu;
    std::function<void(float)> onMasterGainChange;

    void setProjectName(const juce::String& name);
    void setMasterGain(float gain);
    void setMidiStatusVisible(bool visible);
    void setMidiStatus(bool active);

private:
    juce::TextButton openButton{"Open"};
    juce::TextButton addAudioButton{"Add Audio"};
    juce::TextButton saveButton{"Save \xe2\x96\xbe"};   // ▾ hints at the dropdown menu
    juce::TextButton newButton{"New"};
    juce::TextButton databaseButton{"Database"};
    juce::TextButton pictureButton{"Picture"};
    juce::TextButton searchButton{"Search"};
    juce::Label masterLabel_{"", "Master"};
    FineTuneSlider masterGainSlider_;
    juce::Label projectLabel;
    juce::String midiStatusText_{"MIDI"};
    bool midiStatusVisible_ = false;
    bool midiActive_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToolbarComponent)
};

}  // namespace triggerfish

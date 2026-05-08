#pragma once

#include "../LookAndFeel_Radium.h"
#include "../JuceVst3Host.h"
#include "../Vst3PluginScanner.h"
#include "app_controller.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>

namespace triggerfish {

class Vst3InsertRack : public juce::Component {
public:
    static constexpr int kSlotCount = 5;
    using HostArray = std::array<std::unique_ptr<JuceVst3Host>, kSlotCount>;
    using PluginState = radium::AppController::LayerOverride::Vst3PluginState;
    using PluginStateArray = std::array<std::optional<PluginState>, kSlotCount>;

    explicit Vst3InsertRack(Vst3PluginScanner& scanner);
    ~Vst3InsertRack() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Set which layer's hosts to display. Hosts are owned externally.
    void setHosts(HostArray* hosts, double sampleRate, int blockSize);

    // Update the UI labels/bypass state from controller data.
    void updateDisplay(radium::AppController& controller, std::size_t layerIndex);
    void updateDisplay(const PluginStateArray& plugins);

    void clearDisplay();
    void setLayoutMetrics(int slotHeight, int slotSpacing);
    void setAccentColour(juce::Colour accent);

    // Called when a plugin is loaded/unloaded so the caller can update AppController state.
    std::function<void(std::size_t slotIndex, const juce::String& filePath,
                       const juce::String& classId, const juce::String& name)> onPluginLoaded;
    std::function<void(std::size_t slotIndex)> onPluginRemoved;
    std::function<void(std::size_t slotIndex)> onBypassToggle;

    // Access the host session for the current layer's slot.
    JuceVst3Host* getHostSession(std::size_t slot) const;

private:
    struct SlotUI {
        juce::TextButton loadButton{"Empty"};
        juce::TextButton bypassButton{"B"};
        juce::TextButton editorButton{"E"};
        juce::TextButton removeButton{"X"};
        std::unique_ptr<juce::DocumentWindow> editorWindow;
    };

    std::array<SlotUI, kSlotCount> slots_;
    HostArray* currentHosts_ = nullptr;
    Vst3PluginScanner& scanner_;
    double sampleRate_ = 44100.0;
    int blockSize_ = 512;
    int slotHeight_ = 28;
    int slotSpacing_ = 2;
    juce::Colour accentColour_ = colours::accentFocus;

    void showPluginMenu(std::size_t slotIndex);
    void loadPluginIntoSlot(std::size_t slotIndex, const juce::String& filePath,
                            const juce::String& classId, const juce::String& pluginName);
    void removePluginFromSlot(std::size_t slotIndex);
    void openPluginEditor(std::size_t slotIndex);
    void closePluginEditor(std::size_t slotIndex);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Vst3InsertRack)
};

}  // namespace triggerfish

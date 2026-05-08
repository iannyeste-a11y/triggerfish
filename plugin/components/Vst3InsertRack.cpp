#include "Vst3InsertRack.h"
#include "../LookAndFeel_Radium.h"
#include <algorithm>

namespace triggerfish {

Vst3InsertRack::Vst3InsertRack(Vst3PluginScanner& scanner)
    : scanner_(scanner) {
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        auto& slot = slots_[i];

        slot.loadButton.onClick = [this, i] { showPluginMenu(i); };
        slot.removeButton.onClick = [this, i] { removePluginFromSlot(i); };
        slot.bypassButton.onClick = [this, i] {
            if (onBypassToggle) onBypassToggle(i);
        };
        slot.editorButton.onClick = [this, i] { openPluginEditor(i); };

        slot.bypassButton.setClickingTogglesState(true);

        addAndMakeVisible(slot.loadButton);
        addAndMakeVisible(slot.bypassButton);
        addAndMakeVisible(slot.editorButton);
        addAndMakeVisible(slot.removeButton);
    }
}

Vst3InsertRack::~Vst3InsertRack() {
    for (std::size_t i = 0; i < kSlotCount; ++i)
        closePluginEditor(i);
}

void Vst3InsertRack::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::panel);
    g.fillRoundedRectangle(bounds, 4.0f);
    g.setColour(accentColour_.withAlpha(0.42f));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 2.0f);
}

void Vst3InsertRack::resized() {
    auto area = getLocalBounds().reduced(4, 2);
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        auto& slot = slots_[i];
        auto row = area.removeFromTop(slotHeight_);
        area.removeFromTop(slotSpacing_);

        slot.removeButton.setBounds(row.removeFromRight(24));
        row.removeFromRight(2);
        slot.editorButton.setBounds(row.removeFromRight(24));
        row.removeFromRight(2);
        slot.bypassButton.setBounds(row.removeFromRight(24));
        row.removeFromRight(4);
        slot.loadButton.setBounds(row);
    }
}

void Vst3InsertRack::setHosts(HostArray* hosts, double sampleRate, int blockSize) {
    // Only close editor windows when actually switching to different hosts
    if (hosts != currentHosts_) {
        for (std::size_t i = 0; i < kSlotCount; ++i)
            closePluginEditor(i);
    }

    currentHosts_ = hosts;
    sampleRate_ = sampleRate;
    blockSize_ = blockSize;
}

void Vst3InsertRack::setLayoutMetrics(int slotHeight, int slotSpacing) {
    slotHeight_ = std::max(12, slotHeight);
    slotSpacing_ = std::max(0, slotSpacing);
    resized();
}

void Vst3InsertRack::setAccentColour(juce::Colour accent) {
    accentColour_ = accent;
    for (auto& slot : slots_) {
        slot.bypassButton.setColour(juce::TextButton::buttonOnColourId, accentColour_);
    }
    repaint();
}

void Vst3InsertRack::updateDisplay(radium::AppController& controller,
                                    std::size_t layerIndex) {
    updateDisplay(controller.layer_vst3_plugins(layerIndex));
}

void Vst3InsertRack::updateDisplay(const PluginStateArray& plugins) {
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        auto& slot = slots_[i];
        auto* host = getHostSession(i);

        if (host && host->is_configured()) {
            slot.loadButton.setButtonText(host->getPluginName());
            slot.bypassButton.setToggleState(
                plugins[i].has_value() && plugins[i]->bypassed,
                juce::dontSendNotification);
        } else if (plugins[i].has_value()) {
            const auto fallbackName = plugins[i]->display_name.empty()
                ? juce::String("Plugin")
                : juce::String(plugins[i]->display_name);
            slot.loadButton.setButtonText(fallbackName);
            slot.bypassButton.setToggleState(
                plugins[i]->bypassed,
                juce::dontSendNotification);
        } else {
            slot.loadButton.setButtonText("Empty");
            slot.bypassButton.setToggleState(false, juce::dontSendNotification);
        }
    }
}

void Vst3InsertRack::clearDisplay() {
    for (std::size_t i = 0; i < kSlotCount; ++i) {
        closePluginEditor(i);
        slots_[i].loadButton.setButtonText("Empty");
        slots_[i].bypassButton.setToggleState(false, juce::dontSendNotification);
    }
    currentHosts_ = nullptr;
}

JuceVst3Host* Vst3InsertRack::getHostSession(std::size_t slot) const {
    if (currentHosts_ && slot < kSlotCount)
        return (*currentHosts_)[slot].get();
    return nullptr;
}

void Vst3InsertRack::showPluginMenu(std::size_t slotIndex) {
    juce::PopupMenu menu;

    menu.addItem(1, "Set VST3 Folder...");
    if (scanner_.hasScanFolder()) {
        menu.addItem(2, "Rescan (" + scanner_.getScanFolder().getFileName() + ")");
    }
    menu.addSeparator();

    if (!scanner_.hasScanFolder()) {
        menu.addItem(0, "No VST3 folder configured", false, false);
    } else if (scanner_.scannedFiles().empty()) {
        menu.addItem(0, "No plugins found", false, false);
    } else {
        const auto& files = scanner_.scannedFiles();
        for (std::size_t fi = 0; fi < files.size(); ++fi) {
            const auto& sf = files[fi];
            if (sf.probed && sf.plugins.size() > 1) {
                juce::PopupMenu subMenu;
                for (std::size_t pi = 0; pi < sf.plugins.size(); ++pi) {
                    int id = 10000 + static_cast<int>(fi) * 1000 + static_cast<int>(pi);
                    subMenu.addItem(id, sf.plugins[pi].name);
                }
                menu.addSubMenu(sf.fileName, subMenu);
            } else if (sf.probed && sf.plugins.size() == 1) {
                int id = 10000 + static_cast<int>(fi) * 1000;
                menu.addItem(id, sf.plugins[0].name);
            } else {
                int id = 100 + static_cast<int>(fi);
                menu.addItem(id, sf.fileName);
            }
        }
    }

    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetComponent(slots_[slotIndex].loadButton),
        [this, slotIndex](int result) {
            if (result == 0) return;

            if (result == 1) {
                auto chooser = std::make_shared<juce::FileChooser>(
                    "Select VST3 Plugin Folder", scanner_.getScanFolder());
                chooser->launchAsync(juce::FileBrowserComponent::openMode |
                                         juce::FileBrowserComponent::canSelectDirectories,
                    [this, chooser](const juce::FileChooser& fc) {
                        auto folder = fc.getResult();
                        if (folder.isDirectory()) {
                            scanner_.setScanFolder(folder);
                        }
                    });
                return;
            }

            if (result == 2) {
                scanner_.rescan();
                return;
            }

            if (result >= 10000) {
                int encoded = result - 10000;
                int fileIdx = encoded / 1000;
                int pluginIdx = encoded % 1000;

                const auto& files = scanner_.scannedFiles();
                if (fileIdx >= 0 && fileIdx < static_cast<int>(files.size())) {
                    const auto& sf = files[static_cast<std::size_t>(fileIdx)];
                    if (pluginIdx >= 0 && pluginIdx < static_cast<int>(sf.plugins.size())) {
                        const auto& pe = sf.plugins[static_cast<std::size_t>(pluginIdx)];
                        loadPluginIntoSlot(slotIndex, pe.filePath, pe.classId, pe.name);
                    }
                }
                return;
            }

            if (result >= 100) {
                auto fileIdx = static_cast<std::size_t>(result - 100);
                const auto& plugins = scanner_.probeFile(fileIdx);

                if (plugins.empty()) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::MessageBoxIconType::WarningIcon,
                        "Plugin Scan", "No plugins found in this file.");
                    return;
                }

                if (plugins.size() == 1) {
                    loadPluginIntoSlot(slotIndex, plugins[0].filePath, plugins[0].classId, plugins[0].name);
                    return;
                }

                juce::PopupMenu subMenu;
                for (std::size_t pi = 0; pi < plugins.size(); ++pi) {
                    subMenu.addItem(static_cast<int>(pi + 1), plugins[pi].name);
                }
                subMenu.showMenuAsync(juce::PopupMenu::Options()
                                          .withTargetComponent(slots_[slotIndex].loadButton),
                    [this, slotIndex, fileIdx](int subResult) {
                        if (subResult <= 0) return;
                        const auto& files = scanner_.scannedFiles();
                        if (fileIdx < files.size()) {
                            auto pi = static_cast<std::size_t>(subResult - 1);
                            const auto& sf = files[fileIdx];
                            if (pi < sf.plugins.size()) {
                                loadPluginIntoSlot(slotIndex, sf.plugins[pi].filePath,
                                                   sf.plugins[pi].classId, sf.plugins[pi].name);
                            }
                        }
                    });
            }
        });
}

void Vst3InsertRack::loadPluginIntoSlot(std::size_t slotIndex,
                                         const juce::String& filePath,
                                         const juce::String& classId,
                                         const juce::String& pluginName) {
    if (!currentHosts_) return;

    auto& hostPtr = (*currentHosts_)[slotIndex];
    if (!hostPtr)
        hostPtr = std::make_unique<JuceVst3Host>();

    juce::String err;
    if (hostPtr->loadPlugin(filePath, classId, sampleRate_, blockSize_, err, pluginName)) {
        auto name = hostPtr->getPluginName();
        slots_[slotIndex].loadButton.setButtonText(name);
        if (onPluginLoaded) {
            onPluginLoaded(slotIndex, filePath, classId, name);
        }
    } else {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Plugin Load Error", err);
    }
}

void Vst3InsertRack::removePluginFromSlot(std::size_t slotIndex) {
    closePluginEditor(slotIndex);
    if (currentHosts_) {
        auto& hostPtr = (*currentHosts_)[slotIndex];
        if (hostPtr) hostPtr->unloadPlugin();
    }
    slots_[slotIndex].loadButton.setButtonText("Empty");
    if (onPluginRemoved) onPluginRemoved(slotIndex);
}

void Vst3InsertRack::openPluginEditor(std::size_t slotIndex) {
    auto* host = getHostSession(slotIndex);
    if (!host || !host->is_configured()) return;

    auto& slot = slots_[slotIndex];
    if (slot.editorWindow != nullptr) {
        closePluginEditor(slotIndex);
        return;
    }

    auto* instance = host->getPluginInstance();
    if (auto* editor = instance->createEditorIfNeeded()) {
        auto* window = new juce::DocumentWindow(
            host->getPluginName(),
            juce::Colours::darkgrey,
            juce::DocumentWindow::closeButton);
        window->setContentNonOwned(editor, true);
        window->setResizable(true, false);
        window->centreWithSize(editor->getWidth(), editor->getHeight());
        window->setVisible(true);
        slot.editorWindow.reset(window);
    }
}

void Vst3InsertRack::closePluginEditor(std::size_t slotIndex) {
    if (slotIndex < kSlotCount) {
        slots_[slotIndex].editorWindow.reset();
    }
}

}  // namespace triggerfish

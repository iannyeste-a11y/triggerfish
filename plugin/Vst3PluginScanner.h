#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include <map>
#include <vector>

namespace triggerfish {

// Lists .vst3 files from a user-configured folder and lazily probes them
// on demand to discover sub-plugins (e.g. Waves shells).
// The initial scan is filesystem-only — no plugin loading, no auth dialogs.
class Vst3PluginScanner {
public:
    struct PluginEntry {
        juce::String name;         // display name
        juce::String filePath;     // full path to .vst3
        juce::String classId;      // uniqueId as string
        juce::String manufacturer;
    };

    // One .vst3 file found on disk.
    struct ScannedFile {
        juce::String filePath;     // full path to .vst3
        juce::String fileName;     // stem (e.g. "WaveShell1-VST3 14.0")
        bool probed = false;       // true once findAllTypesForFile has been called
        std::vector<PluginEntry> plugins; // populated after probing
    };

    Vst3PluginScanner();

    juce::File getScanFolder() const;
    void setScanFolder(const juce::File& folder);
    bool hasScanFolder() const;

    // Filesystem-only scan — lists .vst3 files without loading any modules.
    void rescan();

    // Probe a single file to discover its sub-plugins. Returns the entries.
    // Uses JUCE's findAllTypesForFile on the single clicked file.
    // Results are cached so subsequent opens don't re-probe.
    const std::vector<PluginEntry>& probeFile(std::size_t fileIndex);

    const std::vector<ScannedFile>& scannedFiles() const { return scannedFiles_; }

private:
    void loadSettings();
    void saveSettings();
    void loadCache();
    void saveCache();

    juce::File scanFolder_;
    std::vector<ScannedFile> scannedFiles_;
    std::unique_ptr<juce::PropertiesFile> props_;

    // Cache: filePath -> list of (name, classId) so we don't re-probe across sessions
    std::map<juce::String, std::vector<PluginEntry>> probeCache_;
};

}  // namespace triggerfish

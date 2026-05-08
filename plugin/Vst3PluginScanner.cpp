#include "Vst3PluginScanner.h"
#include "PortableDataLocation.h"
#include <algorithm>

#if JUCE_WINDOWS
 #include <windows.h>
 // VST3 SDK interfaces — available via JUCE's pluginhost includes
 #include <pluginterfaces/base/ipluginbase.h>
 #include <pluginterfaces/vst/ivstaudioprocessor.h>
#endif

namespace triggerfish {

namespace {

juce::File detectDefaultVst3Folder() {
#if JUCE_WINDOWS
    juce::StringArray candidates = {
        R"(C:\Program Files\Common Files\VST3)",
        R"(C:\Program Files\VST3)",
        R"(C:\Program Files (x86)\Common Files\VST3)"
    };
#elif JUCE_MAC
    juce::StringArray candidates = {
        "/Library/Audio/Plug-Ins/VST3",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library")
            .getChildFile("Audio")
            .getChildFile("Plug-Ins")
            .getChildFile("VST3")
            .getFullPathName()
    };
#else
    juce::StringArray candidates = {
        juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile(".vst3")
            .getFullPathName(),
        "/usr/lib/vst3",
        "/usr/local/lib/vst3"
    };
#endif

    for (const auto& path : candidates) {
        juce::File folder(path);
        if (folder.isDirectory()) {
            return folder;
        }
    }

    return {};
}

}  // namespace

// Lightweight VST3 factory scan on Windows: loads the DLL properly (with InitDll)
// and reads class names from the factory WITHOUT initializing any components.
// This avoids triggering Waves authorization dialogs which happen during
// component->initialize().
static std::vector<Vst3PluginScanner::PluginEntry>
probeLightweight(const juce::String& filePath) {
    std::vector<Vst3PluginScanner::PluginEntry> result;

#if JUCE_WINDOWS
    HMODULE hModule = LoadLibraryW(filePath.toWideCharPointer());
    if (!hModule) return result;

    // InitDll is required for proper module initialization (without it, GetPluginFactory crashes).
    // This is safe — it's module-level init, NOT plugin-level init.
    using InitProc = bool (PLUGIN_API*)();
    auto initDll = reinterpret_cast<InitProc>(GetProcAddress(hModule, "InitDll"));
    if (initDll) {
        if (!initDll()) {
            FreeLibrary(hModule);
            return result;
        }
    }

    using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API*)();
    auto getFactory = reinterpret_cast<GetFactoryProc>(
        GetProcAddress(hModule, "GetPluginFactory"));

    if (getFactory) {
        auto* factory = getFactory();
        if (factory) {
            Steinberg::PFactoryInfo factoryInfo;
            factory->getFactoryInfo(&factoryInfo);
            juce::String manufacturer = juce::String(factoryInfo.vendor).trim();

            auto numClasses = factory->countClasses();
            for (Steinberg::int32 i = 0; i < numClasses; ++i) {
                Steinberg::PClassInfo info;
                factory->getClassInfo(i, &info);

                // Only list audio effect classes
                if (std::strcmp(info.category, kVstAudioEffectClass) != 0)
                    continue;

                Vst3PluginScanner::PluginEntry pe;
                pe.name = juce::String(info.name).trim();
                pe.filePath = filePath;
                pe.manufacturer = manufacturer;

                // Compute JUCE-compatible uniqueId from the class ID (TUID).
                // This matches JUCE's getHashForRange(getNormalisedTUID(info.cid)).
                Steinberg::FUID fuid(info.cid);
                std::array<Steinberg::uint32, 4> norm = {{
                    fuid.getLong1(), fuid.getLong2(),
                    fuid.getLong3(), fuid.getLong4()
                }};
                Steinberg::uint32 hash = 0;
                for (auto v : norm) hash = hash * 31 + v;
                pe.classId = juce::String(static_cast<int>(hash));
                result.push_back(std::move(pe));
            }
        }
    }

    // ExitDll to clean up before unloading
    using ExitProc = bool (PLUGIN_API*)();
    auto exitDll = reinterpret_cast<ExitProc>(GetProcAddress(hModule, "ExitDll"));
    if (exitDll) exitDll();

    FreeLibrary(hModule);
#else
    // On non-Windows, fall back to JUCE's findAllTypesForFile.
    // classId must be the integer uniqueId stringified — JuceVst3Host parses
    // it back via getIntValue() to match against PluginDescription::uniqueId.
    juce::VST3PluginFormat vst3Format;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    vst3Format.findAllTypesForFile(descriptions, filePath);
    for (auto* desc : descriptions) {
        Vst3PluginScanner::PluginEntry pe;
        pe.name = desc->name;
        pe.filePath = filePath;
        pe.classId = juce::String(desc->uniqueId);
        pe.manufacturer = desc->manufacturerName;
        result.push_back(std::move(pe));
    }
#endif

    return result;
}

Vst3PluginScanner::Vst3PluginScanner() {
    loadSettings();
    loadCache();
    if (scanFolder_.isDirectory())
        rescan();
}

juce::File Vst3PluginScanner::getScanFolder() const {
    return scanFolder_;
}

bool Vst3PluginScanner::hasScanFolder() const {
    return scanFolder_.isDirectory();
}

void Vst3PluginScanner::setScanFolder(const juce::File& folder) {
    scanFolder_ = folder;
    saveSettings();
    rescan();
}

void Vst3PluginScanner::rescan() {
    scannedFiles_.clear();
    if (!scanFolder_.isDirectory()) return;

    // Filesystem-only: just enumerate .vst3 files/bundles. No module loading.
    juce::Array<juce::File> vst3Files;
    scanFolder_.findChildFiles(vst3Files, juce::File::findFilesAndDirectories, false, "*.vst3");

    // Also check one level of subdirectories (but skip .vst3 bundle dirs)
    juce::Array<juce::File> subdirs;
    scanFolder_.findChildFiles(subdirs, juce::File::findDirectories, false);
    for (const auto& subdir : subdirs) {
        if (subdir.getFileExtension().equalsIgnoreCase(".vst3")) continue;
        subdir.findChildFiles(vst3Files, juce::File::findFilesAndDirectories, false, "*.vst3");
    }

    for (const auto& file : vst3Files) {
        ScannedFile entry;
        entry.filePath = file.getFullPathName();
        entry.fileName = file.getFileNameWithoutExtension();

        auto it = probeCache_.find(entry.filePath);
        if (it != probeCache_.end()) {
            entry.plugins = it->second;
            entry.probed = true;
        }

        scannedFiles_.push_back(std::move(entry));
    }

    std::sort(scannedFiles_.begin(), scannedFiles_.end(),
              [](const ScannedFile& a, const ScannedFile& b) {
                  return a.fileName.compareIgnoreCase(b.fileName) < 0;
              });
}

const std::vector<Vst3PluginScanner::PluginEntry>&
Vst3PluginScanner::probeFile(std::size_t fileIndex) {
    static const std::vector<PluginEntry> empty;
    if (fileIndex >= scannedFiles_.size()) return empty;

    auto& sf = scannedFiles_[fileIndex];
    if (sf.probed) return sf.plugins;

    // Lightweight probe: reads factory class info without initializing components.
    // This avoids triggering auth dialogs (e.g. Waves license manager).
    sf.plugins = probeLightweight(sf.filePath);

    std::sort(sf.plugins.begin(), sf.plugins.end(),
              [](const PluginEntry& a, const PluginEntry& b) {
                  return a.name.compareIgnoreCase(b.name) < 0;
              });

    sf.probed = true;
    probeCache_[sf.filePath] = sf.plugins;
    saveCache();

    return sf.plugins;
}

// --- Settings persistence ---

void Vst3PluginScanner::loadSettings() {
    auto opts = triggerfish::portablePropertiesOptions(".settings");
    props_ = std::make_unique<juce::PropertiesFile>(opts);
    auto path = props_->getValue("vst3ScanFolder", "");
    if (path.isNotEmpty()) {
        scanFolder_ = juce::File(path);
    }

    // In portable mode, never auto-pick a system VST3 folder — the packaged
    // app should look fresh until the user explicitly sets a folder.
    if (!scanFolder_.isDirectory() && !triggerfish::isPortableMode()) {
        scanFolder_ = detectDefaultVst3Folder();
        if (scanFolder_.isDirectory()) {
            saveSettings();
        }
    }
}

void Vst3PluginScanner::saveSettings() {
    if (props_) {
        props_->setValue("vst3ScanFolder", scanFolder_.getFullPathName());
        props_->saveIfNeeded();
    }
}

// --- Probe cache persistence ---

void Vst3PluginScanner::loadCache() {
    auto opts = triggerfish::portablePropertiesOptions(".vst3cache");
    juce::PropertiesFile cacheFile(opts);
    for (auto& key : cacheFile.getAllProperties().getAllKeys()) {
        auto value = cacheFile.getValue(key);
        std::vector<PluginEntry> entries;
        for (auto& token : juce::StringArray::fromTokens(value, ";", "")) {
            auto parts = juce::StringArray::fromTokens(token, "|", "");
            if (parts.size() >= 2) {
                PluginEntry pe;
                pe.name = parts[0];
                pe.classId = parts[1];
                pe.filePath = key;
                if (parts.size() >= 3) pe.manufacturer = parts[2];
                entries.push_back(std::move(pe));
            }
        }
        if (!entries.empty())
            probeCache_[key] = std::move(entries);
    }
}

void Vst3PluginScanner::saveCache() {
    auto opts = triggerfish::portablePropertiesOptions(".vst3cache");
    juce::PropertiesFile cacheFile(opts);
    cacheFile.clear();

    for (auto& [filePath, entries] : probeCache_) {
        juce::String value;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) value += ";";
            auto safeName = entries[i].name.replace("|", " ").replace(";", " ");
            auto safeMfr = entries[i].manufacturer.replace("|", " ").replace(";", " ");
            value += safeName + "|" + entries[i].classId + "|" + safeMfr;
        }
        cacheFile.setValue(filePath, value);
    }
    cacheFile.saveIfNeeded();
}

}  // namespace triggerfish

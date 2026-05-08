#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace triggerfish {

// Returns the root folder where Triggerfish stores all of its persistent data
// (working files, library databases, VST3 scan settings, JUCE standalone
// wrapper settings).
//
// Behavior:
//   - If a marker file named "Triggerfish.portable" sits next to the
//     executable, returns "<exe-folder>/Data/Triggerfish/" so the app is fully
//     self-contained on disk and ships with no settings carry-over.
//   - Otherwise returns the user's per-application data folder
//     (e.g. "%APPDATA%/Triggerfish/" on Windows). This is the path used during
//     normal development runs.
//
// The folder is created if it does not exist.
juce::File portableDataRoot();

// True when the portable marker file is present next to the executable.
// Use this to skip "convenience" defaults that would otherwise auto-pick
// system paths on first run (e.g. the system VST3 folder), so the packaged
// app feels like a fresh install until the user explicitly chooses.
bool isPortableMode();

// Convenience: builds PropertiesFile::Options that target the portable data
// root, so any caller using JUCE's PropertiesFile (Vst3PluginScanner, the
// custom standalone app, etc.) writes into the same folder hierarchy.
juce::PropertiesFile::Options portablePropertiesOptions(const juce::String& filenameSuffix);

}  // namespace triggerfish

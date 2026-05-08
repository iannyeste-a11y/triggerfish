#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <optional>
#include <vector>

namespace triggerfish {

struct LibraryDescriptor {
    juce::String id;
    juce::String name;
    juce::File databaseFile;
    juce::File rootFolder;
    int fileCount = 0;
    int unresolvedFileCount = 0;
    bool isSoundminerImport = false;
};

struct LibrarySearchResult {
    juce::String path;
    juce::String filename;
    juce::String folder;
    juce::String extension;
    juce::String metadataSummary;
    double durationSeconds = 0.0;
    int sampleRate = 0;
    int channels = 0;
};

class LibraryDatabase {
public:
    using ProgressCallback = std::function<void(double progress, const juce::String& status)>;
    enum class SearchSortMode {
        Name = 1,
        FileLength = 2
    };

    static juce::File librariesRoot();
    static juce::Array<LibraryDescriptor> availableLibraries();
    static std::optional<LibraryDescriptor> findLibrary(const juce::String& libraryId);
    static bool createOrUpdateDatabase(const juce::File& rootFolder, juce::String& error,
                                       const juce::String& displayName = {},
                                       juce::String* resultingLibraryId = nullptr,
                                       ProgressCallback progress = {},
                                       const juce::String& preferredLibraryId = {});
    static bool appendFolderToDatabase(const juce::String& libraryId,
                                       const juce::File& rootFolder,
                                       juce::String& error,
                                       ProgressCallback progress = {});
    static bool renameDatabase(const juce::String& libraryId,
                               const juce::String& newDisplayName,
                               juce::String& error);
    static bool deleteDatabase(const juce::String& libraryId,
                               juce::String& error);
    static std::vector<LibrarySearchResult> search(const juce::String& libraryId,
                                                   const juce::String& query,
                                                   juce::String& error,
                                                   int limit = 500,
                                                   int offset = 0,
                                                   int* totalMatchesOut = nullptr,
                                                   bool monoOnly = false,
                                                   bool stereoOnly = false,
                                                   SearchSortMode sortMode = SearchSortMode::Name,
                                                   bool sortDescending = false);
    static juce::String sanitiseId(const juce::String& text);
};

}  // namespace triggerfish

#include "PortableDataLocation.h"

namespace triggerfish {

namespace {

constexpr const char* kPortableMarkerFilename = "Triggerfish.portable";
constexpr const char* kPortableDataSubfolder = "Data";
constexpr const char* kAppFolderName = "Triggerfish";

juce::File executableFolder() {
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory();
}

bool portableMarkerPresent() {
    return executableFolder().getChildFile(kPortableMarkerFilename).existsAsFile();
}

juce::File computeRoot() {
    if (portableMarkerPresent()) {
        return executableFolder()
            .getChildFile(kPortableDataSubfolder)
            .getChildFile(kAppFolderName);
    }
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(kAppFolderName);
}

}  // namespace

juce::File portableDataRoot() {
    auto root = computeRoot();
    root.createDirectory();
    return root;
}

bool isPortableMode() {
    return portableMarkerPresent();
}

juce::PropertiesFile::Options portablePropertiesOptions(const juce::String& filenameSuffix) {
    juce::PropertiesFile::Options opts;
    opts.applicationName = kAppFolderName;
    opts.filenameSuffix = filenameSuffix;
    opts.osxLibrarySubFolder = "Application Support";
    opts.storageFormat = juce::PropertiesFile::storeAsXML;
    opts.commonToAllUsers = false;
    // Force PropertiesFile to write to the portable root by setting the full
    // file path explicitly. JUCE will use this exact location instead of
    // composing one from applicationName/folderName.
    auto file = portableDataRoot().getChildFile(juce::String(kAppFolderName) + filenameSuffix);
    opts.folderName = file.getParentDirectory().getFullPathName();
    return opts;
}

}  // namespace triggerfish

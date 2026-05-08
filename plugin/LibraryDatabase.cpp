#include "LibraryDatabase.h"
#include "PortableDataLocation.h"

#include <sqlite3.h>
#include <algorithm>
#include <map>
#include <regex>

namespace triggerfish {

namespace {

class ScopedStatement {
public:
    ScopedStatement(sqlite3* db, const juce::String& sql) {
        if (sqlite3_prepare_v2(db, sql.toRawUTF8(), -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }

    ScopedStatement(const ScopedStatement&) = delete;
    ScopedStatement& operator=(const ScopedStatement&) = delete;

    ScopedStatement(ScopedStatement&& other) noexcept
        : stmt_(other.stmt_) {
        other.stmt_ = nullptr;
    }

    ScopedStatement& operator=(ScopedStatement&& other) noexcept {
        if (this != &other) {
            if (stmt_ != nullptr) {
                sqlite3_finalize(stmt_);
            }
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    ~ScopedStatement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    sqlite3_stmt* get() const { return stmt_; }
    explicit operator bool() const { return stmt_ != nullptr; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

juce::File appDataRoot() {
    return triggerfish::portableDataRoot();
}

juce::String sqliteError(sqlite3* db, const juce::String& fallback = "SQLite error") {
    if (db == nullptr) {
        return fallback;
    }
    auto* message = sqlite3_errmsg(db);
    return message != nullptr ? juce::String::fromUTF8(message) : fallback;
}

bool exec(sqlite3* db, const juce::String& sql, juce::String& error) {
    char* rawError = nullptr;
    if (sqlite3_exec(db, sql.toRawUTF8(), nullptr, nullptr, &rawError) == SQLITE_OK) {
        return true;
    }

    error = rawError != nullptr ? juce::String::fromUTF8(rawError) : sqliteError(db);
    if (rawError != nullptr) {
        sqlite3_free(rawError);
    }
    return false;
}

juce::String normaliseForSearch(const juce::String& text) {
    auto normalised = text.toLowerCase();
    normalised = normalised.replaceCharacters("\r\n\t", "   ");
    while (normalised.contains("  ")) {
        normalised = normalised.replace("  ", " ");
    }
    return normalised.trim();
}

juce::String trimSummary(const juce::String& text, int maxChars = 180) {
    auto compact = text.replaceCharacters("\r\n\t", "   ");
    while (compact.contains("  ")) {
        compact = compact.replace("  ", " ");
    }
    compact = compact.trim();
    if (compact.length() <= maxChars) {
        return compact;
    }
    return compact.substring(0, maxChars - 1).trimEnd() + juce::String("...");
}

juce::String collectMetadataSummary(const juce::StringPairArray& metadataValues) {
    juce::StringArray parts;
    for (int i = 0; i < metadataValues.size(); ++i) {
        const auto key = metadataValues.getAllKeys()[i].trim();
        const auto value = metadataValues.getAllValues()[i].trim();
        if (value.isEmpty()) {
            continue;
        }
        if (key.isNotEmpty()) {
            parts.add(key + ": " + value);
        } else {
            parts.add(value);
        }
    }
    return parts.joinIntoString(" | ");
}

juce::String buildSearchText(const juce::File& file, const juce::String& metadataSummary) {
    juce::StringArray parts;
    parts.add(file.getFileNameWithoutExtension());
    parts.add(file.getFileName());
    parts.add(file.getParentDirectory().getFullPathName());
    parts.add(file.getFullPathName());
    if (metadataSummary.isNotEmpty()) {
        parts.add(metadataSummary);
    }
    return normaliseForSearch(parts.joinIntoString(" "));
}

juce::String combineMetadataSummary(const juce::String& description,
                                    const juce::String& keywords,
                                    const juce::String& category,
                                    const juce::String& subCategory,
                                    const juce::String& library) {
    juce::StringArray parts;
    if (description.trim().isNotEmpty()) {
        parts.add(description.trim());
    }
    if (keywords.trim().isNotEmpty()) {
        parts.add("Keywords: " + keywords.trim());
    }
    if (category.trim().isNotEmpty()) {
        auto categoryText = category.trim();
        if (subCategory.trim().isNotEmpty()) {
            categoryText << " / " << subCategory.trim();
        }
        parts.add("Category: " + categoryText);
    } else if (subCategory.trim().isNotEmpty()) {
        parts.add("Category: " + subCategory.trim());
    }
    if (library.trim().isNotEmpty()) {
        parts.add("Library: " + library.trim());
    }
    return trimSummary(parts.joinIntoString(" | "));
}

bool isAudioFile(const juce::File& file, juce::AudioFormatManager& formatManager,
                 LibrarySearchResult& outResult) {
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr) {
        return false;
    }

    outResult.path = file.getFullPathName();
    outResult.filename = file.getFileNameWithoutExtension();
    outResult.folder = file.getParentDirectory().getFullPathName();
    outResult.extension = file.getFileExtension().trimCharactersAtStart(".");
    outResult.durationSeconds = reader->sampleRate > 0.0
        ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate
        : 0.0;
    outResult.sampleRate = static_cast<int>(reader->sampleRate);
    outResult.channels = static_cast<int>(reader->numChannels);
    outResult.metadataSummary = trimSummary(collectMetadataSummary(reader->metadataValues));
    return true;
}

bool initialiseSchema(sqlite3* db, juce::String& error) {
    if (!exec(db,
              "CREATE TABLE IF NOT EXISTS meta ("
              " key TEXT PRIMARY KEY,"
              " value TEXT NOT NULL"
              ");"
              "CREATE TABLE IF NOT EXISTS files ("
              " id INTEGER PRIMARY KEY,"
              " path TEXT NOT NULL UNIQUE,"
              " filename TEXT NOT NULL,"
              " folder TEXT NOT NULL,"
              " extension TEXT NOT NULL,"
              " metadata_summary TEXT NOT NULL,"
              " duration_seconds REAL NOT NULL DEFAULT 0,"
              " sample_rate INTEGER NOT NULL DEFAULT 0,"
              " channels INTEGER NOT NULL DEFAULT 0,"
              " file_size INTEGER NOT NULL DEFAULT 0,"
              " modified_time INTEGER NOT NULL DEFAULT 0,"
              " search_text TEXT NOT NULL"
              ");"
              "CREATE INDEX IF NOT EXISTS idx_files_filename ON files(filename);"
              "CREATE INDEX IF NOT EXISTS idx_files_folder ON files(folder);"
              "CREATE INDEX IF NOT EXISTS idx_files_modified_time ON files(modified_time);",
              error)) {
        return false;
    }

    juce::String ignoredError;
    exec(db,
         "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
         " filename,"
         " folder,"
         " metadata_summary,"
         " search_text,"
         " content='files',"
         " content_rowid='id'"
         ");"
         "CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN"
         " INSERT INTO files_fts(rowid, filename, folder, metadata_summary, search_text)"
         " VALUES (new.id, new.filename, new.folder, new.metadata_summary, new.search_text);"
         "END;"
         "CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN"
         " INSERT INTO files_fts(files_fts, rowid, filename, folder, metadata_summary, search_text)"
         " VALUES('delete', old.id, old.filename, old.folder, old.metadata_summary, old.search_text);"
         "END;"
         "CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE ON files BEGIN"
         " INSERT INTO files_fts(files_fts, rowid, filename, folder, metadata_summary, search_text)"
         " VALUES('delete', old.id, old.filename, old.folder, old.metadata_summary, old.search_text);"
         " INSERT INTO files_fts(rowid, filename, folder, metadata_summary, search_text)"
         " VALUES (new.id, new.filename, new.folder, new.metadata_summary, new.search_text);"
         "END;",
         ignoredError);
    return true;
}

juce::String buildFtsQuery(const juce::String& query) {
    auto tokens = juce::StringArray::fromTokens(normaliseForSearch(query), " ", "\"");
    tokens.removeEmptyStrings();

    juce::StringArray terms;
    for (const auto& token : tokens) {
        auto cleaned = token.replace("\"", "\"\"");
        if (cleaned.isNotEmpty()) {
            terms.add("\"" + cleaned + "\"*");
        }
    }

    return terms.joinIntoString(" AND ");
}

bool rebuildFtsIndex(sqlite3* db, juce::String& error) {
    juce::String checkError;
    ScopedStatement checkStmt(db,
                              "SELECT name FROM sqlite_master "
                              "WHERE type='table' AND name='files_fts'");
    if (!checkStmt) {
        return true;
    }

    if (sqlite3_step(checkStmt.get()) != SQLITE_ROW) {
        return true;
    }

    return exec(db,
                "INSERT INTO files_fts(files_fts) VALUES('rebuild');",
                error);
}

std::optional<LibraryDescriptor> readDescriptor(const juce::File& dbFile) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbFile.getFullPathName().toRawUTF8(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return std::nullopt;
    }

    LibraryDescriptor descriptor;
    descriptor.databaseFile = dbFile;

    {
        ScopedStatement metaStmt(db, "SELECT key, value FROM meta");
        if (metaStmt) {
            while (sqlite3_step(metaStmt.get()) == SQLITE_ROW) {
                const auto key = juce::String::fromUTF8(
                    reinterpret_cast<const char*>(sqlite3_column_text(metaStmt.get(), 0)));
                const auto value = juce::String::fromUTF8(
                    reinterpret_cast<const char*>(sqlite3_column_text(metaStmt.get(), 1)));
                if (key == "id") descriptor.id = value;
                else if (key == "name") descriptor.name = value;
                else if (key == "root_path") descriptor.rootFolder = juce::File(value);
            }
        }
    }

    {
        ScopedStatement countStmt(db, "SELECT COUNT(*) FROM files");
        if (countStmt && sqlite3_step(countStmt.get()) == SQLITE_ROW) {
            descriptor.fileCount = sqlite3_column_int(countStmt.get(), 0);
        }
    }

    sqlite3_close(db);

    if (descriptor.id.isEmpty()) {
        descriptor.id = dbFile.getFileNameWithoutExtension();
    }
    if (descriptor.name.isEmpty()) {
        descriptor.name = descriptor.id;
    }

    return descriptor;
}

juce::File databaseFileForFolder(const juce::File& rootFolder,
                                 const juce::String& preferredLibraryId) {
    const auto librariesDir = LibraryDatabase::librariesRoot();
    const auto desiredId = preferredLibraryId.trim().isNotEmpty()
        ? LibraryDatabase::sanitiseId(preferredLibraryId)
        : LibraryDatabase::sanitiseId(rootFolder.getFileName());

    if (preferredLibraryId.trim().isNotEmpty()) {
        for (const auto& descriptor : LibraryDatabase::availableLibraries()) {
            if (descriptor.id == desiredId && descriptor.databaseFile.existsAsFile()) {
                return descriptor.databaseFile;
            }
        }

        auto candidate = librariesDir.getChildFile(desiredId + ".tfdb");
        int suffix = 2;
        while (candidate.existsAsFile()) {
            candidate = librariesDir.getChildFile(desiredId + "-" + juce::String(suffix++) + ".tfdb");
        }
        return candidate;
    }

    for (const auto& descriptor : LibraryDatabase::availableLibraries()) {
        if (descriptor.rootFolder == rootFolder && descriptor.databaseFile.existsAsFile()) {
            return descriptor.databaseFile;
        }
    }

    auto candidate = librariesDir.getChildFile(desiredId + ".tfdb");
    int suffix = 2;
    while (candidate.existsAsFile()) {
        candidate = librariesDir.getChildFile(desiredId + "-" + juce::String(suffix++) + ".tfdb");
    }
    return candidate;
}

bool writeMetaValue(sqlite3* db, const juce::String& key, const juce::String& value, juce::String& error) {
    ScopedStatement stmt(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    if (!stmt) {
        error = sqliteError(db, "Could not prepare metadata write.");
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, key.toRawUTF8(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.toRawUTF8(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        error = sqliteError(db, "Could not write metadata.");
        return false;
    }
    return true;
}

bool indexFolderIntoDatabase(sqlite3* db, const juce::File& rootFolder, juce::String& error, int* indexedCountOut = nullptr) {
    ScopedStatement insertFile(
        db,
        "INSERT OR REPLACE INTO files("
        " path, filename, folder, extension, metadata_summary, duration_seconds,"
        " sample_rate, channels, file_size, modified_time, search_text"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    if (!insertFile) {
        error = sqliteError(db, "Could not prepare the library database.");
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    int indexedCount = 0;
    for (juce::RangedDirectoryIterator iter(rootFolder, true, "*", juce::File::findFiles);
         iter != juce::RangedDirectoryIterator(); ++iter) {
        LibrarySearchResult result;
        const auto file = iter->getFile();
        if (!isAudioFile(file, formatManager, result)) {
            continue;
        }

        const auto searchText = buildSearchText(file, result.metadataSummary);
        sqlite3_reset(insertFile.get());
        sqlite3_clear_bindings(insertFile.get());
        sqlite3_bind_text(insertFile.get(), 1, result.path.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertFile.get(), 2, result.filename.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertFile.get(), 3, result.folder.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertFile.get(), 4, result.extension.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertFile.get(), 5, result.metadataSummary.toRawUTF8(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(insertFile.get(), 6, result.durationSeconds);
        sqlite3_bind_int(insertFile.get(), 7, result.sampleRate);
        sqlite3_bind_int(insertFile.get(), 8, result.channels);
        sqlite3_bind_int64(insertFile.get(), 9, static_cast<sqlite3_int64>(file.getSize()));
        sqlite3_bind_int64(insertFile.get(), 10, static_cast<sqlite3_int64>(file.getLastModificationTime().toMilliseconds()));
        sqlite3_bind_text(insertFile.get(), 11, searchText.toRawUTF8(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insertFile.get()) != SQLITE_DONE) {
            error = sqliteError(db, "Could not store one of the indexed files.");
            return false;
        }
        ++indexedCount;
    }

    if (indexedCountOut != nullptr) {
        *indexedCountOut = indexedCount;
    }
    return true;
}

juce::Array<juce::File> collectFilesRecursively(const juce::File& rootFolder) {
    juce::Array<juce::File> files;
    for (juce::RangedDirectoryIterator iter(rootFolder, true, "*", juce::File::findFiles);
         iter != juce::RangedDirectoryIterator(); ++iter) {
        files.add(iter->getFile());
    }
    return files;
}

bool indexFilesIntoDatabase(sqlite3* db,
                            const juce::Array<juce::File>& files,
                            juce::String& error,
                            int* indexedCountOut,
                            const LibraryDatabase::ProgressCallback& progress) {
    ScopedStatement insertFile(
        db,
        "INSERT OR REPLACE INTO files("
        " path, filename, folder, extension, metadata_summary, duration_seconds,"
        " sample_rate, channels, file_size, modified_time, search_text"
        ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    if (!insertFile) {
        error = sqliteError(db, "Could not prepare the library database.");
        return false;
    }

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    const int totalFiles = files.size();
    int indexedCount = 0;

    if (progress) {
        progress(totalFiles == 0 ? 1.0 : 0.0, "Scanning audio files...");
    }

    for (int i = 0; i < totalFiles; ++i) {
        LibrarySearchResult result;
        const auto& file = files.getReference(i);
        if (isAudioFile(file, formatManager, result)) {
            const auto searchText = buildSearchText(file, result.metadataSummary);
            sqlite3_reset(insertFile.get());
            sqlite3_clear_bindings(insertFile.get());
            sqlite3_bind_text(insertFile.get(), 1, result.path.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertFile.get(), 2, result.filename.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertFile.get(), 3, result.folder.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertFile.get(), 4, result.extension.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insertFile.get(), 5, result.metadataSummary.toRawUTF8(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(insertFile.get(), 6, result.durationSeconds);
            sqlite3_bind_int(insertFile.get(), 7, result.sampleRate);
            sqlite3_bind_int(insertFile.get(), 8, result.channels);
            sqlite3_bind_int64(insertFile.get(), 9, static_cast<sqlite3_int64>(file.getSize()));
            sqlite3_bind_int64(insertFile.get(), 10, static_cast<sqlite3_int64>(file.getLastModificationTime().toMilliseconds()));
            sqlite3_bind_text(insertFile.get(), 11, searchText.toRawUTF8(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(insertFile.get()) != SQLITE_DONE) {
                error = sqliteError(db, "Could not store one of the indexed files.");
                return false;
            }
            ++indexedCount;
        }

        if (progress) {
            const double pct = totalFiles > 0 ? static_cast<double>(i + 1) / static_cast<double>(totalFiles) : 1.0;
            progress(pct, "Indexing " + file.getFileName());
        }
    }

    if (indexedCountOut != nullptr) {
        *indexedCountOut = indexedCount;
    }
    return true;
}

int queryFileCount(sqlite3* db) {
    ScopedStatement countStmt(db, "SELECT COUNT(*) FROM files");
    if (countStmt && sqlite3_step(countStmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int(countStmt.get(), 0);
    }
    return 0;
}

}  // namespace

juce::File LibraryDatabase::librariesRoot() {
    auto root = appDataRoot().getChildFile("libraries");
    root.createDirectory();
    return root;
}

juce::Array<LibraryDescriptor> LibraryDatabase::availableLibraries() {
    juce::Array<LibraryDescriptor> descriptors;
    auto root = librariesRoot();
    if (!root.exists()) {
        return descriptors;
    }

    juce::Array<juce::File> files;
    root.findChildFiles(files, juce::File::findFiles, false, "*.tfdb");

    std::vector<LibraryDescriptor> sorted;
    for (const auto& file : files) {
        if (auto descriptor = readDescriptor(file); descriptor.has_value()) {
            sorted.push_back(*descriptor);
        }
    }

    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.name.compareNatural(b.name) < 0;
    });

    for (const auto& descriptor : sorted) {
        descriptors.add(descriptor);
    }
    return descriptors;
}

std::optional<LibraryDescriptor> LibraryDatabase::findLibrary(const juce::String& libraryId) {
    const auto libraries = availableLibraries();
    for (const auto& descriptor : libraries) {
        if (descriptor.id == libraryId) {
            return descriptor;
        }
    }
    return std::nullopt;
}

juce::String LibraryDatabase::sanitiseId(const juce::String& text) {
    juce::String cleaned;
    for (auto ch : text) {
        if (juce::CharacterFunctions::isLetterOrDigit(ch)) {
            cleaned << juce::CharacterFunctions::toLowerCase(ch);
        } else if (ch == ' ' || ch == '-' || ch == '_') {
            cleaned << '-';
        }
    }

    while (cleaned.contains("--")) {
        cleaned = cleaned.replace("--", "-");
    }
    cleaned = cleaned.trimCharactersAtStart("-").trimCharactersAtEnd("-");
    if (cleaned.isEmpty()) {
        cleaned = "library";
    }
    return cleaned;
}

bool LibraryDatabase::createOrUpdateDatabase(const juce::File& rootFolder, juce::String& error,
                                             const juce::String& displayName,
                                             juce::String* resultingLibraryId,
                                             ProgressCallback progress,
                                             const juce::String& preferredLibraryId) {
    error.clear();
    if (!rootFolder.isDirectory()) {
        error = "Please choose a valid folder.";
        return false;
    }

    if (progress) {
        progress(0.0, "Scanning folder...");
    }
    auto files = collectFilesRecursively(rootFolder);

    auto dbFile = databaseFileForFolder(rootFolder, preferredLibraryId);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbFile.getFullPathName().toRawUTF8(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        error = sqliteError(db, "Could not create the library database.");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return false;
    }

    if (!initialiseSchema(db, error)) {
        sqlite3_close(db);
        return false;
    }

    if (!exec(db, "BEGIN IMMEDIATE TRANSACTION", error) ||
        !exec(db, "DELETE FROM files", error) ||
        !exec(db, "DELETE FROM meta", error)) {
        sqlite3_close(db);
        return false;
    }

    const auto libraryId = dbFile.getFileNameWithoutExtension();
    const auto resolvedDisplayName = displayName.trim().isNotEmpty() ? displayName.trim()
                                                                      : rootFolder.getFileName();

    if (!writeMetaValue(db, "id", libraryId, error) ||
        !writeMetaValue(db, "name", resolvedDisplayName, error) ||
        !writeMetaValue(db, "root_path", rootFolder.getFullPathName(), error) ||
        !writeMetaValue(db, "indexed_at", juce::Time::getCurrentTime().toISO8601(true), error)) {
        error = sqliteError(db, "Could not write library metadata.");
        sqlite3_close(db);
        return false;
    }

    int indexedCount = 0;
    if (!indexFilesIntoDatabase(db, files, error, &indexedCount, progress)) {
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    if (!rebuildFtsIndex(db, error)) {
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    if (indexedCount == 0) {
        error = "No supported audio files were found in the selected folder.";
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    if (!writeMetaValue(db, "file_count", juce::String(indexedCount), error) ||
        !exec(db, "COMMIT", error)) {
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);

    if (progress) {
        progress(1.0, "Database ready");
    }

    if (resultingLibraryId != nullptr) {
        *resultingLibraryId = libraryId;
    }
    return true;
}

bool LibraryDatabase::appendFolderToDatabase(const juce::String& libraryId,
                                             const juce::File& rootFolder,
                                             juce::String& error,
                                             ProgressCallback progress) {
    error.clear();
    if (!rootFolder.isDirectory()) {
        error = "Please choose a valid folder.";
        return false;
    }

    if (progress) {
        progress(0.0, "Scanning folder...");
    }
    auto files = collectFilesRecursively(rootFolder);

    const auto descriptor = findLibrary(libraryId);
    if (!descriptor.has_value()) {
        error = "That database is no longer available.";
        return false;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(descriptor->databaseFile.getFullPathName().toRawUTF8(), &db,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        error = sqliteError(db, "Could not open the library database.");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return false;
    }

    if (!initialiseSchema(db, error) ||
        !exec(db, "BEGIN IMMEDIATE TRANSACTION", error) ||
        !indexFilesIntoDatabase(db, files, error, nullptr, progress) ||
        !rebuildFtsIndex(db, error)) {
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    const auto fileCount = queryFileCount(db);
    if (fileCount == 0) {
        error = "No supported audio files were found in the selected folder.";
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    if (!writeMetaValue(db, "indexed_at", juce::Time::getCurrentTime().toISO8601(true), error) ||
        !writeMetaValue(db, "file_count", juce::String(fileCount), error) ||
        !exec(db, "COMMIT", error)) {
        exec(db, "ROLLBACK", error);
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);

    if (progress) {
        progress(1.0, "Database updated");
    }
    return true;
}

bool LibraryDatabase::renameDatabase(const juce::String& libraryId,
                                     const juce::String& newDisplayName,
                                     juce::String& error) {
    error.clear();
    const auto descriptor = findLibrary(libraryId);
    if (!descriptor.has_value()) {
        error = "That database is no longer available.";
        return false;
    }

    const auto trimmedName = newDisplayName.trim();
    if (trimmedName.isEmpty()) {
        error = "Please enter a database name.";
        return false;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(descriptor->databaseFile.getFullPathName().toRawUTF8(), &db,
                        SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        error = sqliteError(db, "Could not open the library database.");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return false;
    }

    const bool ok = writeMetaValue(db, "name", trimmedName, error);
    sqlite3_close(db);
    return ok;
}

bool LibraryDatabase::deleteDatabase(const juce::String& libraryId,
                                     juce::String& error) {
    error.clear();
    const auto descriptor = findLibrary(libraryId);
    if (!descriptor.has_value()) {
        error = "That database is no longer available.";
        return false;
    }

    if (!descriptor->databaseFile.existsAsFile()) {
        return true;
    }

    if (!descriptor->databaseFile.deleteFile()) {
        error = "Could not delete the database file.";
        return false;
    }

    return true;
}

std::vector<LibrarySearchResult> LibraryDatabase::search(const juce::String& libraryId,
                                                         const juce::String& query,
                                                         juce::String& error,
                                                         int limit,
                                                         int offset,
                                                         int* totalMatchesOut,
                                                         bool monoOnly,
                                                         bool stereoOnly,
                                                         SearchSortMode sortMode,
                                                         bool sortDescending) {
    std::vector<LibrarySearchResult> results;
    error.clear();
    if (totalMatchesOut != nullptr) {
        *totalMatchesOut = 0;
    }

    const auto descriptor = findLibrary(libraryId);
    if (!descriptor.has_value()) {
        error = "That database is no longer available.";
        return results;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(descriptor->databaseFile.getFullPathName().toRawUTF8(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        error = sqliteError(db, "Could not open the selected library.");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return results;
    }

    const auto ftsQuery = buildFtsQuery(query);
    auto tokens = juce::StringArray::fromTokens(normaliseForSearch(query), " ", "\"");
    tokens.removeEmptyStrings();

    auto appendFilterConditions = [monoOnly, stereoOnly](juce::String& sql, bool& hasWhere) {
        if (monoOnly) {
            sql << (hasWhere ? " AND " : " WHERE ") << "f.channels <= 1";
            hasWhere = true;
        } else if (stereoOnly) {
            sql << (hasWhere ? " AND " : " WHERE ") << "f.channels > 1";
            hasWhere = true;
        }
    };

    auto appendOrderBy = [sortMode, sortDescending](juce::String& sql) {
        sql << " ORDER BY ";
        if (sortMode == LibraryDatabase::SearchSortMode::FileLength) {
            sql << "CASE WHEN f.channels <= 1 THEN 0 ELSE 1 END ASC, "
                << "f.duration_seconds " << (sortDescending ? "DESC" : "ASC")
                << ", f.filename COLLATE NOCASE " << (sortDescending ? "DESC" : "ASC");
        } else {
            sql << "CASE WHEN f.channels <= 1 THEN 0 ELSE 1 END ASC, "
                << "f.filename COLLATE NOCASE " << (sortDescending ? "DESC" : "ASC");
        }
    };

    auto buildFallbackSql = [&tokens, &appendFilterConditions, &appendOrderBy](bool countOnly) {
        juce::String sql =
            countOnly
                ? "SELECT COUNT(*) FROM files f"
                : "SELECT f.path, f.filename, f.folder, f.extension, f.metadata_summary, f.duration_seconds,"
                  " f.sample_rate, f.channels FROM files f";

        bool hasWhere = false;
        if (!tokens.isEmpty()) {
            sql << " WHERE ";
            hasWhere = true;
            for (int i = 0; i < tokens.size(); ++i) {
                if (i > 0) {
                    sql << " AND ";
                }
                sql << "f.search_text LIKE ?";
            }
        }

        appendFilterConditions(sql, hasWhere);
        if (!countOnly) {
            appendOrderBy(sql);
            sql << " LIMIT ? OFFSET ?";
        }
        return sql;
    };

    auto buildFtsSql = [&appendFilterConditions, &appendOrderBy](bool countOnly) {
        juce::String sql =
            countOnly
                ? "SELECT COUNT(*) FROM files_fts fts JOIN files f ON f.id = fts.rowid WHERE files_fts MATCH ?"
                : "SELECT f.path, f.filename, f.folder, f.extension, f.metadata_summary, f.duration_seconds,"
                  " f.sample_rate, f.channels"
                  " FROM files_fts fts JOIN files f ON f.id = fts.rowid"
                  " WHERE files_fts MATCH ?";
        bool hasWhere = true;
        appendFilterConditions(sql, hasWhere);
        if (!countOnly) {
            appendOrderBy(sql);
            sql << " LIMIT ? OFFSET ?";
        }
        return sql;
    };

    juce::String sql = buildFallbackSql(false);
    juce::String countSql = buildFallbackSql(true);

    if (ftsQuery.isNotEmpty()) {
        sql = buildFtsSql(false);
        countSql = buildFtsSql(true);
    }

    ScopedStatement stmt(db, sql);
    bool usingFallback = false;
    if (!stmt) {
        if (ftsQuery.isNotEmpty()) {
            sql = buildFallbackSql(false);
            countSql = buildFallbackSql(true);
            stmt = ScopedStatement(db, sql);
            usingFallback = true;
        }
        if (!stmt) {
            error = sqliteError(db, "Could not run the library search.");
            sqlite3_close(db);
            return results;
        }
    }

    ScopedStatement countStmt(db, countSql);
    if (!countStmt) {
        error = sqliteError(db, "Could not count the library search results.");
        sqlite3_close(db);
        return results;
    }

    auto bindQueryTerms = [&](sqlite3_stmt* statement, bool includePaging) {
        int bindIndex = 1;
        if (ftsQuery.isNotEmpty() && !usingFallback) {
            sqlite3_bind_text(statement, bindIndex++, ftsQuery.toRawUTF8(), -1, SQLITE_TRANSIENT);
        } else {
            for (const auto& token : tokens) {
                const auto like = "%" + token + "%";
                sqlite3_bind_text(statement, bindIndex++, like.toRawUTF8(), -1, SQLITE_TRANSIENT);
            }
        }

        if (includePaging) {
            sqlite3_bind_int(statement, bindIndex++, limit);
            sqlite3_bind_int(statement, bindIndex, std::max(0, offset));
        }
    };

    bindQueryTerms(countStmt.get(), false);
    if (sqlite3_step(countStmt.get()) == SQLITE_ROW && totalMatchesOut != nullptr) {
        *totalMatchesOut = sqlite3_column_int(countStmt.get(), 0);
    }

    bindQueryTerms(stmt.get(), true);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        LibrarySearchResult result;
        result.path = juce::String::fromUTF8(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)));
        result.filename = juce::String::fromUTF8(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1)));
        result.folder = juce::String::fromUTF8(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2)));
        result.extension = juce::String::fromUTF8(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3)));
        result.metadataSummary = juce::String::fromUTF8(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4)));
        result.durationSeconds = sqlite3_column_double(stmt.get(), 5);
        result.sampleRate = sqlite3_column_int(stmt.get(), 6);
        result.channels = sqlite3_column_int(stmt.get(), 7);
        results.push_back(std::move(result));
    }

    sqlite3_close(db);
    return results;
}

}  // namespace triggerfish

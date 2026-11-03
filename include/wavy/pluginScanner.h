#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "wavy/vstPlugin.h"

namespace wavy {

// ok: the factory was read. failed: the child reported a load error. crashed: the child died or printed nothing usable. timedOut: it hung.
// Anything but ok is "blocked": the module is not tried again until its file changes (or the caller asks to retry).
enum class ScanStatus { ok, failed, crashed, timedOut };

struct ModuleRecord {
    std::string path;
    int64_t stamp = 0;  // last write time of the file
    uint64_t size = 0;
    ScanStatus status = ScanStatus::ok;
    std::string error;
    std::vector<VstClassInfo> classes;  // audio classes, in the order VstPlugin::create() takes as classIndex

    bool blocked() const { return status != ScanStatus::ok; }
};

// One plugin a user can load: a class of a scanned module.
struct CatalogEntry {
    std::string path;
    size_t classIndex = 0;
    VstClassInfo info;
};

struct ScanOptions {
    std::string scannerExe;  // wavyScan.exe; empty = next to the running executable
    int timeoutMs = 20000;
    bool retryBlocked = false;
    std::function<void(const std::string&)> progress;  // called with the module about to be scanned
};

struct ScanReport {
    int scanned = 0;  // modules the child was run for
    int cached = 0;   // modules whose cached record was still valid
    int blocked = 0;  // modules that failed, crashed or hung this run
    int removed = 0;  // cached modules no longer installed
    std::vector<std::string> problems;
};

// Loads `path` in a child process (`scannerExe <path>`) so a plugin that crashes, hangs or aborts on load cannot take the caller down.
// Windows only; elsewhere the record has status failed.
ModuleRecord scanModuleInChild(const std::string& scannerExe, const std::string& path, int timeoutMs);

// What wavyScan prints for a loaded module, and what the parent reads back. Exposed for tests.
std::string formatScanOutput(const std::vector<VstClassInfo>& classes);
// Parses the child's stdout into `record` (classes and status). A missing "end" line means the child did not finish.
void parseScanOutput(const std::string& output, ModuleRecord& record);

// The list of installed VST3 plugins: scanned out of process, cached on disk by file timestamp and size, with the modules that
// failed remembered so they are not loaded again every start. Not thread safe; scan a copy on a worker thread and swap it in.
class PluginCatalog {
public:
    // A missing file gives an empty catalog and true; false only for a file that exists but is not a catalog.
    bool load(const std::string& file);
    bool save(const std::string& file, std::string& error) const;

    // Brings the catalog in line with `modulePaths` (the complete list of installed modules): unchanged modules keep their record,
    // new or changed ones are scanned, vanished ones are dropped.
    ScanReport scan(const std::vector<std::string>& modulePaths, const ScanOptions& options = {});

    const std::vector<ModuleRecord>& modules() const { return modules_; }
    // Every class of every module that scanned fine, sorted by name.
    std::vector<CatalogEntry> entries() const;
    // First entry named `name` (case-insensitive) that is an instrument or an effect, as asked.
    std::optional<CatalogEntry> find(const std::string& name, bool instrument) const;

    // Every VST3 module installed in the standard folders, found without loading any of them.
    static std::vector<std::string> installedModules();
    static std::string defaultCachePath();  // %LOCALAPPDATA%\wavy\plugins.cache
    static std::string defaultScannerPath();

private:
    std::vector<ModuleRecord> modules_;
};

}  // namespace wavy

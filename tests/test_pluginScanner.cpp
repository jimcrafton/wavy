#include <gtest/gtest.h>

#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "wavy/pluginScanner.h"

// Out-of-process scanning, driven by wavyScan.exe and the wavyChaos plugin (both built next to the tests).
namespace {

using namespace wavy;
namespace fs = std::filesystem;

// Makes the child scanner misbehave (see src/wavyScan.cpp) until the end of the test.
struct ScannerFault {
    explicit ScannerFault(const char* mode) { _putenv_s("WAVY_SCAN_FAULT", mode); }
    ~ScannerFault() { _putenv_s("WAVY_SCAN_FAULT", ""); }
};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("wavyScanTest" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

ScanOptions options() {
    ScanOptions o;
    o.scannerExe = WAVY_SCAN_PATH;
    o.timeoutMs = 10000;
    return o;
}

TEST(PluginScanner, OutputRoundTripsThroughTheParser) {
    VstClassInfo a;
    a.name = "Tab\tSynth";
    a.vendor = "Maker";
    a.version = "1.2";
    a.subCategories = "Instrument|Synth";
    a.classId = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0xFF};
    ModuleRecord record;
    parseScanOutput(formatScanOutput({a}), record);
    ASSERT_EQ(record.status, ScanStatus::ok);
    ASSERT_EQ(record.classes.size(), 1u);
    EXPECT_EQ(record.classes[0].name, "Tab Synth");  // separators in names become spaces
    EXPECT_EQ(record.classes[0].vendor, "Maker");
    EXPECT_EQ(record.classes[0].subCategories, "Instrument|Synth");
    EXPECT_EQ(record.classes[0].classId, a.classId);
    EXPECT_TRUE(record.classes[0].isInstrument());
}

TEST(PluginScanner, OutputWithoutTheEndMarkerIsACrash) {
    ModuleRecord record;
    parseScanOutput("class\tA\tB\t1\tFx\t00112233445566778899AABBCCDDEEFF\n", record);
    EXPECT_EQ(record.status, ScanStatus::crashed);
    EXPECT_TRUE(record.classes.empty());
}

TEST(PluginScanner, AnErrorLineIsALoadFailureWithItsMessage) {
    ModuleRecord record;
    parseScanOutput("error\tmissing dependency\n", record);
    EXPECT_EQ(record.status, ScanStatus::failed);
    EXPECT_EQ(record.error, "missing dependency");
}

TEST(PluginScanner, ScansAModuleInAChildProcess) {
    const ModuleRecord record = scanModuleInChild(WAVY_SCAN_PATH, WAVY_CHAOS_PATH, 10000);
    ASSERT_EQ(record.status, ScanStatus::ok) << record.error;
    ASSERT_EQ(record.classes.size(), 1u);
    EXPECT_EQ(record.classes[0].name, "wavyChaos");
    EXPECT_FALSE(record.classes[0].isInstrument());
    EXPECT_GT(record.size, 0u);
}

TEST(PluginScanner, AModuleThatDoesNotLoadIsReportedNotThrown) {
    const ModuleRecord record = scanModuleInChild(WAVY_SCAN_PATH, "C:/does/not/exist.vst3", 10000);
    EXPECT_EQ(record.status, ScanStatus::failed);
    EXPECT_FALSE(record.error.empty());
}

TEST(PluginScanner, ACrashingScannerLeavesTheCallerAliveAndBlocksTheModule) {
    ScannerFault fault("crash");
    const ModuleRecord record = scanModuleInChild(WAVY_SCAN_PATH, WAVY_CHAOS_PATH, 10000);
    EXPECT_EQ(record.status, ScanStatus::crashed);
    EXPECT_NE(record.error.find("0xC0000005"), std::string::npos) << record.error;
}

TEST(PluginScanner, AHangingScannerIsKilledAtTheTimeout) {
    ScannerFault fault("hang");
    const auto begin = GetTickCount64();
    const ModuleRecord record = scanModuleInChild(WAVY_SCAN_PATH, WAVY_CHAOS_PATH, 500);
    EXPECT_EQ(record.status, ScanStatus::timedOut);
    EXPECT_LT(GetTickCount64() - begin, 5000u);
}

TEST(PluginScanner, ACatalogScansOnceAndThenUsesTheCache) {
    PluginCatalog catalog;
    ScanReport first = catalog.scan({WAVY_CHAOS_PATH}, options());
    EXPECT_EQ(first.scanned, 1);
    EXPECT_EQ(first.cached, 0);
    ASSERT_EQ(catalog.entries().size(), 1u);

    ScanReport second = catalog.scan({WAVY_CHAOS_PATH}, options());
    EXPECT_EQ(second.scanned, 0);
    EXPECT_EQ(second.cached, 1);

    const auto found = catalog.find("WAVYCHAOS", false);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path, WAVY_CHAOS_PATH);
    EXPECT_EQ(found->classIndex, 0u);
    EXPECT_FALSE(catalog.find("wavyChaos", true).has_value());
}

TEST(PluginScanner, TheCacheSurvivesAFileAndAChangedModuleIsScannedAgain) {
    TempDir dir;
    const fs::path copy = dir.path / "copy.vst3";
    fs::copy_file(WAVY_CHAOS_PATH, copy);
    const std::string cacheFile = (dir.path / "plugins.cache").string();

    PluginCatalog catalog;
    ASSERT_EQ(catalog.scan({copy.string()}, options()).scanned, 1);
    std::string error;
    ASSERT_TRUE(catalog.save(cacheFile, error)) << error;

    PluginCatalog reloaded;
    ASSERT_TRUE(reloaded.load(cacheFile));
    const ScanReport report = reloaded.scan({copy.string()}, options());
    EXPECT_EQ(report.cached, 1);
    EXPECT_EQ(report.scanned, 0);
    ASSERT_EQ(reloaded.entries().size(), 1u);
    EXPECT_EQ(reloaded.entries()[0].info.name, "wavyChaos");

    fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::hours(1));
    EXPECT_EQ(reloaded.scan({copy.string()}, options()).scanned, 1);
}

TEST(PluginScanner, ABlockedModuleIsNotTriedAgainUntilItChangesOrIsRetried) {
    TempDir dir;
    const fs::path copy = dir.path / "bad.vst3";
    fs::copy_file(WAVY_CHAOS_PATH, copy);
    const std::string cacheFile = (dir.path / "plugins.cache").string();

    PluginCatalog catalog;
    {
        ScannerFault fault("crash");
        const ScanReport report = catalog.scan({copy.string()}, options());
        EXPECT_EQ(report.blocked, 1);
        ASSERT_EQ(report.problems.size(), 1u);
    }
    EXPECT_TRUE(catalog.entries().empty());

    std::string error;
    ASSERT_TRUE(catalog.save(cacheFile, error)) << error;
    PluginCatalog reloaded;
    ASSERT_TRUE(reloaded.load(cacheFile));
    ASSERT_EQ(reloaded.modules().size(), 1u);
    EXPECT_EQ(reloaded.modules()[0].status, ScanStatus::crashed);

    // The fault is gone, but the blacklisted module is left alone...
    ScanReport skipped = reloaded.scan({copy.string()}, options());
    EXPECT_EQ(skipped.scanned, 0);
    EXPECT_EQ(skipped.cached, 1);
    EXPECT_TRUE(reloaded.entries().empty());

    // ...until the user asks for a retry.
    ScanOptions retry = options();
    retry.retryBlocked = true;
    EXPECT_EQ(reloaded.scan({copy.string()}, retry).scanned, 1);
    EXPECT_EQ(reloaded.entries().size(), 1u);
}

TEST(PluginScanner, ModulesThatAreNoLongerInstalledAreDropped) {
    PluginCatalog catalog;
    catalog.scan({WAVY_CHAOS_PATH}, options());
    const ScanReport report = catalog.scan({}, options());
    EXPECT_EQ(report.removed, 1);
    EXPECT_TRUE(catalog.modules().empty());
}

TEST(PluginScanner, ALoadOfAFileThatIsNotACatalogIsRefused) {
    TempDir dir;
    const fs::path file = dir.path / "junk.cache";
    {
        FILE* f = nullptr;
        fopen_s(&f, file.string().c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("not a catalog\n", f);
        std::fclose(f);
    }
    PluginCatalog catalog;
    EXPECT_FALSE(catalog.load(file.string()));
    EXPECT_TRUE(catalog.load((dir.path / "missing.cache").string()));  // a missing file is just an empty catalog
}

}  // namespace

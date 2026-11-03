#include "wavy/pluginScanner.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "public.sdk/source/vst/hosting/module.h"

namespace wavy {

namespace {

namespace fs = std::filesystem;

constexpr const char* cacheHeader = "WAVYPLUGINS 1";

std::string clean(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::string toHex(const std::array<uint8_t, 16>& id) {
    static const char digits[] = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : id) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::array<uint8_t, 16> fromHex(const std::string& s) {
    std::array<uint8_t, 16> id{};
    auto nibble = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::toupper(static_cast<unsigned char>(c)) - 'A' + 10) & 15; };
    for (size_t i = 0; i < 16 && i * 2 + 1 < s.size(); ++i) {
        id[i] = static_cast<uint8_t>(nibble(s[i * 2]) * 16 + nibble(s[i * 2 + 1]));
    }
    return id;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        const size_t tab = line.find('\t', from);
        if (tab == std::string::npos) {
            parts.push_back(line.substr(from));
            return parts;
        }
        parts.push_back(line.substr(from, tab - from));
        from = tab + 1;
    }
}

bool parseClassLine(const std::vector<std::string>& f, VstClassInfo& info) {
    if (f.size() < 6) return false;
    info.name = f[1];
    info.vendor = f[2];
    info.version = f[3];
    info.subCategories = f[4];
    info.classId = fromHex(f[5]);
    return true;
}

bool stat(const std::string& path, int64_t& stamp, uint64_t& size) {
    std::error_code ec;
    const auto time = fs::last_write_time(fs::path(path), ec);
    if (ec) return false;
    const auto bytes = fs::file_size(fs::path(path), ec);
    if (ec) return false;
    stamp = static_cast<int64_t>(time.time_since_epoch().count());
    size = bytes;
    return true;
}

const char* statusName(ScanStatus s) {
    switch (s) {
        case ScanStatus::ok: return "ok";
        case ScanStatus::failed: return "failed";
        case ScanStatus::crashed: return "crashed";
        default: return "timedOut";
    }
}

ScanStatus statusOf(const std::string& name) {
    if (name == "ok") return ScanStatus::ok;
    if (name == "failed") return ScanStatus::failed;
    if (name == "crashed") return ScanStatus::crashed;
    return ScanStatus::timedOut;
}

}  // namespace

std::string formatScanOutput(const std::vector<VstClassInfo>& classes) {
    std::string out;
    for (const VstClassInfo& c : classes) {
        out += "class\t" + clean(c.name) + "\t" + clean(c.vendor) + "\t" + clean(c.version) + "\t" + clean(c.subCategories) + "\t" + toHex(c.classId) + "\n";
    }
    out += "end\n";
    return out;
}

void parseScanOutput(const std::string& output, ModuleRecord& record) {
    record.classes.clear();
    record.error.clear();
    bool ended = false;
    std::string childError;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto f = split(line);
        if (f[0] == "class") {
            VstClassInfo info;
            if (parseClassLine(f, info)) record.classes.push_back(std::move(info));
        } else if (f[0] == "error") {
            childError = f.size() > 1 ? f[1] : "unknown error";
        } else if (f[0] == "end") {
            ended = true;
        }
    }
    if (!childError.empty()) {
        record.status = ScanStatus::failed;
        record.error = childError;
        record.classes.clear();
    } else if (!ended) {
        record.status = ScanStatus::crashed;
        record.error = "the scanner stopped before it finished";
        record.classes.clear();
    } else if (record.classes.empty()) {
        record.status = ScanStatus::failed;
        record.error = "no audio classes";
    } else {
        record.status = ScanStatus::ok;
    }
}

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void drain(HANDLE pipe, std::string& into) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return;
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(pipe, buf, std::min<DWORD>(available, sizeof(buf)), &got, nullptr) || got == 0) return;
        into.append(buf, got);
    }
}

}  // namespace
#endif

ModuleRecord scanModuleInChild(const std::string& scannerExe, const std::string& path, int timeoutMs) {
    ModuleRecord record;
    record.path = path;
    stat(path, record.stamp, record.size);
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
        record.status = ScanStatus::failed;
        record.error = "could not create a pipe";
        return record;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + widen(scannerExe) + L"\" \"" + widen(path) + L"\"";
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        record.status = ScanStatus::failed;
        record.error = "could not start " + scannerExe;
        return record;
    }

    std::string output;
    const auto begin = std::chrono::steady_clock::now();
    bool timedOut = false;
    for (;;) {
        drain(readEnd, output);
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() - begin > std::chrono::milliseconds(timeoutMs)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            timedOut = true;
            break;
        }
    }
    drain(readEnd, output);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);

    if (timedOut) {
        record.status = ScanStatus::timedOut;
        record.error = "no answer after " + std::to_string(timeoutMs / 1000) + " s";
        return record;
    }
    parseScanOutput(output, record);
    if (record.status == ScanStatus::crashed) {
        char code[32];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(exitCode));
        record.error = std::string("the scanner crashed (exit code ") + code + ")";
    }
#else
    (void)scannerExe;
    (void)timeoutMs;
    record.status = ScanStatus::failed;
    record.error = "scanning needs Windows";
#endif
    return record;
}

bool PluginCatalog::load(const std::string& file) {
    modules_.clear();
    std::ifstream in(file);
    if (!in) return true;
    std::string line;
    if (!std::getline(in, line) || line != cacheHeader) return false;
    while (std::getline(in, line)) {
        const auto f = split(line);
        if (f[0] == "module" && f.size() >= 6) {
            ModuleRecord m;
            m.path = f[1];
            m.stamp = std::strtoll(f[2].c_str(), nullptr, 10);
            m.size = std::strtoull(f[3].c_str(), nullptr, 10);
            m.status = statusOf(f[4]);
            m.error = f[5];
            modules_.push_back(std::move(m));
        } else if (f[0] == "class" && !modules_.empty()) {
            VstClassInfo info;
            if (parseClassLine(f, info)) modules_.back().classes.push_back(std::move(info));
        }
    }
    return true;
}

bool PluginCatalog::save(const std::string& file, std::string& error) const {
    std::error_code ec;
    const fs::path path = fs::path(file);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        error = "cannot write " + file;
        return false;
    }
    out << cacheHeader << "\n";
    for (const ModuleRecord& m : modules_) {
        out << "module\t" << clean(m.path) << "\t" << m.stamp << "\t" << m.size << "\t" << statusName(m.status) << "\t" << clean(m.error) << "\n";
        for (const VstClassInfo& c : m.classes) {
            out << "class\t" << clean(c.name) << "\t" << clean(c.vendor) << "\t" << clean(c.version) << "\t" << clean(c.subCategories) << "\t"
                << toHex(c.classId) << "\n";
        }
    }
    out.flush();
    if (!out) {
        error = "cannot write " + file;
        return false;
    }
    return true;
}

ScanReport PluginCatalog::scan(const std::vector<std::string>& modulePaths, const ScanOptions& options) {
    ScanReport report;
    const std::string exe = options.scannerExe.empty() ? defaultScannerPath() : options.scannerExe;
    std::map<std::string, ModuleRecord> known;
    for (ModuleRecord& m : modules_) {
        known[m.path] = std::move(m);
    }

    std::vector<ModuleRecord> result;
    std::set<std::string> seen;
    for (const std::string& path : modulePaths) {
        if (!seen.insert(path).second) continue;
        int64_t stamp = 0;
        uint64_t size = 0;
        const bool exists = stat(path, stamp, size);
        const auto it = known.find(path);
        if (it != known.end() && exists && it->second.stamp == stamp && it->second.size == size &&
            (!it->second.blocked() || !options.retryBlocked)) {
            result.push_back(std::move(it->second));
            ++report.cached;
            continue;
        }
        if (options.progress) options.progress(path);
        ModuleRecord record = scanModuleInChild(exe, path, options.timeoutMs);
        ++report.scanned;
        if (record.blocked()) {
            ++report.blocked;
            report.problems.push_back(path + ": " + record.error);
        }
        result.push_back(std::move(record));
    }
    for (const auto& [path, m] : known) {
        if (!seen.count(path)) ++report.removed;
    }
    modules_ = std::move(result);
    return report;
}

std::vector<CatalogEntry> PluginCatalog::entries() const {
    std::vector<CatalogEntry> all;
    for (const ModuleRecord& m : modules_) {
        if (m.blocked()) continue;
        for (size_t i = 0; i < m.classes.size(); ++i) {
            all.push_back({m.path, i, m.classes[i]});
        }
    }
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::sort(all.begin(), all.end(), [&](const CatalogEntry& a, const CatalogEntry& b) {
        const std::string x = lower(a.info.name), y = lower(b.info.name);
        return x != y ? x < y : a.path < b.path;
    });
    return all;
}

std::optional<CatalogEntry> PluginCatalog::find(const std::string& name, bool instrument) const {
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string wanted = lower(name);
    for (const CatalogEntry& e : entries()) {
        if (e.info.isInstrument() == instrument && lower(e.info.name) == wanted) return e;
    }
    return std::nullopt;
}

std::vector<std::string> PluginCatalog::installedModules() {
    std::vector<std::string> paths = VST3::Hosting::Module::getModulePaths();
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string PluginCatalog::defaultCachePath() {
    char* base = nullptr;
    size_t length = 0;
    _dupenv_s(&base, &length, "LOCALAPPDATA");
    const std::string root = base ? std::string(base) : std::string();
    std::free(base);
    return (fs::path(root) / "wavy" / "plugins.cache").string();
}

std::string PluginCatalog::defaultScannerPath() {
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    return (fs::path(std::wstring(self, n)).parent_path() / "wavyScan.exe").string();
#else
    return "wavyScan";
#endif
}

}  // namespace wavy

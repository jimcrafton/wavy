// wavyScan <file.vst3>: loads one VST3 module and prints its audio classes for PluginCatalog. It runs as a child process so a plugin
// that crashes or hangs on load costs the editor nothing. Set WAVY_SCAN_FAULT=crash|hang|fail to test the parent's handling.

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "wavy/pluginScanner.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: wavyScan <file.vst3>\n");
        return 2;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (const char* fault = std::getenv("WAVY_SCAN_FAULT")) {
        const std::string f = fault;
        if (f == "crash") {
            volatile int* p = nullptr;
            *p = 1;
        } else if (f == "hang") {
            for (;;) {
#ifdef _WIN32
                Sleep(1000);
#endif
            }
        } else if (f == "fail") {
            std::printf("error\tinjected failure\n");
            return 1;
        }
    }
    std::string error;
    auto module = wavy::VstModule::load(argv[1], error);
    if (!module) {
        for (char& c : error) {
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        }
        std::printf("error\t%s\n", error.c_str());
        return 1;
    }
    std::fputs(wavy::formatScanOutput(module->classes()).c_str(), stdout);
    std::fflush(stdout);
    // The module is left loaded: unloading a plugin DLL at exit is where several of them crash, and the process is about to end.
    std::_Exit(0);
}

// crash_program: crashes on purpose, for Tests/DiagnosticsCheck.cmake. This is the only
// place a fault is injected: nothing a shipped game contains can crash on request.
//
//   crash_program <crashes folder> segfault|abort|trap [logs folder] [program name]
//
// Records a little log and context first (with a fake ticket that must be redacted), then
// crashes two calls deep so the report names shockwaveTestCrashHere.
#include <Core/Diagnostics.h>
#include <Core/Log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_MSC_VER)
#define SW_NOINLINE __declspec(noinline)
#else
#define SW_NOINLINE __attribute__((noinline))
#endif

namespace diag = shockwave::diagnostics;

volatile int g_sink = 0;

void shockwaveTestCrashHere(const char* kind);
void shockwaveTestCrashOuter(const char* kind);

SW_NOINLINE void shockwaveTestCrashHere(const char* kind) {
    if (std::strcmp(kind, "abort") == 0) std::abort();
    if (std::strcmp(kind, "trap") == 0) {
#if defined(_MSC_VER)
        __debugbreak();
#else
        __builtin_trap();
#endif
    }
    volatile int* nowhere = nullptr;
    *nowhere = 42;
    g_sink = 1;
}

SW_NOINLINE void shockwaveTestCrashOuter(const char* kind) {
    shockwaveTestCrashHere(kind);
    g_sink = 2;   // not a tail call, so this frame stays on the stack
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: crash_program <crashes folder> segfault|abort|trap [logs folder] [program]\n");
        return 2;
    }
    const std::string program = argc > 4 ? argv[4] : "crash_program";
    diag::startRecording();
    diag::setContext("program", program);
    diag::setContext("level", "factory");
    diag::setContext("level_epoch", "3");
    SW_LOG(shockwave::LogGame, Info, "Loading the factory");
    SW_LOG(shockwave::LogGame, Info, "ticket {}", std::string(128, 'a'));   // looks like a secret
    SW_LOG(shockwave::LogGame, Warning, "About to crash on purpose ({})", argv[2]);
    std::string error;
    if (argc > 3) {
        diag::PreviousSession previous;
        if (!diag::beginSession(argv[3], argv[1], program, previous, error)) {
            std::fprintf(stderr, "crash_program: %s\n", error.c_str());
            return 3;
        }
    }
    if (!diag::installCrashHandler({argv[1], program}, error)) {
        std::fprintf(stderr, "crash_program: %s\n", error.c_str());
        return 3;
    }
    shockwaveTestCrashOuter(argv[2]);
    return 0;   // never reached
}

#include "app/App.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

// Usage: FloatingIsland [--frames N] [--screenshot out.ppm] [--no-vsync] [--benchmark N [--csv out.csv]]
//   --frames N        quit by itself after N frames (for automated runs)
//   --screenshot P    save the last frame to P (needs --frames or --benchmark)
//   --no-vsync        start with an uncapped frame rate (MAILBOX / IMMEDIATE if available)
//   --benchmark N     fixed camera path, vsync off, no UI; N measured frames, then print statistics
//   --csv P           with --benchmark: write every frame's GPU timings to P
int main(int argc, char** argv) {
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--frames") == 0 && hasValue) options.maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && hasValue) options.screenshotPath = argv[++i];
        else if (std::strcmp(argv[i], "--no-vsync") == 0) options.vsync = false;
        else if (std::strcmp(argv[i], "--benchmark") == 0 && hasValue) options.benchmarkFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--csv") == 0 && hasValue) options.benchmarkCsv = argv[++i];
        else std::fprintf(stderr, "Ignoring unknown argument %s\n", argv[i]);
    }
    try {
        App app;
        app.run(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}

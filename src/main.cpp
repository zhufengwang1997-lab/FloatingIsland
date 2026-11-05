#include "app/App.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

// Usage: FloatingIsland [--frames N] [--screenshot out.ppm] [--no-vsync]
//   --frames N        quit by itself after N frames (for automated runs)
//   --screenshot P    save the last frame to P (needs --frames)
//   --no-vsync        start with an uncapped frame rate (MAILBOX / IMMEDIATE if available)
int main(int argc, char** argv) {
    int maxFrames = 0;
    std::string screenshot;
    bool vsync = true;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--frames") == 0 && hasValue) maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && hasValue) screenshot = argv[++i];
        else if (std::strcmp(argv[i], "--no-vsync") == 0) vsync = false;
    }
    try {
        App app;
        app.run(maxFrames, screenshot, vsync);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}

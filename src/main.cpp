#include "app/App.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

// Usage: FloatingIsland [--frames N] [--screenshot out.ppm]
//   --frames N        quit by itself after N frames (for automated runs)
//   --screenshot P    save the last frame to P (needs --frames)
int main(int argc, char** argv) {
    int maxFrames = 0;
    std::string screenshot;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) maxFrames = std::atoi(argv[i + 1]);
        if (std::strcmp(argv[i], "--screenshot") == 0) screenshot = argv[i + 1];
    }
    try {
        App app;
        app.run(maxFrames, screenshot);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}

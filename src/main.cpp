#include "app/App.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

// Usage: FloatingIsland [--frames N]
//   --frames N  quit by itself after N frames (for automated runs)
int main(int argc, char** argv) {
    int maxFrames = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) maxFrames = std::atoi(argv[i + 1]);
    }
    try {
        App app;
        app.run(maxFrames);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}

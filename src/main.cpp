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
//   --grass-density D multiply the number of grass tufts by D (default 1)
//   --grass-shadows   also draw grass into the shadow map (off by default)
//   --no-culling      draw every chunk, also those outside the camera view
//   --no-lod          draw all grass at every distance
//   --no-ui           start with the debug panel hidden (F1 / H shows it)
//   --no-clouds       no sea of clouds below the island
//   --time H          time of day in hours, 0..24 (default 15; 6 sunrise, 18 sunset, 0 midnight)
//   --day-cycle       let the time of day run by itself (not in benchmarks)
//   --ibl-every-frame remake the IBL cube maps every frame, as the day cycle does (to measure it)
//   --repeat K        with --benchmark: draw the scene passes K times per frame (default 16) so the
//                     GPU stays fully loaded; reported shadow / main times are per single pass
int main(int argc, char** argv) {
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--frames") == 0 && hasValue) options.maxFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && hasValue) options.screenshotPath = argv[++i];
        else if (std::strcmp(argv[i], "--no-vsync") == 0) options.vsync = false;
        else if (std::strcmp(argv[i], "--benchmark") == 0 && hasValue) options.benchmarkFrames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--csv") == 0 && hasValue) options.benchmarkCsv = argv[++i];
        else if (std::strcmp(argv[i], "--repeat") == 0 && hasValue) options.sceneRepeat = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--grass-density") == 0 && hasValue) options.grassDensity = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--grass-shadows") == 0) options.grassShadows = true;
        else if (std::strcmp(argv[i], "--no-culling") == 0) options.frustumCulling = false;
        else if (std::strcmp(argv[i], "--no-lod") == 0) options.distanceLod = false;
        else if (std::strcmp(argv[i], "--no-ui") == 0) options.showUi = false;
        else if (std::strcmp(argv[i], "--no-clouds") == 0) options.cloudSea = false;
        else if (std::strcmp(argv[i], "--time") == 0 && hasValue) options.timeOfDay = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--day-cycle") == 0) options.dayCycle = true;
        else if (std::strcmp(argv[i], "--ibl-every-frame") == 0) options.iblEveryFrame = true;
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

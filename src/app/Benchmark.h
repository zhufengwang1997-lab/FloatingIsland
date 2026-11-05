#pragma once

#include <string>
#include <vector>

class Camera;
class GpuProfiler;

// Repeatable performance run: the camera follows a fixed path (a wide orbit around the island, then a
// low pass over the grass), animation time advances a fixed step per frame, and every frame's CPU and
// GPU timings are recorded. Results are printed as mean / percentiles and optionally saved as CSV.
class Benchmark {
public:
    static constexpr int kWarmupFrames = 120;  // held at the first pose, not recorded

    // sceneRepeat: how many times the renderer draws the shadow and main passes per frame
    // (Renderer::sceneRepeat); their timings are divided by it, so results are per single pass.
    Benchmark(int measuredFrames, int sceneRepeat, std::string csvPath = {});

    int totalFrames() const { return kWarmupFrames + measuredFrames_; }
    // Camera pose and animation time for a frame (0 .. totalFrames - 1).
    void pose(int frame, Camera& camera) const;
    static float animationTime(int frame) { return static_cast<float>(frame) / 60.0f; }

    // Call after drawing each frame. GPU results arrive a frame or two late; each is recorded once.
    void record(int frame, float cpuMs, const GpuProfiler& gpu);
    void report() const;

private:
    struct Sample {
        float frame, ibl, shadow, main, composite, ui;
    };

    int measuredFrames_;
    int sceneRepeat_;
    std::string csvPath_;
    std::vector<float> cpuMs_;
    std::vector<Sample> gpu_;
    unsigned long long lastGpuSample_ = 0;
};

#include "app/Benchmark.h"

#include "renderer/GpuProfiler.h"
#include "scene/Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

void lookAt(Camera& camera, glm::vec3 position, glm::vec3 target) {
    const glm::vec3 d = glm::normalize(target - position);
    camera.position = position;
    camera.yaw = std::atan2(d.z, d.x);
    camera.pitch = std::asin(d.y);
}

struct Stats {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0;
};

Stats computeStats(std::vector<float> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (float x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    return s;
}

}  // namespace

Benchmark::Benchmark(int measuredFrames, int sceneRepeat, std::string csvPath)
    : measuredFrames_(std::max(measuredFrames, 1)), sceneRepeat_(std::max(sceneRepeat, 1)), csvPath_(std::move(csvPath)) {}

void Benchmark::pose(int frame, Camera& camera) const {
    const float t = static_cast<float>(std::max(frame - kWarmupFrames, 0)) / static_cast<float>(measuredFrames_);
    const float twoPi = glm::two_pi<float>();
    if (t < 0.5f) {
        // Wide orbit: the whole island, its underside edge and the sky.
        const float a = t / 0.5f * twoPi;
        lookAt(camera, {85.0f * std::sin(a), 30.0f, 85.0f * std::cos(a)}, {0.0f, 2.0f, 0.0f});
    } else {
        // Low pass over the island: grass and trees fill the screen (the expensive view).
        const float a = (t - 0.5f) / 0.5f * twoPi;
        const glm::vec3 p{22.0f * std::sin(a), 16.0f, 22.0f * std::cos(a)};
        const glm::vec3 tangent{std::cos(a), 0.0f, -std::sin(a)};
        lookAt(camera, p, p + tangent * 10.0f + glm::vec3(0.0f, -5.0f, 0.0f));
    }
}

void Benchmark::record(int frame, float cpuMs, const GpuProfiler& gpu) {
    if (frame < kWarmupFrames) {
        lastGpuSample_ = gpu.sampleCount();
        return;
    }
    cpuMs_.push_back(cpuMs);
    if (gpu.supported() && gpu.sampleCount() != lastGpuSample_) {
        lastGpuSample_ = gpu.sampleCount();
        const float perPass = 1.0f / static_cast<float>(sceneRepeat_);
        gpu_.push_back({gpu.lastMilliseconds(GpuScope::Frame), gpu.lastMilliseconds(GpuScope::Shadow) * perPass,
                        gpu.lastMilliseconds(GpuScope::Main) * perPass, gpu.lastMilliseconds(GpuScope::Composite),
                        gpu.lastMilliseconds(GpuScope::Ui)});
    }
}

void Benchmark::report() const {
    auto column = [&](float Sample::*m) {
        std::vector<float> v;
        for (const Sample& s : gpu_) v.push_back(s.*m);
        return v;
    };
    std::vector<float> scene;
    for (const Sample& s : gpu_) scene.push_back(s.shadow + s.main);

    std::printf("\nBenchmark: %zu frames measured (%d warm-up), %zu GPU samples, scene passes x%d per frame\n",
                cpuMs_.size(), kWarmupFrames, gpu_.size(), sceneRepeat_);
    if (sceneRepeat_ > 1) std::printf("Shadow / main timings below are per single pass (total / %d)\n", sceneRepeat_);
    std::printf("%-22s %8s %8s %8s %8s\n", "ms", "mean", "p50", "p95", "p99");
    auto row = [](const char* name, const std::vector<float>& v) {
        const Stats s = computeStats(v);
        std::printf("%-22s %8.3f %8.3f %8.3f %8.3f\n", name, s.mean, s.p50, s.p95, s.p99);
    };
    row("CPU frame", cpuMs_);
    row("GPU scene (sh+main)", scene);
    row("GPU shadow pass", column(&Sample::shadow));
    row("GPU main pass", column(&Sample::main));
    row("GPU composite*", column(&Sample::composite));
    row("GPU UI", column(&Sample::ui));
    row("GPU frame*", column(&Sample::frame));
    std::printf("* includes waiting for the display to release the swapchain image\n");

    if (csvPath_.empty()) return;
    std::FILE* f = std::fopen(csvPath_.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "Cannot write %s\n", csvPath_.c_str());
        return;
    }
    std::fprintf(f, "sample,gpu_frame_ms,gpu_shadow_ms,gpu_main_ms,gpu_composite_ms,gpu_ui_ms\n");
    for (size_t i = 0; i < gpu_.size(); ++i) {
        const Sample& s = gpu_[i];
        std::fprintf(f, "%zu,%.4f,%.4f,%.4f,%.4f,%.4f\n", i, s.frame, s.shadow, s.main, s.composite, s.ui);
    }
    std::fclose(f);
    std::printf("Per-frame GPU timings written to %s\n", csvPath_.c_str());
}

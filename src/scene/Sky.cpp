#include "scene/Sky.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace {

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Three looks of the sky; evaluateSky blends between them by the sun's height.
struct Palette {
    glm::vec3 zenith, horizon, below, glow;
};
constexpr Palette kDay{{0.10f, 0.24f, 0.68f}, {0.38f, 0.55f, 0.80f}, {0.24f, 0.38f, 0.62f}, {0.0f, 0.0f, 0.0f}};
constexpr Palette kSunset{{0.09f, 0.12f, 0.30f}, {0.62f, 0.42f, 0.36f}, {0.20f, 0.18f, 0.28f}, {1.40f, 0.50f, 0.12f}};
constexpr Palette kNight{{0.005f, 0.008f, 0.019f}, {0.012f, 0.017f, 0.032f}, {0.006f, 0.009f, 0.020f}, {0.0f, 0.0f, 0.0f}};

Palette mix(const Palette& a, const Palette& b, float t) {
    return {glm::mix(a.zenith, b.zenith, t), glm::mix(a.horizon, b.horizon, t), glm::mix(a.below, b.below, t),
            glm::mix(a.glow, b.glow, t)};
}

// How much of the sunlight gets through the atmosphere, per color. Light is scattered out of the
// beam along the way, blue much more than red (that scattered blue is the blue sky). Near the
// horizon the light crosses far more air, so only orange and red are left: sunsets.
glm::vec3 transmittance(float sinElevation) {
    // Relative air mass (Kasten & Young): 1 straight up, about 38 at the horizon.
    const float elevationDeg = glm::degrees(std::asin(std::clamp(sinElevation, 0.0f, 1.0f)));
    const float airMass = 1.0f / (std::max(sinElevation, 0.0f) + 0.50572f * std::pow(elevationDeg + 6.07995f, -1.6364f));
    const glm::vec3 extinction{0.04f, 0.10f, 0.24f};  // per air mass; tuned for the look, not measured
    return glm::exp(-extinction * airMass);
}

}  // namespace

glm::vec3 sunDirectionAt(float hour, const SunPath& path) {
    // Hour angle: 0 at noon, the sun turns 15 degrees per hour.
    const float h = (hour - 12.0f) / 12.0f * glm::pi<float>();
    const float lat = glm::radians(path.latitudeDeg);
    const float heading = glm::radians(path.headingDeg);
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 south{std::cos(heading), 0.0f, std::sin(heading)};
    const glm::vec3 west{-south.z, 0.0f, south.x};  // south turned 90 degrees towards +Z
    // At the equinox the sun moves on a circle tilted by the latitude: east at 6:00, highest in the
    // south at noon, west at 18:00, and as far below the horizon at midnight.
    return glm::normalize(std::cos(h) * (std::cos(lat) * up + std::sin(lat) * south) + std::sin(h) * west);
}

SkyState evaluateSky(glm::vec3 sunDirection, float sunIntensity) {
    SkyState s;
    s.sunDirection = glm::normalize(sunDirection);
    const float y = s.sunDirection.y;  // sine of the sun's elevation

    // Day above 20 degrees, sunset colors around the horizon, night from 15 degrees below it.
    const Palette p = y >= 0.0f ? mix(kSunset, kDay, smoothstep(0.0f, 0.35f, y))
                                : mix(kSunset, kNight, smoothstep(0.0f, 0.25f, -y));
    s.zenith = p.zenith;
    s.horizon = p.horizon;
    s.below = p.below;
    s.glow = p.glow;

    const glm::vec3 sunlight = glm::vec3(1.0f, 0.97f, 0.92f) * sunIntensity * transmittance(y) /
                               transmittance(1.0f);  // noon keeps exactly sunIntensity
    s.sunDisk = sunlight * smoothstep(-0.03f, 0.0f, y);
    s.night = smoothstep(-0.02f, -0.20f, y);

    // The sun lights the scene until it touches the horizon; the moon (opposite the sun, which is
    // roughly true around full moon) takes over once the sun is a few degrees below it.
    if (y > -0.03f) {
        s.lightDirection = s.sunDirection;
        s.lightColor = sunlight * smoothstep(0.0f, 0.10f, y);
    } else {
        s.lightDirection = -s.sunDirection;
        s.lightColor = glm::vec3(0.55f, 0.68f, 1.0f) * 0.30f * smoothstep(0.05f, 0.20f, -y);
    }
    s.exposureScale = 1.0f + 3.0f * smoothstep(0.05f, -0.25f, y);
    return s;
}

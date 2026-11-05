#pragma once

#include <glm/glm.hpp>

// Day/night cycle: where the sun is at a time of day, and what the sky and the light look like then.
// Everything here runs on the CPU once per frame; the shaders only get the resulting colors.

// The sun's path through the sky, simplified to the equinox (the sun rises due east, sets due west
// and is up for exactly 12 hours). latitude sets how high it climbs: 90 - latitude degrees at noon.
struct SunPath {
    float latitudeDeg = 35.0f;
    float headingDeg = -30.0f;  // which world direction is south (from +X towards +Z)
};

// Unit vector towards the sun at a time of day (hours, 0..24; 12 = noon). Below the horizon at night.
glm::vec3 sunDirectionAt(float hour, const SunPath& path);

// What the renderer needs to draw the sky and light the scene for one sun position.
struct SkyState {
    // Sky gradient (HDR radiance): straight up, at the horizon, and below the horizon (the haze
    // under the floating island).
    glm::vec3 zenith{0.0f};
    glm::vec3 horizon{0.0f};
    glm::vec3 below{0.0f};
    glm::vec3 glow{0.0f};        // warm band along the horizon at sunrise and sunset, brightest towards the sun
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};  // the real sun, also when it is below the horizon
    glm::vec3 sunDisk{0.0f};     // radiance of the sun disk and its halo (zero once it has set)
    float night = 0.0f;          // 0 by day, 1 at night: stars and moon fade in with it
    // The one directional light that lights the scene and casts shadows: the sun by day, the moon by
    // night. Both are dark for a moment around sunset, so the switch (and the shadows jumping to the
    // moon's direction) is never visible.
    glm::vec3 lightDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 lightColor{0.0f};
    // Stands in for the eye adapting to the dark: the renderer multiplies its exposure by this.
    float exposureScale = 1.0f;
};

// sunIntensity: the sun's brightness at noon (before the atmosphere tints and dims it).
SkyState evaluateSky(glm::vec3 sunDirection, float sunIntensity);

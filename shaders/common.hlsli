#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// Per-frame data, set 0 binding 0. Matches FrameUniforms in src/renderer/Renderer.h.
// Every member is 16-byte aligned, so HLSL constant-buffer packing equals the C++ layout.
struct FrameData
{
    float4x4 viewProj;
    float4x4 lightViewProj;   // world -> shadow map clip space
    float4x4 invViewProj;     // clip -> world, for reconstructing view rays
    float4 cameraPosition;    // xyz
    float4 sunDirection;      // xyz: unit vector towards the sun
    float4 sunColor;          // rgb: color * intensity
    float4 params;            // x: exposure, y: 1 / shadow map size, z: time (s), w: wind strength
    float4 lod;               // distance LOD: x: start distance, y: end distance, z: fraction kept at
                              // the end, w: fade width (in rank units)
    // Sky for the time of day (computed on the CPU, src/scene/Sky.cpp). sunDirection / sunColor
    // above are the light that shades the scene: the sun by day, the moon by night.
    float4 skyZenith;         // rgb; w: night amount, 0..1 (stars and moon)
    float4 skyHorizon;        // rgb
    float4 skyBelow;          // rgb: the haze below the horizon
    float4 skyGlow;           // rgb: sunrise / sunset band along the horizon
    float4 skySun;            // xyz: unit vector towards the real sun (also when it has set)
    float4 skySunDisk;        // rgb: radiance of the sun disk and its halo
    float4 ibl;               // x: diffuse IBL intensity, y: mip of the specular cube for roughness 1,
                              // z: specular IBL intensity
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

#ifndef COMMON_NO_OBJECT
// Per-draw data, pushed with vkCmdPushConstants. GLM matrices are column-major,
// which matches HLSL's default column_major packing.
struct ObjectData
{
    float4x4 model;
};
[[vk::push_constant]] ObjectData object;
#endif

// Sky radiance seen along a direction (HDR, before exposure), without the sun disk, stars and
// moon (SkyBackground in composite.hlsl adds those): a horizon-to-zenith gradient, a haze below the
// horizon (the island floats, so we can look down), the sunset glow and the sun's wide halo.
float3 SkyRadiance(float3 dir)
{
    float3 sky = dir.y >= 0.0 ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, sqrt(saturate(dir.y)))
                              : lerp(frame.skyHorizon.rgb, frame.skyBelow.rgb, sqrt(saturate(-dir.y)));
    // Sunrise / sunset: a warm band hugging the horizon, much brighter on the sun's side.
    float towardsSun = dot(normalize(dir.xz + 1e-5), normalize(frame.skySun.xz + 1e-5)) * 0.5 + 0.5;
    sky += frame.skyGlow.rgb * exp(-abs(dir.y) * 7.0) * (0.15 + 0.85 * towardsSun * towardsSun * towardsSun);
    float sunCos = saturate(dot(dir, frame.skySun.xyz));
    sky += frame.skySunDisk.rgb * pow(sunCos, 24.0) * 0.08;
    return sky;
}

#ifndef COMMON_NO_OBJECT
// Per-instance data (vertex binding 1). Matches InstanceData in src/scene/Mesh.h.
// Objects drawn once use a single identity instance.
struct InstanceInput
{
    [[vk::location(4)]] float4 positionScale : INSTANCE_POSITION;  // xyz: offset, w: uniform scale
    [[vk::location(5)]] float4 params : INSTANCE_PARAMS;           // x: yaw, y: tint, z: wind sway, w: LOD rank
};

// Distance LOD: the fraction of instances kept at a distance, from 1 + fade (all of them, fully
// grown) at lod.x down to lod.z at lod.y and beyond. Must match Renderer::lodFraction on the CPU.
float LodFraction(float distance)
{
    float t = saturate((distance - frame.lod.x) / max(frame.lod.y - frame.lod.x, 1e-3));
    return lerp(1.0 + frame.lod.w, frame.lod.z, t);
}

// The scale of an instance under distance LOD. Instances are dropped in rank order; each one shrinks
// to nothing over a short fade instead of popping out. The ones left grow a little (fraction^-0.3),
// so the thinner grass still covers most of the ground; growing by the full 1 / sqrt(fraction) keeps
// the coverage exactly but makes the tufts look coarse. A negative rank means no LOD.
float LodScale(float3 position, float rank)
{
    if (rank < 0.0) return 1.0;
    float fraction = LodFraction(distance(position, frame.cameraPosition.xyz));
    float grow = pow(clamp(fraction, frame.lod.z, 1.0), -0.3);
    return saturate((fraction - rank) / frame.lod.w) * grow;
}

float3 RotateYaw(float3 v, float yaw)
{
    float s, c;
    sincos(yaw, s, c);
    return float3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Instance space -> world space, including wind sway. The shadow pass uses the same function,
// so shadows move with the foliage.
float3 InstanceToWorld(float3 localPos, InstanceInput inst)
{
    float scale = inst.positionScale.w * LodScale(inst.positionScale.xyz, inst.params.w);
    float3 p = RotateYaw(localPos, inst.params.x) * scale + inst.positionScale.xyz;
    float3 world = mul(object.model, float4(p, 1.0)).xyz;

    // Sway grows with the square of the height above the instance origin, so the base stays put.
    // Two sine waves with a phase that travels across the island read as gusts.
    float sway = inst.params.z * frame.params.w * localPos.y * localPos.y * scale;
    float phase = dot(world.xz, float2(0.13, 0.09));
    float t = frame.params.z;
    float gust = 0.6 * sin(t * 1.3 + phase) + 0.3 * sin(t * 2.9 + phase * 2.1) + 0.4;
    world.xz += float2(0.8, 0.6) * sway * gust;
    return world;
}

float3 InstanceNormalToWorld(float3 n, InstanceInput inst)
{
    return mul((float3x3)object.model, RotateYaw(n, inst.params.x));  // uniform scale only
}
#endif  // COMMON_NO_OBJECT

#endif

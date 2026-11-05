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
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Per-draw data, pushed with vkCmdPushConstants. GLM matrices are column-major,
// which matches HLSL's default column_major packing.
struct ObjectData
{
    float4x4 model;
};
[[vk::push_constant]] ObjectData object;

// Sky radiance seen along a direction (HDR, before exposure): a horizon-to-zenith gradient, a
// lighter haze below the horizon (the island floats, so we can look down) and the sun disk.
float3 SkyRadiance(float3 dir)
{
    float3 zenith = float3(0.10, 0.24, 0.68);
    float3 horizon = float3(0.38, 0.55, 0.80);
    float3 below = float3(0.24, 0.38, 0.62);
    float3 sky = dir.y >= 0.0 ? lerp(horizon, zenith, sqrt(saturate(dir.y)))
                              : lerp(horizon, below, sqrt(saturate(-dir.y)));
    float sunCos = saturate(dot(dir, frame.sunDirection.xyz));
    sky += frame.sunColor.rgb * (pow(sunCos, 3000.0) * 8.0 + pow(sunCos, 24.0) * 0.08);
    return sky;
}

// Per-instance data (vertex binding 1). Matches InstanceData in src/scene/Mesh.h.
// Objects drawn once use a single identity instance.
struct InstanceInput
{
    [[vk::location(4)]] float4 positionScale : INSTANCE_POSITION;  // xyz: offset, w: uniform scale
    [[vk::location(5)]] float4 params : INSTANCE_PARAMS;           // x: yaw, y: tint, z: wind sway
};

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
    float3 p = RotateYaw(localPos, inst.params.x) * inst.positionScale.w + inst.positionScale.xyz;
    float3 world = mul(object.model, float4(p, 1.0)).xyz;

    // Sway grows with the square of the height above the instance origin, so the base stays put.
    // Two sine waves with a phase that travels across the island read as gusts.
    float sway = inst.params.z * frame.params.w * localPos.y * localPos.y * inst.positionScale.w;
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

#endif

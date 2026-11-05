#ifndef COMMON_HLSLI
#define COMMON_HLSLI

// Per-frame data, set 0 binding 0. Matches FrameUniforms in src/renderer/Renderer.h.
// Every member is 16-byte aligned, so HLSL constant-buffer packing equals the C++ layout.
struct FrameData
{
    float4x4 viewProj;
    float4x4 lightViewProj;   // world -> shadow map clip space
    float4 cameraPosition;    // xyz
    float4 sunDirection;      // xyz: unit vector towards the sun
    float4 sunColor;          // rgb: color * intensity
    float4 params;            // x: exposure, y: 1 / shadow map size
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;

// Per-draw data, pushed with vkCmdPushConstants. GLM matrices are column-major,
// which matches HLSL's default column_major packing.
struct ObjectData
{
    float4x4 model;
};
[[vk::push_constant]] ObjectData object;

#endif

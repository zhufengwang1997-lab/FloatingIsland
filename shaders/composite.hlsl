// Final pass: HDR scene -> display. Fills the sky behind the geometry, applies exposure and
// ACES tonemapping, and writes to the sRGB swapchain (which applies the gamma curve).
#include "common.hlsli"

[[vk::binding(2, 0)]] Texture2D<float4> hdrScene;

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

// One triangle that covers the screen: vertices at uv (0,0), (2,0), (0,2). No vertex buffer.
VSOutput VSMain(uint id : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Narkowicz's fit of the ACES filmic curve: HDR radiance -> [0, 1].
float3 ACESFilm(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 PSMain(VSOutput input) : SV_Target0
{
    // Same resolution as the swapchain, so read the texel directly: no sampler, no filtering.
    float4 scene = hdrScene.Load(int3(input.position.xy, 0));
    float3 radiance = scene.rgb;
    if (scene.a < 0.5)
    {
        // Ray through this pixel. uv.y = 0 is the top row, which is NDC y = -1 in Vulkan.
        float4 world = mul(frame.invViewProj, float4(input.uv * 2.0 - 1.0, 1.0, 1.0));
        radiance = SkyRadiance(normalize(world.xyz / world.w - frame.cameraPosition.xyz));
    }
    return float4(ACESFilm(radiance * frame.params.x), 1.0);
}

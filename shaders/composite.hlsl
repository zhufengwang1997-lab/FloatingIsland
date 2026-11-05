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

float Hash3(float3 p)
{
    p = frac(p * float3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return frac((p.x + p.y) * p.z);
}

// The sky behind everything: SkyRadiance plus what only the background shows (it is left out of
// the clouds' fog and, later, of the ambient light): the sun disk, and at night the moon and stars.
float3 SkyBackground(float3 dir)
{
    float3 sky = SkyRadiance(dir);
    float sunCos = saturate(dot(dir, frame.skySun.xyz));
    sky += frame.skySunDisk.rgb * pow(sunCos, 3000.0) * 8.0;

    float night = frame.skyZenith.w;
    if (night > 0.0)
    {
        // Moon: opposite the sun, a disk of about 1.2 degrees with a faint halo.
        float moonCos = dot(dir, -frame.skySun.xyz);
        float3 moonColor = float3(0.85, 0.88, 1.0);
        sky += night * moonColor * (smoothstep(0.99975, 0.99980, moonCos) * 1.5 + pow(saturate(moonCos), 200.0) * 0.03);

        // Stars: chop the space around the camera into small cells and light a few of them. Fixed in
        // the sky (they depend only on the direction), fading towards the horizon.
        float3 p = dir * 320.0;
        float3 cell = floor(p);
        float h = Hash3(cell);
        if (h > 0.9965)
        {
            float3 centre = cell + 0.5 + (float3(Hash3(cell + 11.0), Hash3(cell + 23.0), Hash3(cell + 37.0)) - 0.5) * 0.6;
            float d = length(p - centre);
            float twinkle = 0.75 + 0.25 * sin(frame.params.z * (2.0 + h * 40.0) + h * 100.0);
            float brightness = (h - 0.9965) / 0.0035;
            sky += night * saturate(1.0 - d * 2.5) * brightness * twinkle * 0.6 * smoothstep(-0.05, 0.25, dir.y);
        }
    }
    return sky;
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
        radiance = SkyBackground(normalize(world.xyz / world.w - frame.cameraPosition.xyz));
    }
    return float4(ACESFilm(radiance * frame.params.x), 1.0);
}

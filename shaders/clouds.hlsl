// The sea of clouds far below the island: one large horizontal quad, shaded with procedural noise.
// Drawn in the main pass after the scene (depth tested, so the island hides it where it is in front),
// writing linear HDR radiance like mesh.hlsl.
#include "common.hlsli"

[[vk::combinedImageSampler]][[vk::binding(4, 0)]] TextureCube<float4> irradianceMap;  // see mesh.hlsl
[[vk::combinedImageSampler]][[vk::binding(4, 0)]] SamplerState irradianceSampler;

static const float kCloudHeight = -45.0;   // below the island's lowest rock (about -35)
static const float kCloudExtent = 1000.0;  // half size of the quad; the far plane is at 1000

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos : POSITION;
};

// Two triangles from the vertex index (no vertex buffer). The quad is centred under the camera, so
// it never ends; the noise is in world space, so moving the quad doesn't move the clouds.
VSOutput VSMain(uint id : SV_VertexID)
{
    const float2 corners[6] = {float2(-1, -1), float2(-1, 1), float2(1, -1),
                               float2(1, -1), float2(-1, 1), float2(1, 1)};
    float2 xz = corners[id] * kCloudExtent + frame.cameraPosition.xz;
    VSOutput output;
    output.worldPos = float3(xz.x, kCloudHeight, xz.y);
    output.position = mul(frame.viewProj, float4(output.worldPos, 1.0));
    return output;
}

// ---- Noise -----------------------------------------------------------------

float Hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash(i), b = Hash(i + float2(1, 0)), c = Hash(i + float2(0, 1)), d = Hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Cloud density is fractal noise: 5 octaves of value noise, each twice as fine and half as strong.
// Octaves drift at different speeds, so the clouds change shape as they move instead of sliding by
// as one rigid picture. Split in two so the shading can reuse the broad shape:
//   CloudShape:  octaves 1-3, the large billows (sum of amplitudes 0.875)
//   CloudDetail: octaves 4-5, small wisps. Only computed close up: octave 4 fades out towards
//                600 m, octave 5 towards 300 m. A faded octave counts as its average value (0.5),
//                so the density keeps the same mean and there is no ring where an octave stops.
static const float kOctaveNorm = 0.96875;  // 0.5 + 0.25 + ... + 1/32

float2 OctaveOffset(int i, float time)
{
    return time * float2(0.010, 0.004) * (1.0 + i * 0.6);
}

float CloudShape(float2 p, float time)
{
    float sum = 0.0, amplitude = 0.5;
    float2 q = p / 70.0;
    [unroll] for (int i = 0; i < 3; ++i)
    {
        sum += amplitude * ValueNoise(q + OctaveOffset(i, time));
        amplitude *= 0.5;
        q = q * 2.03 + float2(17.1, 9.3);
    }
    return sum;
}

float CloudDetail(float2 p, float time, float dist)
{
    float sum = 0.0, amplitude = 0.0625;
    float2 q = p / 70.0;
    [unroll] for (int k = 0; k < 3; ++k) q = q * 2.03 + float2(17.1, 9.3);  // the 4th octave's scale
    [unroll] for (int i = 3; i < 5; ++i)
    {
        float reach = 600.0 * exp2(3.0 - i);
        float weight = saturate((reach - dist) / (0.3 * reach));
        float n = 0.5;
        [branch] if (weight > 0.0)
            n = lerp(0.5, ValueNoise(q + OctaveOffset(i, time)), weight);
        sum += amplitude * n;
        amplitude *= 0.5;
        q = q * 2.03 + float2(17.1, 9.3);
    }
    return sum;
}

// ---- Shading ---------------------------------------------------------------

float4 PSMain(VSOutput input) : SV_Target0
{
    float time = frame.params.z;
    float3 toCamera = frame.cameraPosition.xyz - input.worldPos;
    float dist = length(toCamera);
    float3 viewDir = -toCamera / dist;

    float shape = CloudShape(input.worldPos.xz, time);
    float density = (shape + CloudDetail(input.worldPos.xz, time, dist)) / kOctaveNorm;
    float coverage = smoothstep(0.36, 0.60, density);

    // Cheap self-shadowing: compare with the density a little towards the sun. Where the cloud
    // gets thicker towards the sun, this point is in the shade of that thicker part.
    float2 towardSun = normalize(frame.sunDirection.xz + 1e-4) * 6.0;
    // The broad shape is enough for this (one more CloudShape sample, the detail is left out).
    float shade = saturate((CloudShape(input.worldPos.xz + towardSun, time) - shape) / kOctaveNorm * 6.0);

    float3 L = normalize(frame.sunDirection.xyz);
    float sunAmount = saturate(L.y) * (1.0 - 0.6 * shade);
    float3 albedo = float3(0.92, 0.93, 0.96);
    // The light from the sky above, like a diffuse surface facing up (the irradiance cube).
    float3 skyLight = irradianceMap.SampleLevel(irradianceSampler, float3(0, 1, 0), 0.0).rgb;
    // Thick parts are bright billows; thin edges let light through and read greyer.
    float thickness = smoothstep(0.40, 0.80, density);
    float3 lit = albedo * (frame.sunColor.rgb * sunAmount * lerp(0.30, 0.60, thickness) + skyLight);

    // Through the gaps: the hazy sky below.
    float3 below = SkyRadiance(viewDir);
    float3 color = lerp(below, lit, coverage);

    // Aerial perspective: far away the clouds melt into the sky colour of that direction, which is
    // exactly what the composite pass draws around them, so the horizon has no seam.
    float fog = 1.0 - exp(-dist * 0.0022);
    color = lerp(color, SkyRadiance(viewDir), fog);
    return float4(color, 1.0);
}

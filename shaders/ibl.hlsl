// Image-based lighting (IBL): turns the sky into the two cube maps the mesh shader lights with.
// Compute shaders, run whenever the sky changes (src/renderer/Renderer.cpp, recordIblUpdate):
//   CSSky:        the sky's radiance in every direction -> envCube (64 x 64 per face)
//   CSPrefilter:  envCube blurred by the GGX lobe of a roughness -> one mip of the specular cube
//                 (mip 0 = roughness 0, a mirror ... mip 4 = roughness 1)
//   CSIrradiance: envCube averaged over cosine-weighted hemispheres -> irradiance cube (16 x 16);
//                 what a diffuse surface facing each direction receives from the whole sky
#define COMMON_NO_OBJECT  // compute: no per-object push constant or instance helpers
#include "common.hlsli"

static const float PI = 3.14159265;

[[vk::combinedImageSampler]][[vk::binding(1, 0)]] TextureCube<float4> source;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState sourceSampler;
[[vk::image_format("rgba16f")]][[vk::binding(2, 0)]] RWTexture2DArray<float4> target;

struct IblParams
{
    uint size;        // of the target mip, in texels
    float roughness;  // CSPrefilter
    uint sampleCount;
    uint pad;
};
[[vk::push_constant]] IblParams params;

// Direction through the centre of texel `id` of cube face id.z, in Vulkan's cube map convention
// (faces +X, -X, +Y, -Y, +Z, -Z; u to the right, v down).
float3 CubeDirection(uint3 id)
{
    float2 uv = (float2(id.xy) + 0.5) / float(params.size) * 2.0 - 1.0;
    float3 d;
    switch (id.z)
    {
        case 0: d = float3(1.0, -uv.y, -uv.x); break;
        case 1: d = float3(-1.0, -uv.y, uv.x); break;
        case 2: d = float3(uv.x, 1.0, uv.y); break;
        case 3: d = float3(uv.x, -1.0, -uv.y); break;
        case 4: d = float3(uv.x, -uv.y, 1.0); break;
        default: d = float3(-uv.x, -uv.y, -1.0); break;
    }
    return normalize(d);
}

// Low-discrepancy points in the unit square (Hammersley): evenly spread, so few samples suffice.
float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}

// A frame (tangent, bitangent) around n.
void Basis(float3 n, out float3 t, out float3 b)
{
    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

[numthreads(8, 8, 1)]
void CSSky(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.size || id.y >= params.size) return;
    // No sun disk: the sun is the directional light, which mesh.hlsl already shades exactly.
    // Leaving the tiny, very bright disk out also keeps the blurring below free of sparkles.
    target[id] = float4(SkyRadiance(CubeDirection(id)), 1.0);
}

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.size || id.y >= params.size) return;
    float3 N = CubeDirection(id);
    if (params.roughness <= 0.0)
    {
        target[id] = source.SampleLevel(sourceSampler, N, 0.0);
        return;
    }
    // Split-sum approximation (Karis 2013): assume we look straight along the normal (V = N), and
    // average the sky over the directions L a GGX surface of this roughness reflects towards us.
    // The microfacet normals H are drawn with GGX's own distribution (importance sampling), so
    // each sample counts about the same and 64 of them give a smooth result.
    float a = params.roughness * params.roughness;
    float3 T, B;
    Basis(N, T, B);
    float3 sum = 0.0;
    float weight = 0.0;
    for (uint i = 0; i < params.sampleCount; ++i)
    {
        float2 xi = Hammersley(i, params.sampleCount);
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        float3 H = T * (cos(phi) * sinTheta) + B * (sin(phi) * sinTheta) + N * cosTheta;
        float3 L = reflect(-N, H);
        float NoL = dot(N, L);
        if (NoL > 0.0)
        {
            sum += source.SampleLevel(sourceSampler, L, 0.0).rgb * NoL;
            weight += NoL;
        }
    }
    target[id] = float4(sum / max(weight, 1e-4), 1.0);
}

[numthreads(8, 8, 1)]
void CSIrradiance(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.size || id.y >= params.size) return;
    float3 N = CubeDirection(id);
    float3 T, B;
    Basis(N, T, B);
    // Cosine-weighted directions over the hemisphere around N: their plain average is the
    // irradiance divided by pi, which times the albedo is the diffuse light that leaves the surface.
    float3 sum = 0.0;
    for (uint i = 0; i < params.sampleCount; ++i)
    {
        float2 xi = Hammersley(i, params.sampleCount);
        float phi = 2.0 * PI * xi.x;
        float r = sqrt(xi.y);
        float3 L = T * (cos(phi) * r) + B * (sin(phi) * r) + N * sqrt(1.0 - xi.y);
        sum += source.SampleLevel(sourceSampler, L, 0.0).rgb;
    }
    target[id] = float4(sum / float(params.sampleCount), 1.0);
}

// Physically based shading (metallic-roughness, GGX) for one directional sun light,
// with a PCF shadow map and image-based ambient light from the sky. Writes linear HDR radiance; exposure and
// tonemapping happen later in composite.hlsl.
#include "common.hlsli"

[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float> shadowMap;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerComparisonState shadowSampler;
// Image-based lighting, made from the sky by shaders/ibl.hlsl.
[[vk::combinedImageSampler]][[vk::binding(3, 0)]] TextureCube<float4> specularMap;  // mip = roughness
[[vk::combinedImageSampler]][[vk::binding(3, 0)]] SamplerState specularSampler;
[[vk::combinedImageSampler]][[vk::binding(4, 0)]] TextureCube<float4> irradianceMap;
[[vk::combinedImageSampler]][[vk::binding(4, 0)]] SamplerState irradianceSampler;

static const float PI = 3.14159265;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
    [[vk::location(3)]] float2 material : MATERIAL;   // x: roughness, y: metallic
};

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
    [[vk::location(3)]] float2 material : MATERIAL;
};

VSOutput VSMain(VSInput input, InstanceInput inst)
{
    float3 world = InstanceToWorld(input.position, inst);
    VSOutput output;
    output.position = mul(frame.viewProj, float4(world, 1.0));
    output.worldPos = world;
    output.normal = InstanceNormalToWorld(input.normal, inst);
    output.color = input.color * inst.params.y;
    output.material = input.material;
    return output;
}

// ---- BRDF (Cook-Torrance) ----------------------------------------------------

// Normal distribution: how many microfacets face the half vector.
float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Height-correlated Smith visibility, already divided by 4 * NoL * NoV.
float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (gv + gl);
}

float3 F_Schlick(float VoH, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
}

// ---- Shadows -----------------------------------------------------------------

// 3x3 PCF; each tap is itself a 2x2 hardware-filtered comparison. 1 = lit, 0 = in shadow.
float SunShadow(float3 worldPos)
{
    float4 clip = mul(frame.lightViewProj, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;   // no Y flip: the shadow pass uses the same convention
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
        return 1.0;                   // outside the shadow map: assume lit

    float texel = frame.params.y;
    float sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sum += shadowMap.SampleCmpLevelZero(shadowSampler, uv + float2(x, y) * texel, ndc.z);
    return sum / 9.0;
}

// The second half of the split-sum approximation: how much of the prefiltered environment a
// surface reflects, as f0 * scale + bias, depending on roughness and view angle. Usually a small
// lookup texture computed once; this is Karis's analytic fit of it ("Physically Based Shading on
// Mobile", 2014). At grazing angles it goes towards 1: everything turns into a mirror (Fresnel).
float3 EnvBRDFApprox(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 albedo = input.color;
    float roughness = clamp(input.material.x, 0.045, 1.0);
    float metallic = input.material.y;

    float3 N = normalize(input.normal);
    float3 V = normalize(frame.cameraPosition.xyz - input.worldPos);
    float3 L = normalize(frame.sunDirection.xyz);
    float3 H = normalize(V + L);
    float NoV = max(dot(N, V), 1e-4);
    float NoL = saturate(dot(N, L));
    float NoH = saturate(dot(N, H));
    float VoH = saturate(dot(V, H));

    // Metals have no diffuse and tint their reflection; dielectrics reflect ~4%.
    float3 f0 = lerp(0.04, albedo, metallic);
    float3 diffuseColor = albedo * (1.0 - metallic);

    float a = roughness * roughness;
    float3 F = F_Schlick(VoH, f0);
    float3 specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
    float3 diffuse = diffuseColor / PI;

    float shadow = NoL > 0.0 ? SunShadow(input.worldPos) : 0.0;
    float3 direct = (diffuse + specular) * frame.sunColor.rgb * NoL * shadow;

    // Image-based lighting: the light of the whole sky. Diffuse: the irradiance arriving around N.
    // Specular: the sky in the mirror direction R, blurred as much as the roughness blurs it.
    float3 R = reflect(-V, N);
    float3 irradiance = irradianceMap.SampleLevel(irradianceSampler, N, 0.0).rgb;
    float3 prefiltered = specularMap.SampleLevel(specularSampler, R, roughness * frame.ibl.y).rgb;
    // Reflections that point into the surface (possible with bumpy normals) see the inside of the
    // island, not the sky: fade them out.
    float horizon = saturate(1.0 + dot(R, N));
    // Nothing here knows that grass stands among grass or that a trunk is inside its tree, so the
    // diffuse part is turned down a little everywhere (ibl.x < 1) as a crude ambient occlusion.
    float3 ambient = irradiance * diffuseColor * frame.ibl.x
                   + prefiltered * EnvBRDFApprox(f0, roughness, NoV) * horizon * horizon * frame.ibl.z;

    // Alpha 1 marks geometry; the composite pass draws the sky where alpha is still 0.
    return float4(direct + ambient, 1.0);
}

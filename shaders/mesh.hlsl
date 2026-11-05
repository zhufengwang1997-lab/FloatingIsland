// Physically based shading (metallic-roughness, GGX) for one directional sun light,
// with a PCF shadow map and hemisphere ambient. Writes linear HDR radiance; exposure and
// tonemapping happen later in composite.hlsl.
#include "common.hlsli"

[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float> shadowMap;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerComparisonState shadowSampler;

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

float3 Hemisphere(float3 dir)
{
    float3 sky = float3(0.35, 0.50, 0.75);
    float3 ground = float3(0.18, 0.14, 0.10);
    return lerp(ground, sky, dir.y * 0.5 + 0.5);
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

    // Stand-in for image-based lighting until the sky gets a proper environment map.
    float3 R = reflect(-V, N);
    float3 ambient = Hemisphere(N) * diffuseColor * 0.6
                   + Hemisphere(R) * f0 * (1.0 - roughness * 0.7) * 0.6;

    // Alpha 1 marks geometry; the composite pass draws the sky where alpha is still 0.
    return float4(direct + ambient, 1.0);
}

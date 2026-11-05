// Lit, vertex-coloured mesh: one sun plus a sky/ground ambient term.

struct PushConstants
{
    float4x4 viewProj;      // GLM column-major matrix; HLSL's default column_major packing matches
    float4 sunDirection;    // xyz: unit vector pointing towards the sun
};
[[vk::push_constant]] PushConstants pc;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal : NORMAL;
    [[vk::location(1)]] float3 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = mul(pc.viewProj, float4(input.position, 1.0));
    output.normal = input.normal;
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    float3 n = normalize(input.normal);
    float3 l = normalize(pc.sunDirection.xyz);

    float diffuse = saturate(dot(n, l));
    float3 sunColor = float3(1.0, 0.95, 0.85) * 1.2;

    // Hemisphere ambient: surfaces facing up see the sky, facing down see bounce light.
    float3 skyAmbient = float3(0.35, 0.50, 0.75);
    float3 groundAmbient = float3(0.18, 0.14, 0.10);
    float3 ambient = lerp(groundAmbient, skyAmbient, n.y * 0.5 + 0.5) * 0.4;

    return float4(input.color * (ambient + sunColor * diffuse), 1.0);
}

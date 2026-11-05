// First triangle: positions and colors are hard-coded, so no vertex buffer is needed.
// Vulkan clip space has +Y pointing down, so y = -0.5 is the top of the screen.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
};

static const float2 kPositions[3] =
{
    float2( 0.0, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5),
};

static const float3 kColors[3] =
{
    float3(1.0, 0.3, 0.2),
    float3(0.2, 0.9, 0.4),
    float3(0.2, 0.4, 1.0),
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.position = float4(kPositions[vertexId], 0.0, 1.0);
    output.color = kColors[vertexId];
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}

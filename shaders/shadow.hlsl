// Depth-only pass from the sun's point of view. No pixel shader: only depth is written.
#include "common.hlsli"

float4 VSMain([[vk::location(0)]] float3 position : POSITION) : SV_Position
{
    return mul(frame.lightViewProj, mul(object.model, float4(position, 1.0)));
}

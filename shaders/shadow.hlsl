// Depth-only pass from the sun's point of view. No pixel shader: only depth is written.
#include "common.hlsli"

float4 VSMain([[vk::location(0)]] float3 position : POSITION, InstanceInput inst) : SV_Position
{
    return mul(frame.lightViewProj, float4(InstanceToWorld(position, inst), 1.0));
}

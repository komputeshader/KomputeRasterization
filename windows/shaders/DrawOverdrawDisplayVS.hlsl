float4 main(uint vertexID : SV_VertexID) : SV_POSITION
{
	float2 position = float2((vertexID << 1) & 2, vertexID & 2);
	return float4(
		position * float2(2.0, -2.0) + float2(-1.0, 1.0),
		0.0,
		1.0);
}

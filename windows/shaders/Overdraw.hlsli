float3 OverdrawColor(uint count)
{
	if (count == 0)
	{
		return 0.0;
	}

	float level = log2((float)count);
	if (level < 1.0)
	{
		return lerp(float3(0.0, 0.1, 0.8), float3(0.0, 0.8, 1.0), level);
	}
	if (level < 2.0)
	{
		return lerp(float3(0.0, 0.8, 1.0), float3(0.0, 1.0, 0.0), level - 1.0);
	}
	if (level < 3.0)
	{
		return lerp(float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), level - 2.0);
	}
	if (level < 4.0)
	{
		return lerp(float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0), level - 3.0);
	}
	if (level < 5.0)
	{
		return lerp(float3(1.0, 0.0, 0.0), 1.0, level - 4.0);
	}

	return 1.0;
}

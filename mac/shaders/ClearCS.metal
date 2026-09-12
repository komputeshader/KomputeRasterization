#include "TypesAndConstants.metal"

kernel void ClearUIntBuffer(
	device uint* buffer [[buffer(0)]],
	constant uint& count [[buffer(1)]],
	uint index [[thread_position_in_grid]])
{
	if (index < count)
	{
		buffer[index] = 0;
	}
}

kernel void ClearColorTexture(
	texture2d<float, access::write> texture [[texture(0)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (all(pixel < uint2(texture.get_width(), texture.get_height())))
	{
		texture.write(SkyColor, pixel);
	}
}

kernel void ClearStatistics(
	device uint* statistics [[buffer(0)]],
	uint index [[thread_position_in_grid]])
{
	if (index < 2)
	{
		statistics[index] = 0;
	}
}

kernel void ResetDispatchArguments(
	device DispatchArguments& arguments [[buffer(0)]])
{
	arguments.x = 0;
	arguments.y = 1;
	arguments.z = 1;
}


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
	device atomic_uint* statistics [[buffer(0)]],
	uint index [[thread_position_in_grid]])
{
	if (index < 2)
	{
		atomic_store_explicit(&statistics[index], 0, memory_order_relaxed);
	}
}

kernel void ResetDispatchArguments(
	device DispatchArguments& arguments [[buffer(0)]])
{
	atomic_store_explicit(&arguments.x, 0, memory_order_relaxed);
	arguments.y = 1;
	arguments.z = 1;
	atomic_store_explicit(&arguments.overflow, 0, memory_order_relaxed);
}


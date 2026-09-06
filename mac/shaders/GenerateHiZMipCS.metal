#include "TypesAndConstants.metal"

kernel void CopyDepthArray(
	depth2d_array<float> source [[texture(0)]],
	texture2d_array<float, access::write> destination [[texture(1)]],
	uint3 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= destination.get_width() || pixel.y >= destination.get_height() ||
		pixel.z >= destination.get_array_size())
	{
		return;
	}

	constexpr sampler pointSampler(coord::pixel, filter::nearest);

	destination.write(source.sample(pointSampler, float2(pixel.xy) + 0.5f, pixel.z), pixel.xy, pixel.z, 0);
}

kernel void CopyFloatArray(
	device const uint* source [[buffer(0)]],
	texture2d_array<float, access::write> destination [[texture(1)]],
	uint3 pixel [[thread_position_in_grid]])
{
	if (pixel.x < destination.get_width() && pixel.y < destination.get_height() &&
		pixel.z < destination.get_array_size())
	{
		const uint index = pixel.z * destination.get_width() * destination.get_height() +
			pixel.y * destination.get_width() + pixel.x;

		destination.write(as_type<float>(source[index]), pixel.xy, pixel.z, 0);
	}
}

kernel void GenerateHiZMipArray(
	constant uint2& levels [[buffer(0)]],
	texture2d_array<float, access::read_write> hierarchy [[texture(1)]],
	uint3 pixel [[thread_position_in_grid]])
{
	const uint2 size = uint2(hierarchy.get_width(levels.y), hierarchy.get_height(levels.y));

	if (any(pixel.xy >= size) || pixel.z >= hierarchy.get_array_size())
	{
		return;
	}

	const uint2 p = pixel.xy * 2;
	const uint2 sourceSize = uint2(hierarchy.get_width(levels.x), hierarchy.get_height(levels.x));

	const float a = hierarchy.read(min(p, sourceSize - 1), pixel.z, levels.x).r;
	const float b = hierarchy.read(min(p + uint2(1, 0), sourceSize - 1), pixel.z, levels.x).r;
	const float c = hierarchy.read(min(p + uint2(0, 1), sourceSize - 1), pixel.z, levels.x).r;
	const float d = hierarchy.read(min(p + uint2(1, 1), sourceSize - 1), pixel.z, levels.x).r;

	float result = min(min(a, b), min(c, d));

	const bool borderX = pixel.x == size.x - 1 && (sourceSize.x & 1);
	const bool borderY = pixel.y == size.y - 1 && (sourceSize.y & 1);

	if (borderX)
	{
		result = min(result, hierarchy.read(uint2(p.x + 2, p.y), pixel.z, levels.x).r);
		result = min(result, hierarchy.read(uint2(p.x + 2, min(p.y + 1, sourceSize.y - 1)), pixel.z, levels.x).r);
	}

	if (borderY)
	{
		result = min(result, hierarchy.read(uint2(p.x, p.y + 2), pixel.z, levels.x).r);
		result = min(result, hierarchy.read(uint2(min(p.x + 1, sourceSize.x - 1), p.y + 2), pixel.z, levels.x).r);
	}

	if (borderX && borderY)
	{
		result = min(result, hierarchy.read(uint2(p.x + 2, p.y + 2), pixel.z, levels.x).r);
	}

	hierarchy.write(result, pixel.xy, pixel.z, levels.y);
}

kernel void CopyDepth(
	depth2d<float> source [[texture(0)]],
	texture2d<float, access::write> destination [[texture(1)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (any(pixel >= uint2(destination.get_width(), destination.get_height())))
	{
		return;
	}

	constexpr sampler pointSampler(coord::pixel, filter::nearest);

	destination.write(source.sample(pointSampler, float2(pixel) + 0.5f), pixel, 0);
}

kernel void CopyFloat(
	device const uint* source [[buffer(0)]],
	texture2d<float, access::write> destination [[texture(1)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (all(pixel < uint2(destination.get_width(), destination.get_height())))
	{
		destination.write(
			as_type<float>(source[pixel.y * destination.get_width() + pixel.x]),
			pixel,
			0);
	}
}

kernel void GenerateHiZMip(
	constant uint2& levels [[buffer(0)]],
	texture2d<float, access::read_write> hierarchy [[texture(1)]],
	uint2 pixel [[thread_position_in_grid]])
{
	const uint2 size = uint2(hierarchy.get_width(levels.y), hierarchy.get_height(levels.y));

	if (any(pixel >= size))
	{
		return;
	}

	const uint2 p = pixel * 2;
	const uint2 sourceSize = uint2(hierarchy.get_width(levels.x), hierarchy.get_height(levels.x));

	const float a = hierarchy.read(min(p, sourceSize - 1), levels.x).r;
	const float b = hierarchy.read(min(p + uint2(1, 0), sourceSize - 1), levels.x).r;
	const float c = hierarchy.read(min(p + uint2(0, 1), sourceSize - 1), levels.x).r;
	const float d = hierarchy.read(min(p + uint2(1, 1), sourceSize - 1), levels.x).r;

	float result = min(min(a, b), min(c, d));

	const bool borderX = pixel.x == size.x - 1 && (sourceSize.x & 1);
	const bool borderY = pixel.y == size.y - 1 && (sourceSize.y & 1);

	if (borderX)
	{
		result = min(result, hierarchy.read(uint2(p.x + 2, p.y), levels.x).r);
		result = min(result, hierarchy.read(uint2(p.x + 2, min(p.y + 1, sourceSize.y - 1)), levels.x).r);
	}

	if (borderY)
	{
		result = min(result, hierarchy.read(uint2(p.x, p.y + 2), levels.x).r);
		result = min(result, hierarchy.read(uint2(min(p.x + 1, sourceSize.x - 1), p.y + 2), levels.x).r);
	}

	if (borderX && borderY)
	{
		result = min(result, hierarchy.read(uint2(p.x + 2, p.y + 2), levels.x).r);
	}

	hierarchy.write(result, pixel, levels.y);
}

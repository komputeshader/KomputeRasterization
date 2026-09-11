#ifndef RASTERIZATION_METAL
#define RASTERIZATION_METAL

#include "CullingCommon.metal"

static inline float Area(float2 v0, float2 v1, float2 v2)
{
	float2 e0 = v1 - v0;
	float2 e1 = v2 - v0;
	return e0.x * e1.y - e1.x * e0.y;
}

static inline void EdgeFunction(
	float2 v0,
	float2 v1,
	float2 p,
	thread float& area,
	thread float2& dxdy)
{
	float2 e0 = v1 - v0;
	float2 e1 = p - v0;
	area = e0.x * e1.y - e1.x * e0.y;
	dxdy = e0;
}

// https://userpages.cs.umbc.edu/olano/papers/2dh-tri/
// form edge equations from clip-space xyw without dividing the vertices by w
static inline void EdgeFunctionHomogeneous(
	float4 v0CS,
	float4 v1CS,
	float2 pNDC,
	thread float& area,
	thread float2& dxdy)
{
	// screen y points down
	// reverse the cross product to keep the inside positive
	float3 edge = cross(v1CS.xyw, v0CS.xyw);
	float value = dot(edge, float3(pNDC, 1.0f));
	area = value;
	// E(x + a, y + b) = E(x, y) - a * dy + b * dx with offsets in NDC
	dxdy = float2(edge.y, -edge.x);
}

// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules#triangle-rasterization-rules-without-multisampling
// Any pixel center which falls inside a triangle is drawn; a pixel is assumed
// to be inside if it passes the top-left rule. The top-left rule is that a pixel
// center is defined to lie inside of a triangle if it lies on the top edge or the
// left edge of a triangle.
//
// Where:
//
// - A top edge, is an edge that is exactly horizontal and is above the other edges.
//
// - A left edge, is an edge that is not exactly horizontal and is on the left side
// of thetriangle. A triangle can have one or two left edges. The top-left rule
// ensures that adjacent triangles are drawn once.
static inline bool EdgeIsTopLeft(float2 v0, float2 v1)
{
	float2 e = v1 - v0;
	bool top = e.y == 0.0f && e.x > 0.0f;
	bool left = e.y < 0.0f;
	return top || left;
}

// a version for homogeneous edges
static inline bool EdgeIsTopLeft(float2 dxdy)
{
	// for positive vertex w, dxdy.y has the sign of screen dy, and dxdy.x the opposite sign of screen dx
	bool top = dxdy.y == 0.0f && dxdy.x < 0.0f;
	bool left = dxdy.y < 0.0f;
	return top || left;
}

static inline float EdgeScanlineIntersection(float2 v0, float2 v1, float y)
{
	float denom = v1.y - v0.y;
	return ((denom == 0.0f) ? FloatMax : (y - v0.y) / denom);
}

static inline void ClampScanline(
	float minX,
	float maxX,
	thread float& xMin,
	thread float& xMax)
{
	xMin = max(xMin, minX);
	xMax = min(xMax, maxX);

	// snap min x bound to pixel center
	xMin = ceil(xMin - 0.5f) + 0.5f;

	// top-left rule
	xMax += ((fract(xMax) == 0.5f) ? -1.0f : 0.0f);
}

static inline float4 EdgeNearPlaneIntersection(
	float3 v0CS,
	float3 v1CS,
	float cameraNear)
{
	float3 edge = v1CS - v0CS;
	float t = (cameraNear - v0CS.z) / edge.z;
	return float4(v0CS.xy + t * edge.xy, cameraNear, cameraNear);
}

static inline float4 EdgeNearPlaneIntersection(
	float3 v0CS,
	float3 v1CS,
	float cameraNear,
	thread float& t)
{
	float3 edge = v1CS - v0CS;
	t = (cameraNear - v0CS.z) / edge.z;
	return float4(v0CS.xy + t * edge.xy, cameraNear, cameraNear);
}

static inline void GetTriangleIndices(
	device const uint* indices,
	uint totalTriangles,
	uint startIndexLocation,
	thread uint& i0,
	thread uint& i1,
	thread uint& i2)
{
	startIndexLocation /= INDICES_STRIDE;
	i0 = indices[0 * totalTriangles + startIndexLocation];
	i1 = indices[1 * totalTriangles + startIndexLocation];
	i2 = indices[2 * totalTriangles + startIndexLocation];
}

static inline void GetTriangleVertexPositions(
	device const VertexPosition* positions,
	uint i0, uint i1, uint i2,
	uint baseVertexLocation,
	thread float3& p0,
	thread float3& p1,
	thread float3& p2)
{
	p0 = float3(positions[baseVertexLocation + i0].position);
	p1 = float3(positions[baseVertexLocation + i1].position);
	p2 = float3(positions[baseVertexLocation + i2].position);
}

static inline void GetPackedVertexNormals(
	device const VertexNormal* normals,
	uint i0, uint i1, uint i2,
	uint baseVertexLocation,
	thread VertexNormal& n0,
	thread VertexNormal& n1,
	thread VertexNormal& n2)
{
	n0 = normals[baseVertexLocation + i0];
	n1 = normals[baseVertexLocation + i1];
	n2 = normals[baseVertexLocation + i2];
}

static inline void GetPackedVertexColors(
	device const VertexColor* colors,
	uint i0, uint i1, uint i2,
	uint baseVertexLocation,
	thread VertexColor& c0,
	thread VertexColor& c1,
	thread VertexColor& c2)
{
	c0 = colors[baseVertexLocation + i0];
	c1 = colors[baseVertexLocation + i1];
	c2 = colors[baseVertexLocation + i2];
}

static inline void GetPackedVertexUVs(
	device const VertexUV* texcoords,
	uint i0, uint i1, uint i2,
	uint baseVertexLocation,
	thread VertexUV& UV0,
	thread VertexUV& UV1,
	thread VertexUV& UV2)
{
	UV0 = texcoords[baseVertexLocation + i0];
	UV1 = texcoords[baseVertexLocation + i1];
	UV2 = texcoords[baseVertexLocation + i2];
}

static inline void GetCSPositions(
	Instance instance,
	float3 p0,
	float3 p1,
	float3 p2,
	float4x4 vp,
	thread float3& p0WS,
	thread float3& p1WS,
	thread float3& p2WS,
	thread float4& p0CS,
	thread float4& p1CS,
	thread float4& p2CS)
{
	// MS -> WS
	p0WS = (instance.worldTransform * float4(p0, 1.0f)).xyz;
	p1WS = (instance.worldTransform * float4(p1, 1.0f)).xyz;
	p2WS = (instance.worldTransform * float4(p2, 1.0f)).xyz;

	// WS -> VS -> CS
	p0CS = (vp * float4(p0WS, 1.0f));
	p1CS = (vp * float4(p1WS, 1.0f));
	p2CS = (vp * float4(p2WS, 1.0f));
}

static inline void GetSSPositions(
	float2 p0CS, float2 p1CS, float2 p2CS,
	float invW0, float invW1, float invW2,
	float2 outputResolution,
	thread float2& p0SS,
	thread float2& p1SS,
	thread float2& p2SS)
{
	// CS -> NDC -> DX [0,1] -> SS
	p0SS = (p0CS * invW0 * float2(0.5f, -0.5f) + float2(0.5f, 0.5f)) * outputResolution;
	p1SS = (p1CS * invW1 * float2(0.5f, -0.5f) + float2(0.5f, 0.5f)) * outputResolution;
	p2SS = (p2CS * invW2 * float2(0.5f, -0.5f) + float2(0.5f, 0.5f)) * outputResolution;
}

static inline void ClampToScreenBounds(
	thread float3& minP,
	thread float3& maxP,
	float2 outputResolution)
{
	minP.xy = clamp(minP.xy, float2(0.0f, 0.0f), outputResolution);
	maxP.xy = clamp(maxP.xy, float2(0.0f, 0.0f), outputResolution);
}

static inline float2 SnapMinBoundToPixelCenter(float2 minP)
{
	return ceil(minP - float2(0.5f, 0.5f)) + float2(0.5f, 0.5f);
}

static inline bool TriangleVsHiZ(
	float2 minP,
	float2 maxP,
	float maximumDepth,
	float2 inverseOutputResolution,
	texture2d<float> hierarchy)
{
	const float2 dimensions = maxP - minP;
	const float mipLevel = ceil(log2(0.5f * max(dimensions.x, dimensions.y)));
	const float tileDepth = SampleDepth(
		hierarchy,
		(minP + maxP) * 0.5f * inverseOutputResolution,
		mipLevel);

	return !(tileDepth > maximumDepth);
}

static inline bool TriangleVsHiZ(
	float2 minP,
	float2 maxP,
	float maximumDepth,
	float2 inverseOutputResolution,
	texture2d_array<float> hierarchy,
	uint slice)
{
	const float2 dimensions = maxP - minP;
	const float mipLevel = ceil(log2(0.5f * max(dimensions.x, dimensions.y)));
	const float tileDepth = SampleDepth(
		hierarchy,
		(minP + maxP) * 0.5f * inverseOutputResolution,
		slice,
		mipLevel);

	return !(tileDepth > maximumDepth);
}

#endif // RASTERIZATION_METAL

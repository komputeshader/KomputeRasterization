#ifndef RASTERIZATION_HLSL
#define RASTERIZATION_HLSL

float Area(in float2 v0, in float2 v1, in float2 v2)
{
	float2 e0 = v1 - v0;
	float2 e1 = v2 - v0;
	return e0.x * e1.y - e1.x * e0.y;
}

void EdgeFunction(
	in float2 v0,
	in float2 v1,
	in float2 p,
	out float area,
	out float2 dxdy)
{
	float2 e0 = v1 - v0;
	float2 e1 = p - v0;
	area = e0.x * e1.y - e1.x * e0.y;
	dxdy = e0;
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
bool EdgeIsTopLeft(in float2 v0, in float2 v1)
{
	float2 e = v1 - v0;
	bool top = e.y == 0.0 && e.x > 0.0;
	bool left = e.y < 0.0;
	return top || left;
}

float EdgeScanlineIntersection(in float2 v0, in float2 v1, in float y)
{
	float denom = v1.y - v0.y;
	return ((denom == 0.0) ? FloatMax : (y - v0.y) * rcp(denom));
}

void GetTriangleIndices(
	in uint startIndexLocation,
	out uint i0,
	out uint i1,
	out uint i2)
{
	i0 = Indices[startIndexLocation + 0];
	i1 = Indices[startIndexLocation + 1];
	i2 = Indices[startIndexLocation + 2];
}

void GetTriangleVertexPositions(
	in uint i0, in uint i1, in uint i2,
	in uint baseVertexLocation,
	out float3 p0,
	out float3 p1,
	out float3 p2)
{
	p0 = Positions[baseVertexLocation + i0].position;
	p1 = Positions[baseVertexLocation + i1].position;
	p2 = Positions[baseVertexLocation + i2].position;
}

#ifdef OPAQUE

void GetTriangleVertexNormals(
	in uint i0, in uint i1, in uint i2,
	in uint baseVertexLocation,
	out float3 n0,
	out float3 n1,
	out float3 n2)
{
	n0 = UnpackNormal(Normals[baseVertexLocation + i0]);
	n1 = UnpackNormal(Normals[baseVertexLocation + i1]);
	n2 = UnpackNormal(Normals[baseVertexLocation + i2]);
}

void GetTriangleVertexColors(
	in uint i0, in uint i1, in uint i2,
	in uint baseVertexLocation,
	out float4 c0,
	out float4 c1,
	out float4 c2)
{
	c0 = UnpackColor(Colors[baseVertexLocation + i0]);
	c1 = UnpackColor(Colors[baseVertexLocation + i1]);
	c2 = UnpackColor(Colors[baseVertexLocation + i2]);
}

void GetTriangleVertexUVs(
	in uint i0, in uint i1, in uint i2,
	in uint baseVertexLocation,
	out float2 UV0,
	out float2 UV1,
	out float2 UV2)
{
	UV0 = UnpackTexcoords(UVs[baseVertexLocation + i0]);
	UV1 = UnpackTexcoords(UVs[baseVertexLocation + i1]);
	UV2 = UnpackTexcoords(UVs[baseVertexLocation + i2]);
}

#endif // OPAQUE

void GetCSPositions(
	in Instance instance,
	in float3 p0,
	in float3 p1,
	in float3 p2,
	out float3 p0WS,
	out float3 p1WS,
	out float3 p2WS,
	out float4 p0CS,
	out float4 p1CS,
	out float4 p2CS)
{
	// MS -> WS
	p0WS = mul(instance.worldTransform, float4(p0, 1.0)).xyz;
	p1WS = mul(instance.worldTransform, float4(p1, 1.0)).xyz;
	p2WS = mul(instance.worldTransform, float4(p2, 1.0)).xyz;

	// WS -> VS -> CS
	p0CS = mul(VP, float4(p0WS, 1.0));
	p1CS = mul(VP, float4(p1WS, 1.0));
	p2CS = mul(VP, float4(p2WS, 1.0));
}

void GetSSPositions(
	in float2 p0CS, in float2 p1CS, in float2 p2CS,
	in float invW0, in float invW1, in float invW2,
	out float2 p0SS,
	out float2 p1SS,
	out float2 p2SS)
{
	// CS -> NDC
	float2 p0NDC = p0CS * invW0;
	float2 p1NDC = p1CS * invW1;
	float2 p2NDC = p2CS * invW2;

	// NDC -> DX [0,1]
	p0SS = p0NDC * float2(0.5, -0.5) + float2(0.5, 0.5);
	p1SS = p1NDC * float2(0.5, -0.5) + float2(0.5, 0.5);
	p2SS = p2NDC * float2(0.5, -0.5) + float2(0.5, 0.5);

	// DX [0,1] -> SS
	p0SS *= OutputRes;
	p1SS *= OutputRes;
	p2SS *= OutputRes;
}

void ClampToScreenBounds(
	inout float2 minP,
	inout float2 maxP)
{
	minP = clamp(minP, float2(0.0, 0.0), OutputRes);
	maxP = clamp(maxP, float2(0.0, 0.0), OutputRes);
}

float2 SnapMinBoundToPixelCenter(in float2 minP)
{
	return ceil(minP - float2(0.5, 0.5)) + float2(0.5, 0.5);
}

#endif // RASTERIZATION_HLSL
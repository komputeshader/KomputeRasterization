#include "TypesAndConstants.hlsli"

static const uint ThreadsX = 256;

struct Frustum
{
	float4 left;
	float4 right;
	float4 bottom;
	float4 top;
	float4 near;
	float4 far;
};

struct AABB
{
	float3 center;
	float pad0;
	float3 extents;
	float pad1;
};

struct MeshInfo
{
	AABB aabb;

	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;

	uint3 pad;
};

struct DrawIndexedArguments
{
	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;
};

struct IndirectCommand
{
	DrawIndexedArguments arguments;
};

cbuffer ConstantBuffer : register(b0)
{
	uint TotalMeshesCount;
	uint CascadeCount;
	float2 pad0;
	Frustum Camera;
	Frustum Cascade[MaxCascadeCount];
};

StructuredBuffer<MeshInfo> Info : register(t0);

AppendStructuredBuffer<IndirectCommand> CameraDrawBuffer : register(u0);
AppendStructuredBuffer<IndirectCommand> Cascade0DrawBuffer : register(u1);
AppendStructuredBuffer<IndirectCommand> Cascade1DrawBuffer : register(u2);
AppendStructuredBuffer<IndirectCommand> Cascade2DrawBuffer : register(u3);
AppendStructuredBuffer<IndirectCommand> Cascade3DrawBuffer : register(u4);

bool AABBVsPlane(AABB box, float4 p)
{
	float r = dot(box.extents, abs(p.xyz));
	float s = dot(p.xyz, box.center) + p.w;
	return s >= -r;
}

bool AABBVsFrustum(AABB box, Frustum f)
{
	return
		AABBVsPlane(box, f.left) &&
		AABBVsPlane(box, f.right) &&
		AABBVsPlane(box, f.bottom) &&
		AABBVsPlane(box, f.top) &&
		AABBVsPlane(box, f.near) &&
		AABBVsPlane(box, f.far);
}

[numthreads(ThreadsX, 1, 1)]
void CSMain(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (dispatchThreadID.x < TotalMeshesCount)
	{
		MeshInfo info = Info[dispatchThreadID.x];

		IndirectCommand result;
		result.arguments.indexCountPerInstance = info.indexCountPerInstance;
		result.arguments.instanceCount = 1;
		result.arguments.startIndexLocation = info.startIndexLocation;
		result.arguments.baseVertexLocation = info.baseVertexLocation;
		result.arguments.startInstanceLocation = 0;

		if (AABBVsFrustum(info.aabb, Camera)) CameraDrawBuffer.Append(result);
		if (AABBVsFrustum(info.aabb, Cascade[0])) Cascade0DrawBuffer.Append(result);
		if (AABBVsFrustum(info.aabb, Cascade[1])) Cascade1DrawBuffer.Append(result);
		if (AABBVsFrustum(info.aabb, Cascade[2])) Cascade2DrawBuffer.Append(result);
		if (AABBVsFrustum(info.aabb, Cascade[3])) Cascade3DrawBuffer.Append(result);
	}
}
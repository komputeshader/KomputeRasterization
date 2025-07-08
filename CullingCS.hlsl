#include "CullingCommon.hlsli"

StructuredBuffer<MeshMeta> MeshesMeta : register(t0);
StructuredBuffer<Instance> Instances : register(t1);

Texture2D PrevFrameDepth : register(t2);
Texture2D CascadeShadowMap[MAX_CASCADES_COUNT] : register(t3);

SamplerState DepthSampler : register(s0);

RWStructuredBuffer<Instance> VisibleInstances : register(u0);
RWStructuredBuffer<uint> InstanceCounters : register(u1);

bool FrustumVsAABB(Frustum f, AABB box)
{
	float3 pMax = box.center + box.extents;
	float3 pMin = box.center - box.extents;

	uint sameSideCornersXMin = 0;
	uint sameSideCornersXMax = 0;
	uint sameSideCornersYMin = 0;
	uint sameSideCornersYMax = 0;
	uint sameSideCornersZMin = 0;
	uint sameSideCornersZMax = 0;
	[unroll]
	for (uint i = 0; i < 8; i++)
	{
		sameSideCornersXMin += (f.corners[i].x < pMin.x) ? 1 : 0;
		sameSideCornersXMax += (f.corners[i].x > pMax.x) ? 1 : 0;
		sameSideCornersYMin += (f.corners[i].y < pMin.y) ? 1 : 0;
		sameSideCornersYMax += (f.corners[i].y > pMax.y) ? 1 : 0;
		sameSideCornersZMin += (f.corners[i].z < pMin.z) ? 1 : 0;
		sameSideCornersZMax += (f.corners[i].z > pMax.z) ? 1 : 0;
	}

	return
		!(sameSideCornersXMin == 8
		|| sameSideCornersXMax == 8
		|| sameSideCornersYMin == 8
		|| sameSideCornersYMax == 8
		|| sameSideCornersZMin == 8
		|| sameSideCornersZMax == 8);
}

bool AABBVsPlane(AABB box, float4 plane)
{
	float r = dot(box.extents, abs(plane.xyz));
	float s = dot(plane.xyz, box.center) + plane.w;
	return r + s >= 0.0;
}

bool AABBVsFrustum(AABB box, Frustum frustum)
{
	bool largeAABBTest = FrustumVsAABB(frustum, box);
	bool l = AABBVsPlane(box, frustum.left);
	bool r = AABBVsPlane(box, frustum.right);
	bool b = AABBVsPlane(box, frustum.bottom);
	bool t = AABBVsPlane(box, frustum.top);
	bool n = AABBVsPlane(box, frustum.near);
	bool f = AABBVsPlane(box, frustum.far);

	return l && r && b && t && n && f && largeAABBTest;
}

bool AABBVsHiZ(
	in AABB box,
	in float4x4 VP,
	in float2 HiZResolution,
	in Texture2D HiZ)
{
	float3 boxCorners[8] =
	{
		box.center + box.extents * float3(1.0, 1.0, 1.0),
		box.center + box.extents * float3(1.0, 1.0, -1.0),
		box.center + box.extents * float3(1.0, -1.0, 1.0),
		box.center + box.extents * float3(1.0, -1.0, -1.0),
		box.center + box.extents * float3(-1.0, 1.0, 1.0),
		box.center + box.extents * float3(-1.0, 1.0, -1.0),
		box.center + box.extents * float3(-1.0, -1.0, 1.0),
		box.center + box.extents * float3(-1.0, -1.0, -1.0)
	};

	float3 minP = FloatMax.xxx;
	float3 maxP = -FloatMax.xxx;
	[unroll]
	for (uint corner = 0; corner < 8; corner++)
	{
		float4 cornerNDC = mul(
			VP,
			float4(boxCorners[corner], 1.0));
		cornerNDC.xyz /= cornerNDC.w;

		minP = min(minP, cornerNDC.xyz);
		maxP = max(maxP, cornerNDC.xyz);
	}
	// NDC -> DX [0,1]
	minP.xy = minP.xy * float2(0.5, -0.5) + float2(0.5, 0.5);
	maxP.xy = maxP.xy * float2(0.5, -0.5) + float2(0.5, 0.5);

	float mipLevel = ceil(log2(0.5 * max(
		(maxP.x - minP.x) * HiZResolution.x,
		(maxP.y - minP.y) * HiZResolution.y)));
	float tileDepth = HiZ.SampleLevel(
		DepthSampler,
		(minP.xy + maxP.xy) * 0.5,
		mipLevel).r;

	return !(tileDepth > maxP.z);
}

bool BackfacingMeshlet(
	in float3 cameraPosition,
	in float3 coneApex,
	in float3 coneAxis,
	in float coneCutoff)
{
	return dot(normalize(coneApex - cameraPosition), coneAxis) >= coneCutoff;
}

bool BackfacingMeshletOrthographic(float3 coneAxis, float coneCutoff)
{
	return dot(-LightDirection.xyz, coneAxis) >= coneCutoff;
}

[numthreads(CULLING_THREADS_X, CULLING_THREADS_Y, CULLING_THREADS_Z)]
void main(
	uint3 groupID : SV_GroupID,
	uint3 dispatchThreadID : SV_DispatchThreadID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex)
{
	if (dispatchThreadID.x >= TotalInstancesCount)
	{
		return;
	}

	Instance instance = Instances[dispatchThreadID.x];
	MeshMeta meshMeta = MeshesMeta[instance.meshID];
	meshMeta.aabb = TransformAABB(meshMeta.aabb, instance.worldTransform);
	// TODO: cone axis should be rotated properly
	meshMeta.coneApex = mul(instance.worldTransform, float4(meshMeta.coneApex, 1.0)).xyz;

	uint writeIndex = meshMeta.startInstanceLocation;

	bool cameraBackface = BackfacingMeshlet(
		CameraPosition.xyz,
		meshMeta.coneApex,
		meshMeta.coneAxis,
		meshMeta.coneCutoff);
	bool cameraFC = AABBVsFrustum(meshMeta.aabb, Camera);
	if ((!cameraBackface || !ClusterBackfaceCullingEnabled)
		&& (cameraFC || !FrustumCullingEnabled))
	{
		bool cameraHiZC = AABBVsHiZ(
			meshMeta.aabb,
			PrevFrameCameraVP,
			DepthResolution,
			PrevFrameDepth);
		if (cameraHiZC || !CameraHiZCullingEnabled)
		{
			uint writeOffset;
			InterlockedAdd(InstanceCounters[0 * MaxSceneMeshesMetaCount + instance.meshID], 1, writeOffset);

			VisibleInstances[0 * MaxSceneInstancesCount + writeIndex + writeOffset] = instance;
		}
	}

	[unroll(MAX_CASCADES_COUNT)]
	for (int cascade = 0; cascade < CascadesCount; cascade++)
	{
		bool backfacing = BackfacingMeshletOrthographic(meshMeta.coneAxis, meshMeta.coneCutoff);
		bool notFrustumCulled = AABBVsFrustum(meshMeta.aabb, Cascade[cascade]);
		if ((!backfacing || !ClusterBackfaceCullingEnabled)
			&& (notFrustumCulled || !FrustumCullingEnabled))
		{
			bool HiZ = AABBVsHiZ(
				meshMeta.aabb,
				PrevFrameCascadeVP[cascade],
				ShadowMapResolution,
				CascadeShadowMap[cascade]);
			if (HiZ || !ShadowsHiZCullingEnabled)
			{
				uint writeOffset;
				InterlockedAdd(InstanceCounters[(cascade + 1) * MaxSceneMeshesMetaCount + instance.meshID], 1, writeOffset);

				VisibleInstances[(cascade + 1) * MaxSceneInstancesCount + writeIndex + writeOffset] = instance;
			}
		}
	}
}
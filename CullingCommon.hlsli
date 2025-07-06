#ifndef CULLING_COMMON_HLSL
#define CULLING_COMMON_HLSL

#include "Common.hlsli"

cbuffer CullingCB : register(b0)
{
	uint MaxSceneInstancesCount;
	uint MaxSceneMeshesMetaCount;
	uint TotalInstancesCount;
	uint TotalMeshesCount;
	uint CascadesCount;
	uint FrustumCullingEnabled;
	uint CameraHiZCullingEnabled;
	uint ShadowsHiZCullingEnabled;
	uint ClusterBackfaceCullingEnabled;
	uint pad0;
	uint2 pad1;
	float2 DepthResolution;
	float2 ShadowMapResolution;
	float4 CameraPosition;
	float4 LightDirection;
	float4 CascadeCameraPosition[MAX_CASCADES_COUNT];
	Frustum Camera;
	Frustum Cascade[MAX_CASCADES_COUNT];
	float4x4 PrevFrameCameraVP;
	float4x4 PrevFrameCascadeVP[MAX_CASCADES_COUNT];
};

#endif // CULLING_COMMON_HLSL
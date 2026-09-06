#pragma once

#include "Common.h"

#include <string>

class Settings
{
public:

	static constexpr uint32_t BackBufferWidth = 1920;
	static constexpr uint32_t BackBufferHeight = 1080;
	static constexpr float BackBufferAspectRatio =
		static_cast<float>(BackBufferWidth) / static_cast<float>(BackBufferHeight);

	static constexpr uint32_t MaxBackBufferMipsCount = 15;

	static constexpr uint32_t ShadowMapRes = 2048;
	static constexpr uint32_t ShadowMapMipsCount = 12;

	static constexpr int CameraCount = 1;
	static constexpr float CameraNearZ = 0.001f;
	static constexpr float CameraFarZ = 10000.0f;
	static constexpr float GUITransparency = 0.7f;

	static constexpr int StatsGUILocation = 1;
	static constexpr int SWRGUILocation = 0;
	static constexpr int ShadowsGUILocation = 3;

	static uint32_t RenderWidth;
	static uint32_t RenderHeight;
	static int CascadesCount;
	static int FrustumsCount;

	static bool CullingEnabled;
	static bool FrustumCullingEnabled;
	static bool CameraHiZCullingEnabled;
	static bool ShadowsHiZCullingEnabled;
	static bool PerTriangleHiZRasterizationCullingEnabled;
	static bool ClusterBackfaceCullingEnabled;
	static bool SWREnabled;
	static bool AsyncComputeEnabled;
	static bool ShowMeshlets;
	static bool FreezeCulling;

	static std::string AssetsPath;
};


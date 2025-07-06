#pragma once

#include "Common.h"

#define SCENE_MESHLETIZATION

class Settings
{
public:

	static Settings Demo;

	static const int BackBufferWidth = 1920;
	static const int BackBufferHeight = 1080;
	static const float BackBufferAspectRatio;
	static const int BackBufferMipsCount = 11;

	static const int ShadowMapRes = 2048;
	// TODO: eliminate hardcode
	static const int ShadowMapMipsCount = 12;
	static const int CameraCount = 1;
	static int CascadesCount;
	static int FrustumsCount;
	static bool CullingEnabled;
	static bool FrustumCullingEnabled;
	static bool CameraHiZCullingEnabled;
	static bool ShadowsHiZCullingEnabled;
	static bool ClusterBackfaceCullingEnabled;
	static bool SWREnabled;
	static bool ShowMeshlets;
	static bool FreezeCulling;
	static const float CameraNearZ;
	static const float CameraFarZ;
	static const float GUITransparency;
	static const int StatsGUILocation = 1;
	static const int SWRGUILocation = 0;
	static const int ShadowsGUILocation = 3;

	static const bool UseWarpDevice;

	std::wstring AssetsPath;
	static const DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
};
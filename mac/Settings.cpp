#include "Settings.h"

uint32_t Settings::RenderWidth = Settings::BackBufferWidth;
uint32_t Settings::RenderHeight = Settings::BackBufferHeight;
int Settings::CascadesCount = 4;
int Settings::FrustumsCount = Settings::CameraCount + Settings::CascadesCount;
bool Settings::CullingEnabled = true;
bool Settings::FrustumCullingEnabled = true;
bool Settings::CameraHiZCullingEnabled = true;
bool Settings::ShadowsHiZCullingEnabled = true;
bool Settings::PerTriangleHiZRasterizationCullingEnabled = true;
bool Settings::ClusterBackfaceCullingEnabled = true;
bool Settings::SWREnabled = false;
bool Settings::SWRWaveEnabled = false;
bool Settings::AsyncComputeEnabled = true;
bool Settings::ShowMeshlets = false;
bool Settings::ShowOverdraw = false;
bool Settings::FreezeCulling = false;
std::string Settings::AssetsPath;

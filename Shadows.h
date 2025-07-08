#pragma once

#include "Settings.h"

class Shadows
{
public:

	static Shadows Sun;

	Shadows() = default;
	Shadows(const Shadows&) = delete;
	Shadows& operator=(const Shadows&) = delete;
	~Shadows() = default;

	void Initialize();
	void Update();
	void PreparePrevFrameShadowMap();

	void GUINewFrame();

	ID3D12Resource* GetShadowMapHWR() { return _shadowMapHWR.Get(); }
	ID3D12Resource* GetShadowMapSWR() { return _shadowMapSWR.Get(); }
	ID3D12PipelineState* GetPSO() { return _shadowsPSO.Get(); }
	const CD3DX12_VIEWPORT& GetViewport() { return _viewport; }
	const CD3DX12_RECT& GetScissorRect() { return _scissorRect; }

	const DirectX::XMFLOAT4X4& GetCascadeVP(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeVP[cascade];
	}

	const DirectX::XMFLOAT4X4& GetPrevFrameCascadeVP(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _prevFrameCascadeVP[cascade];
	}

	float GetCascadeBias(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeBias[cascade];
	}

	float GetCascadeSplitNormalized(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeSplitsNormalized[cascade];
	}

	float GetCascadeSplit(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeSplits[cascade];
	}

	const Frustum& GetCascadeFrustum(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeFrustums[cascade];
	}

	const DirectX::XMFLOAT4& GetCascadeCameraPosition(int cascade) const
	{
		assert(cascade < MAX_CASCADES_COUNT);
		return _cascadeCameraPosition[cascade];
	}

	bool ShowCascades() const { return _showCascades; }
	float GetShadowDistance() const { return _shadowDistance; }

private:

	static const float ShadowMinDistance;

	void _createHWRShadowMapResources();
	void _createSWRShadowMapResources();
	void _createPrevFrameShadowMapResources();
	void _createPSO();
	void _updateFrustumPlanes();
	void _computeNearAndFar(
		FLOAT& fNearPlane,
		FLOAT& fFarPlane,
		DirectX::FXMVECTOR vLightCameraOrthographicMin,
		DirectX::FXMVECTOR vLightCameraOrthographicMax,
		DirectX::XMVECTOR* pvPointsInCameraView);

	Microsoft::WRL::ComPtr<ID3D12RootSignature> _shadowsRS;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> _shadowsPSO;
	// for hardware rasterizer pipeline
	Microsoft::WRL::ComPtr<ID3D12Resource> _shadowMapHWR;
	// SWR needs unordered writes
	Microsoft::WRL::ComPtr<ID3D12Resource> _shadowMapSWR;
	// for Hi-Z culling
	Microsoft::WRL::ComPtr<ID3D12Resource> _prevFrameShadowMap;
	CD3DX12_VIEWPORT _viewport;
	CD3DX12_RECT _scissorRect;

	DirectX::XMFLOAT4 _cascadeCameraPosition[MAX_CASCADES_COUNT];
	DirectX::XMFLOAT4X4 _cascadeVP[MAX_CASCADES_COUNT];
	DirectX::XMFLOAT4X4 _prevFrameCascadeVP[MAX_CASCADES_COUNT];
	Frustum _cascadeFrustums[MAX_CASCADES_COUNT];
	float _cascadeBias[MAX_CASCADES_COUNT];
	float _cascadeSplitsNormalized[MAX_CASCADES_COUNT];
	float _cascadeSplits[MAX_CASCADES_COUNT];
	float _shadowDistance = 5000.0f;
	float _bias = 0.001f;

	// debug and visualisation stuff
	bool _showCascades = false;
};
#pragma once

#include "Common.h"

class Camera
{
public:

	Camera();
	~Camera() {}

	void MoveVertical(float amount);
	void Walk(float amount);
	void Strafe(float amount);

	void RotateY(float angle);
	void RotateX(float angle);

	void UpdateViewMatrix();

	void SetProjection(
		float fovY,
		float aspect,
		float nearZ,
		float farZ);

	void LookAt(
		simd_float3 position,
		simd_float3 target,
		simd_float3 worldUp);

	const simd_float4x4& GetView() const { return _view; }
	const simd_float4x4& GetProjection() const { return _projection; }
	const simd_float4x4& GetVP() const { return _viewProjection; }
	const simd_float4x4& GetPrevFrameVP() const
	{
		return _prevFrameViewProjection;
	}

	Frustum GetFrustum() const { return _frustum; }

	const simd_float4& GetFrustumCornerWS(int corner) const
	{
		assert(corner < 8);
		return _frustum.cornersWS[corner];
	}

	const simd_float3& GetUp() const { return _up; }
	const simd_float3& GetRight() const { return _right; }
	const simd_float3& GetLook() const { return _look; }
	const simd_float3& GetPosition() const { return _position; }

	void SetPosition(float x, float y, float z);
	void SetPosition(simd_float3 position);

	float GetNearZ() const { return _nearZ; }
	float GetFarZ() const { return _farZ; }
	float GetAspect() const { return _aspect; }
	float GetFovX() const
	{
		return 2.0f * atanf((0.5f * GetNearWindowWidth()) / _nearZ);
	}
	float GetFovY() const { return _fovY; }
	float GetNearWindowHeight() const { return _nearWindowHeight; }
	float GetFarWindowHeight() const { return _farWindowHeight; }
	float GetNearWindowWidth() const { return _aspect * _nearWindowHeight; }
	float GetFarWindowWidth() const { return _aspect * _farWindowHeight; }

	bool ReverseZ() const { return _reverseZ; }

private:

	void _updateFrustumPlanes();

	simd_float4x4 _view = matrix_identity_float4x4;
	simd_float4x4 _projection = matrix_identity_float4x4;
	simd_float4x4 _viewProjection = matrix_identity_float4x4;
	simd_float4x4 _prevFrameViewProjection = matrix_identity_float4x4;

	Frustum _frustum;

	simd_float3 _up = { 0.0f, 1.0f, 0.0f };
	simd_float3 _right = { 1.0f, 0.0f, 0.0f };
	simd_float3 _look = { 0.0f, 0.0f, 1.0f };
	simd_float3 _position = { 0.0f, 0.0f, -1.0f };

	float _nearZ = 0.0f;
	float _farZ = 0.0f;
	float _aspect = 0.0f;
	float _fovY = 0.0f;
	float _nearWindowHeight = 0.0f;
	float _farWindowHeight = 0.0f;

	bool _reverseZ = true;
};

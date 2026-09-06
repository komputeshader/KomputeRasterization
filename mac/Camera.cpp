#include "Camera.h"
#include "Settings.h"
#include "Utils.h"

Camera::Camera()
{
	SetProjection(
		static_cast<float>(M_PI_4),
		1.0f,
		Settings::CameraNearZ,
		Settings::CameraFarZ);
}

void Camera::MoveVertical(float amount)
{
	_position += _up * amount;
}

void Camera::Walk(float amount)
{
	_position += _look * amount;
}

void Camera::Strafe(float amount)
{
	_position += _right * amount;
}

void Camera::RotateY(float angle)
{
	const simd_quatf rotation = simd_quaternion(angle, simd_make_float3(0.0f, 1.0f, 0.0f));

	_right = simd_act(rotation, _right);
	_up = simd_act(rotation, _up);
	_look = simd_act(rotation, _look);
}

void Camera::RotateX(float angle)
{
	const simd_quatf rotation = simd_quaternion(angle, simd_normalize(_right));

	_up = simd_act(rotation, _up);
	_look = simd_act(rotation, _look);
}

void Camera::UpdateViewMatrix()
{
	_look = simd_normalize(_look);
	_up = simd_normalize(simd_cross(_look, _right));
	_right = simd_cross(_up, _look);

	_view = Utils::LookAtLH(_position, _position + _look, _up);
	_prevFrameViewProjection = _viewProjection;
	_viewProjection = simd_mul(_projection, _view);
	_updateFrustumPlanes();

	const float nearWindowHalfWidth = GetNearWindowWidth() * 0.5f;
	const float nearWindowHalfHeight = GetNearWindowHeight() * 0.5f;
	const simd_float3 nearLook = _look * _nearZ;
	const simd_float3 nearUp = _up * nearWindowHalfHeight;
	const simd_float3 nearRight = _right * nearWindowHalfWidth;
	_frustum.cornersWS[0] = simd_make_float4(_position + nearLook + nearUp - nearRight, 0.0f);
	_frustum.cornersWS[1] = simd_make_float4(_position + nearLook + nearUp + nearRight, 0.0f);
	_frustum.cornersWS[2] = simd_make_float4(_position + nearLook - nearUp + nearRight, 0.0f);
	_frustum.cornersWS[3] = simd_make_float4(_position + nearLook - nearUp - nearRight, 0.0f);

	const float farWindowHalfWidth = GetFarWindowWidth() * 0.5f;
	const float farWindowHalfHeight = GetFarWindowHeight() * 0.5f;
	const simd_float3 farLook = _look * _farZ;
	const simd_float3 farUp = _up * farWindowHalfHeight;
	const simd_float3 farRight = _right * farWindowHalfWidth;
	_frustum.cornersWS[4] = simd_make_float4(_position + farLook + farUp - farRight, 0.0f);
	_frustum.cornersWS[5] = simd_make_float4(_position + farLook + farUp + farRight, 0.0f);
	_frustum.cornersWS[6] = simd_make_float4(_position + farLook - farUp + farRight, 0.0f);
	_frustum.cornersWS[7] = simd_make_float4(_position + farLook - farUp - farRight, 0.0f);
}

void Camera::SetProjection(
	float fovY,
	float aspect,
	float nearZ,
	float farZ)
{
	_fovY = fovY;
	_aspect = aspect;
	_nearZ = nearZ;
	_farZ = farZ;

	const float heightFactor = 2.0f * std::tan(0.5f * fovY);
	_nearWindowHeight = nearZ * heightFactor;
	_farWindowHeight = farZ * heightFactor;

	_projection = Utils::PerspectiveFovLHReverseZ(fovY, aspect, nearZ, farZ);
	_prevFrameViewProjection = _viewProjection;
	_viewProjection = simd_mul(_projection, _view);
	_updateFrustumPlanes();
}

void Camera::_updateFrustumPlanes()
{
	_frustum = Utils::GetFrustum(_viewProjection);
}

void Camera::LookAt(
	simd_float3 position,
	simd_float3 target,
	simd_float3 worldUp)
{
	const simd_float3 look = simd_normalize(target - position);
	const simd_float3 right = simd_normalize(simd_cross(worldUp, look));
	const simd_float3 up = simd_cross(look, right);

	_position = position;
	_look = look;
	_right = right;
	_up = up;
}

void Camera::SetPosition(float x, float y, float z)
{
	_position.x = x;
	_position.y = y;
	_position.z = z;
}

void Camera::SetPosition(simd_float3 position)
{
	_position = position;
}

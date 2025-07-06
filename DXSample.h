//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#include "Common.h"

class DXSample
{
public:
	DXSample(UINT width, UINT height, std::wstring name);
	virtual ~DXSample();

	virtual void Initialize() = 0;
	virtual void Update() = 0;
	virtual void Draw() = 0;
	virtual void Resize(UINT width, UINT height, bool minimized) = 0;
	virtual void Destroy() = 0;

	// Samples override the event handlers to handle specific messages.
	virtual void WindowMoved(int /*x*/, int /*y*/) {}
	virtual void MouseMove(UINT /*x*/, UINT /*y*/) {}
	virtual void LMBPressed(UINT /*x*/, UINT /*y*/) {}
	virtual void LMBReleased(UINT /*x*/, UINT /*y*/) {}
	virtual void RMBPressed(UINT /*x*/, UINT /*y*/) {}
	virtual void RMBReleased(UINT /*x*/, UINT /*y*/) {}
	virtual void KeyPressed(UINT8 /*key*/) {}
	virtual void KeyReleased(UINT8 /*key*/) {}
	virtual void DisplayChanged() {}

	// Accessors.
	UINT GetWidth() const           { return _width; }
	UINT GetHeight() const          { return _height; }
	const WCHAR* GetTitle() const   { return m_title.c_str(); }
	bool GetTearingSupport() const  { return m_tearingSupport; }
	RECT GetWindowsBounds() const   { return m_windowBounds; }
	virtual IDXGISwapChain* GetSwapchain() { return nullptr; }

	void ParseCommandLineArgs(_In_reads_(argc) WCHAR* argv[], int argc);
	void UpdateForSizeChange(UINT clientWidth, UINT clientHeight);
	void SetWindowBounds(int left, int top, int right, int bottom);

protected:

	void SetCustomWindowText(LPCWSTR text);
	void CheckTearingSupport();

	// Viewport dimensions.
	UINT _width;
	UINT _height;

	// Window bounds
	RECT m_windowBounds;

	// Whether or not tearing is available for fullscreen borderless windowed mode.
	bool m_tearingSupport;

	// Adapter info.
	bool m_useWarpDevice;

	// Override to be able to start without Dx11on12 UI for PIX. PIX doesn't support 11 on 12. 
	bool m_enableUI;

	// Root assets path.
	std::wstring _assetsPath;

private:

	// Window title.
	std::wstring m_title;

};

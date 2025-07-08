#pragma once

#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#include "Common.h"

class DXSample
{
public:
	DXSample(unsigned int width, unsigned int height, std::wstring name);
	virtual ~DXSample();

	virtual void Initialize() = 0;
	virtual void Update() = 0;
	virtual void Draw() = 0;
	virtual void Resize(unsigned int width, unsigned int height, bool minimized) = 0;
	virtual void Destroy() = 0;

	// Samples override the event handlers to handle specific messages.
	virtual void WindowMoved(int /*x*/, int /*y*/) {}
	virtual void MouseMove(unsigned int /*x*/, unsigned int /*y*/) {}
	virtual void LMBPressed(unsigned int /*x*/, unsigned int /*y*/) {}
	virtual void LMBReleased(unsigned int /*x*/, unsigned int /*y*/) {}
	virtual void RMBPressed(unsigned int /*x*/, unsigned int /*y*/) {}
	virtual void RMBReleased(unsigned int /*x*/, unsigned int /*y*/) {}
	virtual void KeyPressed(unsigned char /*key*/) {}
	virtual void KeyReleased(unsigned char /*key*/) {}
	virtual void DisplayChanged() {}

	// Accessors.
	unsigned int GetWidth() const { return _width; }
	unsigned int GetHeight() const { return _height; }
	const wchar_t* GetTitle() const { return m_title.c_str(); }
	bool GetTearingSupport() const { return m_tearingSupport; }
	RECT GetWindowsBounds() const { return m_windowBounds; }
	virtual IDXGISwapChain* GetSwapchain() { return nullptr; }

	void ParseCommandLineArgs(_In_reads_(argc) wchar_t* argv[], int argc);
	void UpdateForSizeChange(unsigned int clientWidth, unsigned int clientHeight);
	void SetWindowBounds(int left, int top, int right, int bottom);

protected:

	void SetCustomWindowText(LPCWSTR text);
	void CheckTearingSupport();

	// Viewport dimensions.
	unsigned int _width;
	unsigned int _height;

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

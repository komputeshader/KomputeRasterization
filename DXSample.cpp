#include "DXSample.h"
#include "Win32Application.h"
#include "Utils.h"

using namespace Microsoft::WRL;
using namespace std;

DXSample::DXSample(unsigned int width, unsigned int height, std::wstring name) :
	_width(width),
	_height(height),
	m_windowBounds{0, 0, 0, 0},
	m_title(name),
	m_useWarpDevice(false),
	m_enableUI(true)
{
	wchar_t assetsPath[512];
	Utils::GetAssetsPath(assetsPath, _countof(assetsPath));
	_assetsPath = assetsPath;

	UpdateForSizeChange(width, height);
	CheckTearingSupport();
}

DXSample::~DXSample()
{
}

void DXSample::UpdateForSizeChange(unsigned int clientWidth, unsigned int clientHeight)
{
	_width = clientWidth;
	_height = clientHeight;
}

// Helper function for setting the window's title text.
void DXSample::SetCustomWindowText(LPCWSTR text)
{
	std::wstring windowText = m_title + L": " + text;
	SetWindowText(Win32Application::GetHwnd(), windowText.c_str());
}

// Helper function for parsing any supplied command line args.
_Use_decl_annotations_
void DXSample::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	for (int i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-warp", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/warp", wcslen(argv[i])) == 0)
		{
			m_useWarpDevice = true;
			m_title = m_title + L" (WARP)";
		}
		else if (_wcsnicmp(argv[i], L"-disableUI", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/disableUI", wcslen(argv[i])) == 0)
		{
			m_enableUI = false;
		}
	}
}

// Determines whether tearing support is available for fullscreen borderless windows.
void DXSample::CheckTearingSupport()
{
#ifndef PIXSUPPORT
	ComPtr<IDXGIFactory6> factory;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	BOOL allowTearing = FALSE;
	if (SUCCEEDED(hr))
	{
		hr = factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
	}

	m_tearingSupport = SUCCEEDED(hr) && allowTearing;
#else
	m_tearingSupport = TRUE;
#endif
}

void DXSample::SetWindowBounds(int left, int top, int right, int bottom)
{
	m_windowBounds.left = static_cast<LONG>(left);
	m_windowBounds.top = static_cast<LONG>(top);
	m_windowBounds.right = static_cast<LONG>(right);
	m_windowBounds.bottom = static_cast<LONG>(bottom);
}
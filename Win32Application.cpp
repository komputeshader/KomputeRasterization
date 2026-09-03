#include "Common.h"
#include "Win32Application.h"
#include "DXSample.h"
#include "Utils.h"
#include "imgui.h"
#include "imgui_impl_win32.h"

HWND Win32Application::m_hwnd = nullptr;

namespace
{
	constexpr DWORD WindowStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	constexpr UINT DefaultDpi = 96;

	unsigned int PhysicalClientDimension(unsigned int logicalDimension, float dpiScale)
	{
		return static_cast<unsigned int>(logicalDimension * dpiScale + 0.5f);
	}

	RECT WindowRectForClientSize(
		unsigned int clientWidth,
		unsigned int clientHeight,
		UINT dpi)
	{
		RECT windowRect =
		{
			0,
			0,
			static_cast<LONG>(clientWidth),
			static_cast<LONG>(clientHeight)
		};

		using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(
			LPRECT,
			DWORD,
			BOOL,
			DWORD,
			UINT);
		auto adjustWindowRectExForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
			GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
		if (adjustWindowRectExForDpi)
		{
			adjustWindowRectExForDpi(&windowRect, WindowStyle, FALSE, 0, dpi);
		}
		else
		{
			AdjustWindowRectEx(&windowRect, WindowStyle, FALSE, 0);
		}

		return windowRect;
	}

	bool ImGuiCapturesMouse()
	{
		return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
	}

	bool ImGuiCapturesKeyboard()
	{
		return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
	}

	void AssertClientSize(HWND window, unsigned int width, unsigned int height)
	{
		RECT clientRect = {};
		ASSERT(GetClientRect(window, &clientRect), "Failed to query the window client size")
		ASSERT(
			static_cast<unsigned int>(clientRect.right - clientRect.left) == width &&
			static_cast<unsigned int>(clientRect.bottom - clientRect.top) == height,
			"The window client size does not match the configured backbuffer size")
	}
}

int Win32Application::Run(DXSample* pSample, HINSTANCE hInstance, int nCmdShow)
{
	// Avoid DWM bitmap scaling. Convert the configured 96-DPI window size to
	// physical pixels and render the swap chain at that native resolution.
	ImGui_ImplWin32_EnableDpiAwareness();

	// Parse the command line parameters
	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	pSample->ParseCommandLineArgs(argv, argc);
	LocalFree(argv);

	// Initialize the window class.
	WNDCLASSEX windowClass = { 0 };
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = WindowProc;
	windowClass.hInstance = hInstance;
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.lpszClassName = L"DXSampleClass";
	RegisterClassEx(&windowClass);

	const HMONITOR primaryMonitor = MonitorFromPoint(
		POINT{ 0, 0 },
		MONITOR_DEFAULTTOPRIMARY);
	const float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(primaryMonitor);
	const UINT dpi = static_cast<UINT>(dpiScale * DefaultDpi + 0.5f);
	pSample->UpdateForSizeChange(
		PhysicalClientDimension(pSample->GetLogicalWidth(), dpiScale),
		PhysicalClientDimension(pSample->GetLogicalHeight(), dpiScale));
	const RECT windowRect = WindowRectForClientSize(
		pSample->GetWidth(),
		pSample->GetHeight(),
		dpi);

	// Create the window and store a handle to it.
	m_hwnd = CreateWindow(
		windowClass.lpszClassName,
		pSample->GetTitle(),
		WindowStyle,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr, // We have no parent window.
		nullptr, // We aren't using menus.
		hInstance,
		pSample);
	ASSERT(m_hwnd, "Failed to create the application window")

	// CW_USEDEFAULT normally selects the primary monitor, but correct the frame
	// and native render size against the monitor Windows actually chose.
	const float windowDpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd);
	const UINT windowDpi = static_cast<UINT>(windowDpiScale * DefaultDpi + 0.5f);
	pSample->UpdateForSizeChange(
		PhysicalClientDimension(pSample->GetLogicalWidth(), windowDpiScale),
		PhysicalClientDimension(pSample->GetLogicalHeight(), windowDpiScale));
	const RECT actualWindowRect = WindowRectForClientSize(
		pSample->GetWidth(),
		pSample->GetHeight(),
		windowDpi);
	SetWindowPos(
		m_hwnd,
		nullptr,
		0,
		0,
		actualWindowRect.right - actualWindowRect.left,
		actualWindowRect.bottom - actualWindowRect.top,
		SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
	AssertClientSize(m_hwnd, pSample->GetWidth(), pSample->GetHeight());

	// Initialize the sample. Initialize is defined in each child-implementation of DXSample.
	pSample->Initialize();

	ShowWindow(m_hwnd, nCmdShow == SW_SHOWMAXIMIZED ? SW_SHOWNORMAL : nCmdShow);

	// Main sample loop.
	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		// Process any messages in the queue.
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	pSample->Destroy();

	// Return this part of the WM_QUIT message to Windows.
	return static_cast<char>(msg.wParam);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND hWnd,
	UINT msg,
	WPARAM wParam,
	LPARAM lParam);

// Main message handler for the sample.
LRESULT CALLBACK Win32Application::WindowProc(
	HWND hWnd,
	unsigned int message,
	WPARAM wParam,
	LPARAM lParam)
{
	DXSample* pSample = reinterpret_cast<DXSample*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
	{
		return true;
	}

	switch (message)
	{
	case WM_CREATE:
	{
		// Save the DXSample* passed in to CreateWindow.
		LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
		SetWindowLongPtr(
			hWnd,
			GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));

		return 0;
	}

	case WM_MOUSEMOVE:
	{
		unsigned int x = LOWORD(lParam);
		unsigned int y = HIWORD(lParam);
		if (pSample && !ImGuiCapturesMouse() && static_cast<unsigned char>(wParam) == MK_RBUTTON)
		{
			pSample->MouseMove(x, y);
		}
		else if (pSample && !ImGuiCapturesMouse())
		{
			pSample->RMBPressed(x, y);
		}
		return 0;
	}

	case WM_KEYDOWN:
	{
		if (pSample && !ImGuiCapturesKeyboard())
		{
			pSample->KeyPressed(static_cast<unsigned char>(wParam));
		}
		return 0;
	}

	case WM_GETMINMAXINFO:
	{
		if (!pSample)
		{
			break;
		}

		const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd);
		const UINT dpi = static_cast<UINT>(dpiScale * DefaultDpi + 0.5f);
		const RECT outerRect = WindowRectForClientSize(
			pSample->GetWidth(),
			pSample->GetHeight(),
			dpi);
		MINMAXINFO* minMaxInfo = reinterpret_cast<MINMAXINFO*>(lParam);
		const LONG requestedWidth = outerRect.right - outerRect.left;
		const LONG requestedHeight = outerRect.bottom - outerRect.top;
		if (minMaxInfo->ptMaxTrackSize.x < requestedWidth)
		{
			minMaxInfo->ptMaxTrackSize.x = requestedWidth;
		}
		if (minMaxInfo->ptMaxTrackSize.y < requestedHeight)
		{
			minMaxInfo->ptMaxTrackSize.y = requestedHeight;
		}
		return 0;
	}

	case WM_DPICHANGED:
	{
		if (pSample)
		{
			const UINT dpi = HIWORD(wParam);
			const float dpiScale = static_cast<float>(dpi) / DefaultDpi;
			pSample->DpiChanged(dpiScale);

			const RECT outerRect = WindowRectForClientSize(
				pSample->GetWidth(),
				pSample->GetHeight(),
				dpi);
			const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam);
			SetWindowPos(
				hWnd,
				nullptr,
				suggestedRect->left,
				suggestedRect->top,
				outerRect.right - outerRect.left,
				outerRect.bottom - outerRect.top,
				SWP_NOACTIVATE | SWP_NOZORDER);
			AssertClientSize(hWnd, pSample->GetWidth(), pSample->GetHeight());
		}
		return 0;
	}

	case WM_PAINT:
	{
		if (pSample)
		{
			pSample->Update();
			pSample->Draw();
		}
		return 0;
	}

	case WM_DESTROY:
	{
		PostQuitMessage(0);
		return 0;
	}
	}

	// Handle any messages the switch statement didn't.
	return DefWindowProc(hWnd, message, wParam, lParam);
}

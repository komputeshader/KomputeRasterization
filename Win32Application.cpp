#include "Common.h"
#include "Win32Application.h"
#include "DXSample.h"
#include "imgui_impl_win32.h"

HWND Win32Application::m_hwnd = nullptr;

#define WINDOW_FLAGS (WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER)

int Win32Application::Run(DXSample* pSample, HINSTANCE hInstance, int nCmdShow)
{
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

	RECT windowRect =
	{
		0,
		0,
		static_cast<LONG>(pSample->GetWidth()),
		static_cast<LONG>(pSample->GetHeight())
	};

	AdjustWindowRect(&windowRect, WINDOW_FLAGS, FALSE);

	// Create the window and store a handle to it.
	m_hwnd = CreateWindow(
		windowClass.lpszClassName,
		pSample->GetTitle(),
		WINDOW_FLAGS,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		nullptr, // We have no parent window.
		nullptr, // We aren't using menus.
		hInstance,
		pSample);

	// Initialize the sample. Initialize is defined in each child-implementation of DXSample.
	pSample->Initialize();

	ShowWindow(m_hwnd, nCmdShow);

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
		if (pSample && static_cast<unsigned char>(wParam) == MK_RBUTTON)
		{
			pSample->MouseMove(x, y);
		}
		else if (pSample)
		{
			pSample->RMBPressed(x, y);
		}
		return 0;
	}

	case WM_KEYDOWN:
	{
		if (pSample)
		{
			pSample->KeyPressed(static_cast<unsigned char>(wParam));
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
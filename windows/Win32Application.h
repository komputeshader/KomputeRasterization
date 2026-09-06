#pragma once

class DXSample;

class Win32Application
{
public:

	static int Run(DXSample* pSample, HINSTANCE hInstance, int nCmdShow);
	static HWND GetHwnd() { return m_hwnd; }

protected:

	static LRESULT CALLBACK WindowProc(HWND hWnd, unsigned int message, WPARAM wParam, LPARAM lParam);

private:

	static HWND m_hwnd;
};

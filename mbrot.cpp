#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <richedit.h>
#include <commctrl.h>
#include "resource.h"
#include <ppl.h>
#include <Vfw.h>

#include <atomic>
#include <complex>
#include <mutex>

RECT rFractal, rStat;
HBITMAP hFractal;
HINSTANCE hInstance;
HWND hwndFractal;
HWND hdlgStatus;

std::complex<double> centre{ -0.5, 0 };
double scale = 0.0;
double scaleSave = 0.0;
double offsetX;
double offsetY;
bool fJulia;
std::complex<double> juliaSeed;
DWORD previter, stepiter;
DWORD *rgbPlBits;
BITMAP bmFractal;
std::atomic<bool> invalidate = true;
std::atomic<bool> finished = false;

std::atomic<DWORD> captured = 0;
std::atomic<DWORD> escaped = 0;
std::complex<double> *rgZValue;
std::complex<double> *rgZValueSlow;
std::complex<double> *rgCValue;

std::mutex bitmapLock;
std::condition_variable cvInvalidate;
std::condition_variable cvFinished;
#define TakeBitmapLock() std::lock_guard<std::mutex> lock(bitmapLock)

template<typename T> T sqr(T t) { return t*t; }

void EarlyExit(DWORD &out, LONG x, LONG y)
{
	int idx = y * (rFractal.right - rFractal.left) + x;
	const std::complex<double> &c = rgCValue[idx];
	std::complex<double> &z = rgZValue[idx];

	// Early bail test -- look for main cardioid
	double q = sqr(c.real() - 0.25) + sqr(c.imag());
	if (q*(q + (c.real() - 0.25)) < sqr(c.imag()) / 4.0)
	{
		out = RGB(0, 0, 0);
		z = nan("");
		captured++;
		return;
	}

	// Look for primary bulb
	if (sqr(c.real() + 1.0) + sqr(c.imag()) < 1.0 / 16.0)
	{
		out = RGB(0, 0, 0);
		z = nan("");
		captured++;
		return;
	}
}

void Mandelbrot(DWORD &out, LONG x, LONG y)
{
//	std::lock_guard<std::mutex> l(bitmapLock);  // uncomment to single-thread calc for debugging

	int idx = y * (rFractal.right - rFractal.left) + x;

	const std::complex<double> &c = fJulia ? juliaSeed : rgCValue[idx];

	DWORD maxiter = stepiter;
	std::complex<double> &z = rgZValue[idx];
	std::complex<double> &zSlow = rgZValueSlow[idx];

	if (isnan(z.real()))
	{
		return;
	}

	for (DWORD i = 0; i < maxiter; i++)
	{
		if (invalidate)
			break;

		z = sqr(z) + c;
		if (i & 1)
			zSlow = sqr(zSlow) + c;

		if (z == zSlow)
		{
			captured++;
			out = RGB(0, 0, 0);
			z = nan("");
			return;
		}

		if ((z.real() < -4.0) || (z.real() > 4.0) || (z.imag() < -4.0) || (z.imag() > 4.0))
		{
			double log_zn = log(sqr(z.real()) + sqr(z.imag())) / 2.0;
			double nu = log2(log_zn / log(2));
			double clr = i + previter + 1 - nu;
			int hue, coord;
			if (clr < 1.0)
			{
				hue = coord = 0;
			}
			else
			{
				hue = int(log2(clr)*576.0);
				coord = hue % 256;
			}

			z = nan("");
			escaped++;

			switch ((hue / 256) % 6)
			{
			case 0: out = RGB(255, coord, 0);        return;
			case 1: out = RGB(255-coord, 255, 0);    return;
			case 2: out = RGB(0, 255, coord);        return;
			case 3: out = RGB(0, 255-coord, 255);    return;
			case 4: out = RGB(coord, 0, 255);        return;
			case 5: out = RGB(255, 0, 255 - coord);  return;
			}
		}
	}
}

void LateFill(DWORD &out, LONG x, LONG y)
{
	int idx = y * (rFractal.right - rFractal.left) + x;
	std::complex<double> &z = rgZValue[idx];
	if (!isnan(z.real()))
	{
		out = RGB(0, 0, 0);
	}
}

char* DoubleToStr(double d)
{
	static char num[_CVTBUFSIZE];
	_snprintf_s(num, _TRUNCATE, "%.16f", d);
	return num;
}

double GetDouble(HWND hwnd)
{
	char sz[64];
	SendMessageA(hwnd, WM_GETTEXT, _countof(sz), (LPARAM)sz);
	return atof(sz);
}

void GenerateOffsets(void)
{
	offsetX = centre.real() - (rFractal.right - rFractal.left) / 2 * scale;
	offsetY = centre.imag() + (rFractal.bottom - rFractal.top) / 2 * scale;
	SendDlgItemMessageA(hdlgStatus, IDC_CENTER_X, WM_SETTEXT, 0, (LPARAM)DoubleToStr(centre.real()));
	SendDlgItemMessageA(hdlgStatus, IDC_CENTER_Y, WM_SETTEXT, 0, (LPARAM)DoubleToStr(centre.imag()));
	SendDlgItemMessageA(hdlgStatus, IDC_SCALE, WM_SETTEXT, 0, (LPARAM)DoubleToStr(-log2(scale)));
}

std::complex<double> PixelToValue(DWORD x, DWORD y)
{
	return std::complex<double>(x * scale + offsetX, y*-scale + offsetY);
}

void ParallelCalc(LONG xSize, LONG ySize, LONG y, void (*fn)(DWORD &, LONG, LONG))
{
	DWORD *row = new DWORD[xSize];
	memcpy(row, rgbPlBits + (ySize - 1 - y)*xSize, 4 * xSize);
	for (LONG x = 0; x < xSize; x++)
		(*fn)(row[x], x, y);

	TakeBitmapLock();
	memcpy(rgbPlBits + (ySize - 1 - y)*xSize, row, 4 * xSize);
	GdiFlush();
	RECT r;
	r.top = rFractal.top + y;
	r.bottom = r.top + 1;
	r.left = rFractal.left;
	r.right = r.left + xSize;
	InvalidateRect(hwndFractal, &r, FALSE);
	delete[] row;
}

DWORD WINAPI CalculateFractal(void *)
{
	std::unique_lock<std::mutex> cvLock(bitmapLock, std::defer_lock);
	for (;;)
	{
		cvLock.lock();
		if (!invalidate)
		{
			cvInvalidate.wait(cvLock, [] { return invalidate && scale != 0.0; });
		}
		
		invalidate = false;
		finished = false;
		LONG xSize = rFractal.right - rFractal.left;
		LONG ySize = rFractal.bottom - rFractal.top;
		if (!hFractal || (xSize != bmFractal.bmWidth) || (ySize != bmFractal.bmHeight))
		{
			if (hFractal) DeleteObject(hFractal);
			delete[] rgZValue;
			delete[] rgZValueSlow;
			delete[] rgCValue;

			BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), xSize, ySize, 1, 32,
				BI_RGB, 0, 2834, 2834, 0, 0 } };

			HDC hdc = CreateCompatibleDC(NULL);
			hFractal = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&rgbPlBits, NULL, 0);
			rgZValue = new std::complex<double>[xSize * ySize];
			rgZValueSlow = new std::complex<double>[xSize * ySize];
			rgCValue = new std::complex<double>[xSize * ySize];

			GetObject(hFractal, sizeof(BITMAP), &bmFractal);
			GdiFlush();
			DeleteDC(hdc);
		}

		for (LONG y = 0; y < ySize; y++)
		{
			double yVal = y*-scale + offsetY;
			for (LONG x = 0; x < xSize; x++)
			{
				rgZValueSlow[y*xSize + x] = rgZValue[y*xSize + x] = rgCValue[y*xSize + x] = std::complex<double>(x * scale + offsetX, yVal);
			}
		}

		memset(rgbPlBits, RGB(192,192,192), bmFractal.bmWidthBytes*bmFractal.bmHeight);

		InvalidateRect(hwndFractal, NULL, FALSE);
		cvLock.unlock();
		captured = escaped = 0;
		if (!fJulia)
		{
			Concurrency::parallel_for(0L, ySize, [xSize, ySize](long row) { return ParallelCalc(xSize, ySize, row, EarlyExit); });
		}

		previter = 0;
		stepiter = 1000;
		auto mbrot = [xSize, ySize](long row) { return ParallelCalc(xSize, ySize, row, Mandelbrot); };
		bool found = false;
		unsigned long total = xSize * ySize - captured;

		for (;;)
		{
			if (found)
				captured = escaped = 0;
			Concurrency::parallel_for(0L, ySize, mbrot);
			SetDlgItemInt(hdlgStatus, IDC_MAXITER, previter + stepiter, FALSE);

			if (found)
			{
				if ((captured + escaped) < total / 256)
				{
					break;
				}
				total -= captured + escaped;
			}
			else
			{
				if ((captured + escaped) > total / 16)
				{
					found = true;
					total -= captured + escaped;
				}
			}

			SetDlgItemInt(hdlgStatus, IDC_UNRESOLVED, total, FALSE);
			
			if (total == 0)
				break;

			if (previter > 0x40000000)
				break;

			previter += stepiter;
			stepiter += stepiter / 2;
		}

		Concurrency::parallel_for(0L, ySize, std::bind(ParallelCalc, xSize, ySize, std::placeholders::_1, LateFill));
		SendDlgItemMessageA(hdlgStatus, IDC_UNRESOLVED, WM_SETTEXT, 0, (LPARAM)"0");

		finished = !invalidate;
		cvFinished.notify_one();
	}
}


DWORD WINAPI GenerateMovieFrames(void *args)
{
	PAVISTREAM pcompstream = (PAVISTREAM)(((LPARAM *)args)[0]);
	DWORD frames = (DWORD)(((LPARAM *)args)[1]);
	HWND hwnd = (HWND)(((LPARAM *)args)[2]);

	double startScale = 3.0 / (rFractal.right - rFractal.left);
	double endScale = scale;
	double deltaScale = log2(startScale / endScale);

	// We want to quantize the scale factor so that it hits every power of two.

	// Figure out make frames it takes to double and quantize it
	int framesToDouble = int(0.5 + frames / deltaScale);

	// Now we need to adjust the total number of frames
	// so we get to the end at the same time.
	frames = DWORD(0.5 + deltaScale * framesToDouble);
	DWORD loops = (frames + framesToDouble - 1) / framesToDouble;

	// Allocate three additional DIBs:
	// * The big frame (zoomed out)
	// * The small frame (zoomed in)
	// * The composite frame
	BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), rFractal.right - rFractal.left, rFractal.bottom - rFractal.top, 1, 32,
		BI_RGB, 0, 2834, 2834, 0, 0 } };
	const size_t cbFractal = bmFractal.bmWidthBytes*bmFractal.bmHeight;
	HDC hdc = CreateCompatibleDC(NULL);
	HDC hdc2 = CreateCompatibleDC(hdc);
	SetStretchBltMode(hdc, COLORONCOLOR);
	SetStretchBltMode(hdc2, COLORONCOLOR);
	DWORD *rgbBig, *rgbSmall, *rgbMerged;
	HBITMAP hBig = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&rgbBig, NULL, 0);
	HBITMAP hSmall = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&rgbSmall, NULL, 0);
	HBITMAP hMerged = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&rgbMerged, NULL, 0);
	SelectObject(hdc2, hMerged);

	SendDlgItemMessage(hwnd, IDC_PROGBAR, PBM_SETRANGE32, 0, loops + 1);

	std::unique_lock<std::mutex> cvLock(bitmapLock);

	// First frame
	scale = startScale;
	GenerateOffsets();
	invalidate = true;
	finished = false;
	cvInvalidate.notify_one();
	cvFinished.wait(cvLock, [] { return (bool)finished; });
	memcpy(rgbSmall, rgbPlBits, cbFractal);
	SendDlgItemMessage(hwnd, IDC_PROGBAR, PBM_SETPOS, 0, 0);

	// Second frame
	scale = startScale / 2.0;
	GenerateOffsets();
	invalidate = true;
	finished = false;
	time_t lasttime = time(nullptr);
	cvInvalidate.notify_one();
	SendDlgItemMessage(hwnd, IDC_PROGBAR, PBM_SETPOS, 1, 0);

	// The loop is going to push keyframe i, plus interpolate between frame i and i+1 while telling
	// the background calc to generate frame i+2
	int iFrame = 0;
	for (DWORD i = 0; i < loops; i++)
	{
		memcpy(rgbBig, rgbSmall, cbFractal);
		cvFinished.wait(cvLock, [] { return (bool)finished; });
		memcpy(rgbSmall, rgbPlBits, cbFractal);

		static WCHAR wzProg[40];
		time_t curtime = time(nullptr);
		swprintf_s(wzProg, L"Frame %d of %d [%lld sec]", i + 2, loops + 1, curtime - lasttime);
		lasttime = curtime;
		SendDlgItemMessage(hwnd, IDC_PROGTEXT, WM_SETTEXT, 0, (LPARAM)wzProg);
		SendDlgItemMessage(hwnd, IDC_PROGBAR, PBM_SETPOS, i + 2, 0);

		// Set args for next render here
		if (i + 1 != loops)
		{
			scale = startScale / exp2(i + 2);
			GenerateOffsets();
			invalidate = true;
			finished = false;
			cvInvalidate.notify_one();
		}

		cvLock.unlock();

		// Write the very first frame 30 times to give a 1 second into
		for (int j = 0; j < (i == 0 ? 30 : 1); j++)
		{
			if (FAILED(AVIStreamWrite((PAVISTREAM)pcompstream, iFrame++, 1, rgbBig, bmFractal.bmWidthBytes*bmFractal.bmHeight, 0, NULL, NULL)))
			{
				MessageBox(hwnd, L"Error writing frame.", L"Error", MB_OK);
				EndDialog(hwnd, 1);
				return 1;
			}
		}

		// Generate intermediate frames
		int limit = framesToDouble - 1;
		if (i + 1 == loops)
		{
			limit = frames % framesToDouble;
		}
		for (int j = 1; j < limit; j++)
		{
			double fraction = exp2(double(j) / double(framesToDouble));
			double scaleMed = 1.0 / fraction;
			GdiFlush();
			SelectObject(hdc, hBig);
			int w = int(bmFractal.bmWidth*scaleMed + 0.5);
			int h = int(bmFractal.bmHeight*scaleMed + 0.5);
			int x = (bmFractal.bmWidth - w) / 2;
			int y = (bmFractal.bmHeight - h) / 2;
			StretchBlt(hdc2, 0, 0, bmFractal.bmWidth, bmFractal.bmHeight, hdc, x, y, w, h, SRCCOPY);

			SelectObject(hdc, hSmall);
			scaleMed = fraction / 2.0;
			w = int(bmFractal.bmWidth*scaleMed + 0.5);
			h = int(bmFractal.bmHeight*scaleMed + 0.5);
			x = (bmFractal.bmWidth - w + 1) / 2;
			y = (bmFractal.bmHeight - h + 1) / 2;
			StretchBlt(hdc2, x, y, w, h, hdc, 0, 0, bmFractal.bmWidth, bmFractal.bmHeight, SRCCOPY);
			GdiFlush();

			if (FAILED(AVIStreamWrite((PAVISTREAM)pcompstream, iFrame++, 1, rgbMerged, bmFractal.bmWidthBytes*bmFractal.bmHeight, 0, NULL, NULL)))
			{
				MessageBox(hwnd, L"Error writing frame.", L"Error", MB_OK);
				EndDialog(hwnd, 1);
				return 1;
			}
		}

		cvLock.lock();
	}

	// Write the very last frame 30 times to give a 1 second outro
	for (int i = 0; i < 30; i++)
	{
		if (FAILED(AVIStreamWrite((PAVISTREAM)pcompstream, iFrame++, 1, rgbMerged, bmFractal.bmWidthBytes*bmFractal.bmHeight, 0, NULL, NULL)))
		{
			MessageBox(hwnd, L"Error writing frame.", L"Error", MB_OK);
			EndDialog(hwnd, 1);
			return 1;
		}
	}

	DeleteDC(hdc);
	DeleteDC(hdc2);
	DeleteObject(hBig);
	DeleteObject(hSmall);
	DeleteObject(hMerged);
	EndDialog(hwnd, 0);
	return 0;
}


INT_PTR CALLBACK ProgDlgProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	static HANDLE hRenderThread = 0;
	switch (iMsg)
	{
	case WM_INITDIALOG:
		((void **)lParam)[2] = hwnd;
		hRenderThread = CreateThread(NULL, 0, GenerateMovieFrames, (void *)lParam, 0, NULL);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDCANCEL:
			if (hRenderThread)
			{
				TerminateThread(hRenderThread, 1);
				CloseHandle(hRenderThread);
				hRenderThread = 0;
			}
			EndDialog(hwnd, 1);
			return TRUE;
		}
		break;
	}
	return FALSE;
};

INT_PTR CALLBACK AboutDlgProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM)
{
	switch (iMsg)
	{
	case WM_INITDIALOG:
		return(TRUE);
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
		case IDCANCEL:
			EndDialog(hwnd, 0);
			return(TRUE);
		};
		break;
	};
	return FALSE;
};

INT_PTR CALLBACK GetSeedDlgProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	static int initSeed;
	wchar_t szText[8];

	switch (iMsg)
	{
	case WM_INITDIALOG:
		initSeed = (int)lParam;
		wsprintf(szText, L"%d", (int)lParam);
		SendDlgItemMessage(hwnd, IDC_EDIT, EM_SETLIMITTEXT, (WPARAM)6, 0);
		SendDlgItemMessage(hwnd, IDC_EDIT, WM_SETTEXT, 0, (LPARAM)szText);
		SendDlgItemMessage(hwnd, IDC_EDIT, EM_SETSEL, 0, (LPARAM)-1);
		SetFocus(GetDlgItem(hwnd, IDC_EDIT));
		return FALSE;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			SendDlgItemMessage(hwnd, IDC_EDIT, WM_GETTEXT, 8, (LPARAM)szText);
			EndDialog(hwnd, _wtoi(szText));
			return(TRUE);

		case IDCANCEL:
			EndDialog(hwnd, initSeed);
			return(TRUE);
		};
		break;
	};
	return FALSE;
};

void RenderMovie(HWND hwnd)
{
	INT_PTR length = DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_GETSEED), hwnd, GetSeedDlgProc, 0);
	if (length == 0)
	{
		return;
	}

	OPENFILENAME ofn = { sizeof(ofn) };
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = L"AVI Files\0*.avi\0";
	WCHAR wzFile[MAX_PATH] = L"";
	ofn.lpstrFile = wzFile;
	ofn.nMaxFile = _countof(wzFile);
	ofn.lpstrTitle = L"Save animation as...";
	ofn.lpstrDefExt = L"avi";

	if (!GetSaveFileName(&ofn))
	{
		return;
	}

	AVIFileInit();

	PAVIFILE pavi;
	if (FAILED(AVIFileOpen(&pavi, wzFile, OF_CREATE | OF_SHARE_EXCLUSIVE | OF_WRITE, NULL)))
	{
		MessageBox(hwnd, L"Unable to write file...", L"Error", MB_OK);
		return;
	}

	PAVISTREAM pstream;
	AVISTREAMINFO si = { 0 };
	si.fccType = mmioFOURCC('v', 'i', 'd', 's');
	si.fccHandler = NULL;
	si.dwScale = 1001;
	si.dwRate = 30000;
	si.dwSuggestedBufferSize = bmFractal.bmWidthBytes*bmFractal.bmHeight;
	si.rcFrame.top = 0;
	si.rcFrame.bottom = rFractal.bottom - rFractal.top;
	si.rcFrame.left = 0;
	si.rcFrame.right = rFractal.right - rFractal.left;
	if (FAILED(AVIFileCreateStream(pavi, &pstream, &si)))
	{
		MessageBox(hwnd, L"Unable to create stream...", L"Error", MB_OK);
		return;
	}

	AVICOMPRESSOPTIONS avicomp = {};
	AVICOMPRESSOPTIONS *pavicomp = &avicomp;
	if (!AVISaveOptions(hwnd, ICMF_CHOOSE_KEYFRAME | ICMF_CHOOSE_DATARATE, 1, &pstream, &pavicomp))
	{
		return;
	}

	PAVISTREAM pcompstream;
	if (FAILED(AVIMakeCompressedStream(&pcompstream, pstream, &avicomp, NULL)))
	{
		MessageBox(hwnd, L"Unable to create compressed stream...", L"Error", MB_OK);
		return;
	}

	BITMAPINFOHEADER bih = { sizeof(BITMAPINFOHEADER), rFractal.right - rFractal.left, rFractal.bottom - rFractal.top, 1, 32,
		BI_RGB, 0, 2834, 2834, 0, 0 };

	if (FAILED(AVIStreamSetFormat(pcompstream, 0, &bih, sizeof(BITMAPINFOHEADER))))
	{
		MessageBox(hwnd, L"Could not set compressed stream format.", L"Error", MB_OK);
		return;
	}

	LPARAM args[3]{ (LPARAM)pcompstream, (LPARAM)length*30 };
	DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_PROG), hwnd, ProgDlgProc, (LPARAM)args);

	AVIStreamRelease(pcompstream);
	AVISaveOptionsFree(1, &pavicomp);
	AVIStreamRelease(pstream);
	AVIFileRelease(pavi);
	AVIFileExit();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	PAINTSTRUCT ps;
	static RECT rSelect{ 0,0,0,0 };
	static RECT rDrag{ 0,0 };

	switch (iMsg)
	{
	case WM_CLOSE:
		invalidate = true;
		bitmapLock.lock();
		break;

	case WM_LBUTTONDOWN:
		rSelect.left = GET_X_LPARAM(lParam);
		rSelect.top = GET_Y_LPARAM(lParam);
		break;

	case WM_LBUTTONUP:
	{
		rSelect.right = GET_X_LPARAM(lParam);
		rSelect.bottom = GET_Y_LPARAM(lParam);

		TakeBitmapLock();

		if (wParam & MK_CONTROL)
		{
			// Zoom out
			centre = 2.0*centre - PixelToValue((rSelect.left + rSelect.right) / 2, (rSelect.top + rSelect.bottom) / 2);
			scale = scale * (rFractal.right - rFractal.left) / abs(rSelect.right - rSelect.left);
		}
		else
		{
			// Zoom in
			centre = PixelToValue((rSelect.left + rSelect.right) / 2, (rSelect.top + rSelect.bottom) / 2);
			scale = scale * abs(rSelect.right - rSelect.left) / (rFractal.right - rFractal.left);
		}
		GenerateOffsets();

		rSelect = { 0,0,0,0 };
		invalidate = true;
		cvInvalidate.notify_one();
		break;
	}

	case WM_RBUTTONDOWN:
		rDrag.left = GET_X_LPARAM(lParam);
		rDrag.top = GET_Y_LPARAM(lParam);
		break;

	case WM_RBUTTONUP:
	{
		rDrag.right = GET_X_LPARAM(lParam);
		rDrag.bottom = GET_Y_LPARAM(lParam);

		TakeBitmapLock();
		// Move center without adjusting scale
		centre -= std::complex<double>((rDrag.right - rDrag.left)*scale, (rDrag.top - rDrag.bottom)*scale);
		GenerateOffsets();
		rDrag = { 0,0,0,0 };
		invalidate = true;
		cvInvalidate.notify_one();
		break;
	}

	case WM_MOUSEMOVE:
	{
		if (wParam & MK_LBUTTON)
		{
			InvalidateRect(hwnd, &rSelect, FALSE);
			rSelect.right = GET_X_LPARAM(lParam);
			rSelect.bottom = GET_Y_LPARAM(lParam);
			InvalidateRect(hwnd, &rSelect, FALSE);
		}
		else if (wParam & MK_RBUTTON)
		{
			rDrag.right = GET_X_LPARAM(lParam);
			rDrag.bottom = GET_Y_LPARAM(lParam);
			InvalidateRect(hwnd, NULL, FALSE);
		}

		SendDlgItemMessageA(hdlgStatus, IDC_CURSOR_X, WM_SETTEXT, 0, (LPARAM)DoubleToStr(GET_X_LPARAM(lParam) * scale + offsetX));
		SendDlgItemMessageA(hdlgStatus, IDC_CURSOR_Y, WM_SETTEXT, 0, (LPARAM)DoubleToStr(GET_Y_LPARAM(lParam) * -scale + offsetY));
		break;
	}

	case WM_SIZE:
	{
		if ((wParam == SIZE_MAXIMIZED) || (wParam == SIZE_RESTORED))
		{
			GetClientRect(hwnd, &rFractal);
			if ((rFractal.right - rFractal.left != bmFractal.bmWidth) ||
				(rFractal.bottom - rFractal.top != bmFractal.bmHeight))
			{
				TakeBitmapLock();
				invalidate = true;
				if (scale == 0.0)
				{
					scale = 3.0 / (rFractal.right - rFractal.left);
				}
				GenerateOffsets();
				cvInvalidate.notify_one();
			}
		}
		return 0;
	}

	case WM_PAINT:
	{
		hdc = BeginPaint(hwnd, &ps);
		bool drawn = false;

		if (hFractal)
		{
			if (bitmapLock.try_lock())
			{
				drawn = true;
				std::lock_guard<std::mutex> l(bitmapLock, std::adopt_lock);
				HDC hdcT = CreateCompatibleDC(hdc);
				SetStretchBltMode(hdc, COLORONCOLOR);
				SelectObject(hdcT, hFractal);
				BitBlt(hdc, rFractal.left + (rDrag.right - rDrag.left), rFractal.top + (rDrag.bottom - rDrag.top), rFractal.right - rFractal.left, rFractal.bottom - rFractal.top,
					hdcT, 0, 0, SRCCOPY);
				SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
				SelectObject(hdc, GetStockObject(WHITE_PEN));
				Rectangle(hdc, rSelect.left, rSelect.top, rSelect.right, rSelect.bottom);
				DeleteDC(hdcT);
				GdiFlush();
			}
		}

		EndPaint(hwnd, &ps);

		if (!drawn)
			InvalidateRect(hwnd, &ps.rcPaint, FALSE);

		return 0;
	};

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDM_FILE_NEW:
		{
			TakeBitmapLock();
			centre = { fJulia ? 0.0 : -0.5, 0 };
			scale = 3.0 / (rFractal.right - rFractal.left);
			GenerateOffsets();
			invalidate = true;
			cvInvalidate.notify_one();
			break;
		}

		case IDM_FILE_EXIT:
			SendMessage(hwnd, WM_CLOSE, 0, 0);
			break;

		case IDM_FILE_SAVE:
		{
			RenderMovie(hwnd);
			break;
		}

		case IDM_EDIT_COPY:
		{
			TakeBitmapLock();
			HDC hdcW = GetDC(hwnd);
			HDC hdcT = CreateCompatibleDC(NULL);
			HDC hdcC = CreateCompatibleDC(NULL);
			SelectObject(hdcT, hFractal);

			HBITMAP hClip = CreateCompatibleBitmap(hdcW, rFractal.right - rFractal.left, rFractal.bottom - rFractal.top);
			ReleaseDC(hwnd, hdcW);
			SelectObject(hdcC, hClip);

			BitBlt(hdcC, 0, 0, rFractal.right - rFractal.left, rFractal.bottom - rFractal.top,
				hdcT, 1, 1, SRCCOPY);
			DeleteDC(hdcT);
			DeleteDC(hdcC);
			OpenClipboard(hwnd);
			EmptyClipboard();
			SetClipboardData(CF_BITMAP, hClip);
			CloseClipboard();
			GdiFlush();
			break;
		};

		case IDM_FILE_ABOUT:
			DialogBox(hInstance, MAKEINTRESOURCE(IDD_ABOUT), hwnd, AboutDlgProc);
			break;

		default: goto WontHandle;
		};
		return 0;
	};

WontHandle:
	return DefWindowProc(hwnd, iMsg, wParam, lParam);
};

INT_PTR CALLBACK StatusDlgProc(HWND hwnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	switch (iMsg)
	{
	case WM_INITDIALOG:
		SendDlgItemMessage(hwnd, IDC_CENTER_X, EM_SETEVENTMASK, 0, ENM_CHANGE);
		SendDlgItemMessage(hwnd, IDC_CENTER_Y, EM_SETEVENTMASK, 0, ENM_CHANGE);
		SendDlgItemMessage(hwnd, IDC_SCALE, EM_SETEVENTMASK, 0, ENM_CHANGE);
		SendDlgItemMessage(hwnd, IDC_JULIA_X, EM_SETEVENTMASK, 0, ENM_CHANGE);
		SendDlgItemMessage(hwnd, IDC_JULIA_Y, EM_SETEVENTMASK, 0, ENM_CHANGE);

		SendDlgItemMessage(hwnd, IDC_JULIA, BM_SETCHECK, BST_UNCHECKED, 0);
		return true;

	case WM_COMMAND:
		switch (HIWORD(wParam))
		{
		case EN_KILLFOCUS:
		{
			bool fDirty = false;
			double val = GetDouble((HWND)lParam);
			TakeBitmapLock();

			switch (LOWORD(wParam))
			{
			case IDC_CENTER_X:
				fDirty = val != centre.real();
				centre = std::complex<double>(val, centre.imag());
				break;
			case IDC_CENTER_Y:
				fDirty = val != centre.imag();
				centre = std::complex<double>(centre.real(), val);
				break;
			case IDC_SCALE:
				fDirty = exp2(-val) != scale;
				scale = exp2(-val);
				break;
			case IDC_JULIA_X:
				fDirty = val != juliaSeed.real();
				juliaSeed = std::complex<double>(val, juliaSeed.imag());
				break;
			case IDC_JULIA_Y:
				fDirty = val != juliaSeed.imag();
				juliaSeed = std::complex<double>(juliaSeed.real(), val);
				break;
			}

			if (fDirty)
			{
				GenerateOffsets();
				invalidate = true;
				cvInvalidate.notify_one();
			}
			return false;
		}

		case BN_CLICKED:
			if (LOWORD(wParam) == IDC_JULIA)
			{
				TakeBitmapLock();

				fJulia = IsDlgButtonChecked(hwnd, IDC_JULIA);
				EnableWindow(GetDlgItem(hwnd, IDC_JULIA_X), fJulia);
				EnableWindow(GetDlgItem(hwnd, IDC_JULIA_Y), fJulia);

				if (fJulia)
				{
					// Going TO Julia:
					// Save view
					// Reset view to center of 0 and full screen
					// Seed is the old center
					juliaSeed = centre;
					centre = std::complex<double>(0, 0);
					scaleSave = scale;
					scale = 3.0 / (rFractal.right - rFractal.left);
					SendDlgItemMessageA(hdlgStatus, IDC_JULIA_X, WM_SETTEXT, 0, (LPARAM)DoubleToStr(juliaSeed.real()));
					SendDlgItemMessageA(hdlgStatus, IDC_JULIA_Y, WM_SETTEXT, 0, (LPARAM)DoubleToStr(juliaSeed.imag()));
				}
				else
				{
					// Going FROM Julia:
					// Restore view
					// Center is the old seed
					scale = scaleSave;
					centre = juliaSeed;
					SendDlgItemMessageA(hdlgStatus, IDC_JULIA_X, WM_SETTEXT, 0, (LPARAM)"");
					SendDlgItemMessageA(hdlgStatus, IDC_JULIA_Y, WM_SETTEXT, 0, (LPARAM)"");
				}
				GenerateOffsets();
				invalidate = true;
				cvInvalidate.notify_one();

			}
			return false;

		default:
			return false;
		}
	}

	return false;
}

constexpr wchar_t szAppName[] = L"MSet";
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, PSTR, int iCmdShow)
{
	MSG msg;
	WNDCLASSEX wndclass;

	hInstance = hInst;
	wndclass.cbSize = sizeof(wndclass);
	wndclass.style = CS_VREDRAW | CS_HREDRAW;
	wndclass.lpfnWndProc = WndProc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = hInstance;
	wndclass.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FRACTAL));
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wndclass.lpszMenuName = MAKEINTRESOURCE(IDR_MBAR);
	wndclass.lpszClassName = szAppName;
	wndclass.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_FRACTAL));

	InitCommonControls();
	RegisterClassEx(&wndclass);

	hwndFractal = CreateWindow(szAppName, L"Mandelbrot Explorer", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL, NULL, hInstance, NULL);

	ShowWindow(hwndFractal, iCmdShow);
	//UpdateWindow(hwndFractal);

	hdlgStatus = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_STATUSPANE), hwndFractal, StatusDlgProc);
	ShowWindow(hdlgStatus, iCmdShow);
	GenerateOffsets();

	HANDLE hThread = CreateThread(NULL, 0, CalculateFractal, NULL, 0, NULL);
	while (GetMessage(&msg, NULL, 0, 0))
	{
		if (!IsDialogMessage(hdlgStatus, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	TerminateThread(hThread, 0);
	return (int)msg.wParam;
};

#include "pch.h"
#include "SwitchButton.h"

#include <algorithm>
#include <memory>

namespace
{
    bool g_gdiplusInitialized = false;
    ULONG_PTR g_gdiplusToken = 0;

    std::unique_ptr<Gdiplus::Bitmap> LoadPngFromResource(UINT resId)
    {
        if (resId == 0)
            return nullptr;

        HINSTANCE hInst = AfxGetResourceHandle();
        HRSRC hRsrc = ::FindResource(hInst, MAKEINTRESOURCE(resId), _T("PNG"));
        if (!hRsrc)
            return nullptr;

        DWORD dwSize = ::SizeofResource(hInst, hRsrc);
        HGLOBAL hGlobal = ::LoadResource(hInst, hRsrc);
        if (!hGlobal || dwSize == 0)
            return nullptr;

        void* pData = ::LockResource(hGlobal);
        if (!pData)
            return nullptr;

        HGLOBAL hBuffer = ::GlobalAlloc(GMEM_MOVEABLE, dwSize);
        if (!hBuffer)
            return nullptr;

        void* pBuffer = ::GlobalLock(hBuffer);
        if (!pBuffer)
        {
            ::GlobalFree(hBuffer);
            return nullptr;
        }

        memcpy(pBuffer, pData, dwSize);
        ::GlobalUnlock(hBuffer);

        IStream* pStream = nullptr;
        if (FAILED(::CreateStreamOnHGlobal(hBuffer, TRUE, &pStream)))
        {
            ::GlobalFree(hBuffer);
            return nullptr;
        }

        std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromStream(pStream));
        pStream->Release();

        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok)
            return nullptr;

        return bmp;
    }
}


CSwitchButton::CSwitchButton()
    : m_state(SWITCH_OFF)
    , m_bgColor(GetSysColor(COLOR_3DFACE))
    , m_resOff(0)
    , m_resOn(0)
{
}

CSwitchButton::~CSwitchButton()
{
}

BEGIN_MESSAGE_MAP(CSwitchButton, CButton)
    // ON_WM_LBUTTONUP() // Let the parent handle the click logic via BN_CLICKED, or handle here if we want internal toggling
END_MESSAGE_MAP()

bool CSwitchButton::EnsureGdiPlus()
{
    if (g_gdiplusInitialized)
        return true;

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) == Gdiplus::Ok)
    {
        g_gdiplusInitialized = true;
    }
    return g_gdiplusInitialized;
}

void CSwitchButton::SetPngResources(UINT resOff, UINT resOn)
{
    m_resOff = resOff;
    m_resOn = resOn;
    m_bmpOff.reset();
    m_bmpOn.reset();
    Invalidate();
}

void CSwitchButton::SetBackgroundColor(COLORREF color)
{
    m_bgColor = color;
    Invalidate();
}

void CSwitchButton::SetSwitchState(SwitchState state)
{
    if (m_state != state)
    {
        m_state = state;
        Invalidate();
    }
}

void CSwitchButton::EnsureBitmaps()
{
    if (!EnsureGdiPlus())
        return;

    if (!m_bmpOff && m_resOff)
        m_bmpOff = LoadPngFromResource(m_resOff);
    if (!m_bmpOn && m_resOn)
        m_bmpOn = LoadPngFromResource(m_resOn);
}

Gdiplus::Bitmap* CSwitchButton::GetBitmapForState()
{
    EnsureBitmaps();

    if (m_state == SWITCH_OFF)
        return m_bmpOff.get();

    // WAITING and ON share the ON visual by default
    return m_bmpOn ? m_bmpOn.get() : m_bmpOff.get();
}

void CSwitchButton::OnLButtonUp(UINT nFlags, CPoint point)
{
    // Pass to base class to generate BN_CLICKED notification
    CButton::OnLButtonUp(nFlags, point);
}

void CSwitchButton::DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
    CRect rect = lpDrawItemStruct->rcItem;

    // Double buffer
    CMemDC memDC(*pDC, &rect);
    CDC& dc = memDC.GetDC();

    // Background fill (Parent bg color usually, but here we fill consistent with the app theme or transparent)
    // For now, assume a dark background or let's fill with a known color if needed.
    // Since we are custom drawing, we draw the pill shape.

    dc.FillSolidRect(&rect, m_bgColor);
    dc.SetBkMode(TRANSPARENT);

    Gdiplus::Bitmap* pBmp = GetBitmapForState();

    if (pBmp)
    {
        Gdiplus::Graphics graphics(dc.GetSafeHdc());
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        const int imgW = static_cast<int>(pBmp->GetWidth());
        const int imgH = static_cast<int>(pBmp->GetHeight());

        if (imgW > 0 && imgH > 0)
        {
            double scale = (std::min)(static_cast<double>(rect.Width()) / imgW, static_cast<double>(rect.Height()) / imgH);
            double drawW = imgW * scale;
            double drawH = imgH * scale;
            double offsetX = rect.left + (rect.Width() - drawW) / 2.0;
            double offsetY = rect.top + (rect.Height() - drawH) / 2.0;

            Gdiplus::RectF destRect(static_cast<Gdiplus::REAL>(offsetX), static_cast<Gdiplus::REAL>(offsetY),
                static_cast<Gdiplus::REAL>(drawW), static_cast<Gdiplus::REAL>(drawH));

            graphics.DrawImage(pBmp, destRect);

            if (m_state == SWITCH_WAITING)
            {
                Gdiplus::Color maskColor(120, 0, 0, 0);
                Gdiplus::SolidBrush maskBrush(maskColor);
                graphics.FillRectangle(&maskBrush, destRect);
            }
        }
    }
    else
    {
        // Fallback to a simple rectangle to avoid blank control if PNG failed
        dc.FillSolidRect(&rect, RGB(200, 200, 200));
    }
}

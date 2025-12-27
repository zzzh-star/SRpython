#pragma once
#include <afxwin.h>
#include <gdiplus.h>
#include <memory>

#pragma comment(lib, "gdiplus.lib")

class CSwitchButton : public CButton
{
public:
    CSwitchButton();
    virtual ~CSwitchButton();

    enum SwitchState {
        SWITCH_OFF = 0,
        SWITCH_WAITING,
        SWITCH_ON
    };

    void SetSwitchState(SwitchState state);
    SwitchState GetSwitchState() const { return m_state; }

    void SetPngResources(UINT resOff, UINT resOn);

    void SetBackgroundColor(COLORREF color);

protected:
    SwitchState m_state;
    COLORREF m_bgColor;
    UINT m_resOff;
    UINT m_resOn;

    std::unique_ptr<Gdiplus::Bitmap> m_bmpOff;
    std::unique_ptr<Gdiplus::Bitmap> m_bmpOn;

    static bool EnsureGdiPlus();
    void EnsureBitmaps();
    Gdiplus::Bitmap* GetBitmapForState();
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()
};

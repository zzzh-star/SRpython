#include "pch.h"
#include "SwitchButton.h"

CSwitchButton::CSwitchButton()
    : m_state(SWITCH_OFF)
{
}

CSwitchButton::~CSwitchButton()
{
}

BEGIN_MESSAGE_MAP(CSwitchButton, CButton)
    // ON_WM_LBUTTONUP() // Let the parent handle the click logic via BN_CLICKED, or handle here if we want internal toggling
END_MESSAGE_MAP()

void CSwitchButton::SetSwitchState(SwitchState state)
{
    if (m_state != state)
    {
        m_state = state;
        Invalidate();
    }
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

    dc.SetBkMode(TRANSPARENT);

    // Colors
    COLORREF clrBgOff = RGB(80, 80, 80);    // Dark Gray
    COLORREF clrBgOn = RGB(46, 125, 50);    // Green
    COLORREF clrBgWait = RGB(255, 140, 0);  // Orange
    
    COLORREF clrKnob = RGB(240, 240, 240);  // White/Light Gray
    COLORREF clrBorder = RGB(60, 60, 60);

    COLORREF curBg = clrBgOff;
    if (m_state == SWITCH_ON) curBg = clrBgOn;
    else if (m_state == SWITCH_WAITING) curBg = clrBgWait;

    // Draw Pill Shape
    CPen pen(PS_SOLID, 1, clrBorder);
    CBrush brush(curBg);
    CPen* pOldPen = dc.SelectObject(&pen);
    CBrush* pOldBrush = dc.SelectObject(&brush);

    int radius = rect.Height() / 2;
    dc.RoundRect(rect, CPoint(radius * 2, radius * 2));

    dc.SelectObject(pOldPen);
    dc.SelectObject(pOldBrush);

    // Draw Knob
    // Calculate Knob Position
    // Off: Left, On: Right. Waiting: Middle? Or Keep strictly Toggle behavior visually?
    // "Switch" implies binary position. 
    // If waiting, we might keep it at the "target" position or pulsing.
    // Let's assume: OFF -> Left, ON -> Right. Waiting -> Maybe keep at the "To Be" position or Middle?
    // Request says: "Switch turns green once speed set success". "Once closed... switch turns off".
    // This implies the switch *position* might toggle immediately or reflect state.
    // Let's align knob with state: OFF=Left, ON=Right. WAITING=Left (if starting) or Right (if stopping)?
    // Actually, usually a switch moves to ON immediately by user interaction, but color indicates status.
    // But since this is a complex state machine, let's put Knob on Left for OFF, Right for ON.
    // For WAITING, let's keep it in the *target* state position but with Orange color?
    // Or simpler: Left = OFF, Right = ON/WAITING(Startup), Left=WAITING(Shutdown)?
    
    // User logic: "Once open switch: execute start... wait... then speed... then Green".
    // This implies during startup wait, it should look "Active" (Right) but Orange.
    // During shutdown wait, it should look "Inactive" (Left) but Orange? 
    // Or maybe just use the state.
    
    int knobPadding = 2;
    int knobSize = rect.Height() - 2 * knobPadding;
    CRect rectKnob;

    if (m_state == SWITCH_OFF)
    {
        // Left
        rectKnob.SetRect(rect.left + knobPadding, rect.top + knobPadding, rect.left + knobPadding + knobSize, rect.top + knobPadding + knobSize);
    }
    else
    {
        // Right (ON or WAITING treated as 'Active' position for now, unless we want a middle state)
        rectKnob.SetRect(rect.right - knobPadding - knobSize, rect.top + knobPadding, rect.right - knobPadding, rect.top + knobPadding + knobSize);
    }

    CPen penKnob(PS_SOLID, 1, RGB(200, 200, 200));
    CBrush brushKnob(clrKnob);
    pOldPen = dc.SelectObject(&penKnob);
    pOldBrush = dc.SelectObject(&brushKnob);

    dc.Ellipse(rectKnob);

    dc.SelectObject(pOldPen);
    dc.SelectObject(pOldBrush);
    
    // Optional: Draw the small dash on the "On" part background if space permits, like the image?
    // The image shows a small dash on the left when ON.
    if (m_state == SWITCH_ON || m_state == SWITCH_WAITING)
    {
        // Draw a small dash/rect on the left side
        CRect rcMark;
        int mw = 8; int mh = 3;
        int mx = rect.left + radius - mw/2; 
        int my = rect.top + rect.Height()/2 - mh/2;
        rcMark.SetRect(mx, my, mx+mw, my+mh);
        dc.FillSolidRect(&rcMark, RGB(200,255,200)); // Light Greenish mark
    }
}

#pragma once
#include <afxwin.h>

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

protected:
    SwitchState m_state;
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()
};

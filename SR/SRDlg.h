#pragma once
#include "MotorManager.h"
#include "dhdc.h"
#include <vector>
#include "ChartCtrl.h"
#include "ChartLineSerie.h"
#include "SwitchButton.h"
#include <opencv2/opencv.hpp>

constexpr double kPI = 3.1415926;
constexpr int kNumSegments = 4;
constexpr double kJointThickness = 5.0;
constexpr double kJointSpacing = 3.0;
constexpr double kDriveRadius = 5.5;
#define REFRESH_INTERVAL  0.1  

class CSRDlg : public CDialogEx
{
public:
	CSRDlg(CWnd* pParent = nullptr);
	virtual ~CSRDlg();

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_SR_DIALOG };
#endif

	CEdit m_editMotorStatus;
	CEdit m_editHapticStatus;
	CEdit m_editMasterPos;
	CEdit m_editMasterEnc;
	CEdit m_editMasterForce;
	CEdit m_editPose;
	CEdit m_editBend;
	CEdit m_editGripAngle;
	CEdit m_editGripMotor;

	double pos_x_real = 0.0;
	double pos_y_real = 0.0;
	double pos_z_real = 0.0;

	// Chart
	CChartCtrl m_ChartCtrl;
	CChartLineSerie* m_pLineSeries[3];

	// Camera
	cv::VideoCapture m_camera;
	cv::Mat m_cameraFrame;
	CStatic m_picCamera;

	// Switch Button
	CSwitchButton m_btnMotorSwitch;
	// 0: OFF, 1: STARTING (Wait Start), 2: CONFIGURING (Wait Speed), 3: ON, 4: ZEROING (Wait Zero), 5: STOPPING (Wait Disable)
	int m_nMotorSwitchState = 0; 
	// Timer for async operations
	UINT_PTR m_nMotorTimer = 0;

	// UI Customization Resources
	CFont m_fontTitle;      // Header Title Font
	CFont m_fontMain;       // Main UI Font
	CFont m_fontLabel;      // Label Font (Bold)
	CFont m_fontSidebarBtn; // Sidebar Button Font

	// --- Theme Colors ---
	COLORREF m_clrAppBg;
	COLORREF m_clrHdrTop, m_clrHdrBottom, m_clrHdrText, m_clrHdrLine;
	
	// Sidebar
	COLORREF m_clrSidebarBg, m_clrSidebarText;
	COLORREF m_clrSideCardTitle, m_clrSideCardBg, m_clrSideCardBorder;
	
	// Main Cards
	COLORREF m_clrMainCardBg, m_clrMainCardBorder;
	COLORREF m_clrMainText, m_clrSubText;

	// Status
	COLORREF m_clrOkGreen, m_clrDangerRed;

	// --- Theme Brushes ---
	CBrush m_brushAppBg;
	CBrush m_brushSidebarBg;
	CBrush m_brushMainCardBg;
	
	// Layout Helpers
	static const int kHeaderHeight = 78;
	static const int kSidebarWidth = 210;
	static const int kMargin = 18;
	static const int kGap = 16;
	static const int kRadius = 14;
	
	// Refined Layout Params
	static const int kSidebarPad = 14;
	static const int kCardGap = 14;

	// Layout Rects (for OnPaint)
	CRect m_rectSidebar;
	CRect m_rectMainArea;
	CRect m_rectCardMotor, m_rectCardHaptic;
	CRect m_rectCardCamera, m_rectCardMaster, m_rectCardRobot, m_rectCardChart;

	void LayoutUI();
	void DrawRoundedRectFillBorder(CDC& dc, CRect rc, int radius, COLORREF fill, COLORREF border);
	void DrawCardWithTitle(CDC& dc, CRect rc, int radius, CString title, COLORREF titleBg, COLORREF bodyBg, COLORREF border, COLORREF titleText);
	void DrawMainCardTitle(CDC& dc, CRect rc, CString title); // New helper for Main cards
	CWnd* FindStaticByText(const CString& text);

protected:
	virtual void DoDataExchange(CDataExchange* pDX);

	double px, py, pz;
	double fx, fy, fz;

	double ref_px = 0.0;
	double ref_py = 0.0;
	double ref_pz = 0.0;

	double rel_px = 0.0;
	double rel_py = 0.0;
	double rel_pz = 0.0;

	int enc[DHD_MAX_DOF];
	int encCount;
	int offset_enc0 = 0;

	double pos_x = 0, pos_y = 0, pos_z = 0;
	double delta_L1 = 0, delta_L2 = 0;
	double bend_angle_end = 0;
	double bend_direction_end = 0;

	double t0, t1;

	BOOL maxon_state = FALSE;
	int  done = 0;
	BOOL Ning = false;
	BOOL motor_flag = FALSE;

	MotorManager* m_pMotorManager;

	int maxon5_position = 0, maxon4_position = 0, maxon3_position = 0, maxon2_position = 0, maxon1_position = 0;

protected:
	HICON m_hIcon;

	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnDestroy();

	// Add OnCtlColor for custom control coloring
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);

	afx_msg void OnTimer(UINT_PTR nIDEvent);

	DECLARE_MESSAGE_MAP()

public:
	afx_msg void OnBnClickedOk();

	afx_msg void OnClickedButtonStartM();
	afx_msg void OnClickedButtonShutM();
	afx_msg void OnClickedButtonSpeedM();
	afx_msg void OnClickedButtonZeroM();

	afx_msg void OnClickedButtonStartH();
	afx_msg void OnClickedButtonZeroH();
	afx_msg void OnClickedButtonShutH();
	afx_msg void OnClickedMotorSwitch();

	// Haptic Switch Helpers and Handler
	CSwitchButton m_btnHapticSwitch;
	int m_nHapticSwitchState = 0; // 0: OFF, 1: ON
	afx_msg void OnClickedHapticSwitch();

	bool StartHapticDevice();
	bool ZeroHapticDevice();
	void StopHapticDevice();
};

// Add to class declaration
// afx_msg void OnClickedMotorSwitch(); 
// We need to inject this into SRDlg.h, let's use sed or replace

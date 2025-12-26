#pragma once
#include "MotorManager.h"
#include "dhdc.h"
#include <vector>
#include "ChartCtrl.h"
#include "ChartLineSerie.h"
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
	CEdit m_editRobotInfo;

	// Chart
	CChartCtrl m_ChartCtrl;
	CChartLineSerie* m_pLineSeries[3];

	// Camera
	cv::VideoCapture m_camera;
	cv::Mat m_cameraFrame;
	CStatic m_picCamera;

	// UI Customization Resources
	CFont m_fontTitle;      // 标题字体
	CFont m_fontMain;       // 主界面字体
	CFont m_fontLabel;      // 标签字体(加粗)

	// Colors
	COLORREF m_clrHeader;   // 标题栏背景色
	COLORREF m_clrBg;       // 全局背景色
	COLORREF m_clrTextTitle;// 标题文字颜色
	COLORREF m_clrTextNormal;// 普通文字颜色

	CBrush m_brushBg;       // 背景画刷
	CBrush m_brushHeader;   // 标题栏画刷

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
	double m_startTime = 0.0;

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
};
// SRDlg.cpp : implementation file
//v1.4 Layout Polish: Aspect Ratio Camera, Adaptive Window Size, Better Labels

#include "pch.h"
#include "framework.h"
#include "SR.h"
#include "SRDlg.h"
#include "afxdialogex.h"

#ifdef _WIN64
#pragma comment(lib, "EposCmd64.lib")
#pragma comment(lib, "dhdms64.lib")
#else
#pragma comment(lib, "EposCmd.lib")
#pragma comment(lib, "dhdms.lib")
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <cmath>
#include <iostream>
#include <algorithm>
#include <vector>

#include <unsupported/Eigen/CXX11/Tensor> 
#include <Eigen/Dense> 

using namespace Eigen;
using namespace std;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

LARGE_INTEGER iFreq;
LARGE_INTEGER iBegTime;
LARGE_INTEGER iStopTime;
int state;

// --- Helper Functions ---
void DrawGradient(CDC* pDC, CRect rect, COLORREF cTop, COLORREF cBottom)
{
	int h = rect.Height();
	int w = rect.Width();

	double r1 = GetRValue(cTop), g1 = GetGValue(cTop), b1 = GetBValue(cTop);
	double r2 = GetRValue(cBottom), g2 = GetGValue(cBottom), b2 = GetBValue(cBottom);

	for (int i = 0; i < h; i++)
	{
		double factor = (double)i / (double)h;
		COLORREF c = RGB(
			(BYTE)(r1 + (r2 - r1) * factor),
			(BYTE)(g1 + (g2 - g1) * factor),
			(BYTE)(b1 + (b2 - b1) * factor)
		);
		pDC->FillSolidRect(rect.left, rect.top + i, w, 1, c);
	}
}

// Helper to draw OpenCV Mat to CStatic with Aspect Ratio Preservation (Letterboxing)
void DrawMatToPic(cv::Mat& img, CStatic& pic)
{
	if (img.empty()) return;
	if (!pic.GetSafeHwnd()) return;

	CRect rect;
	pic.GetClientRect(&rect);

	if (rect.IsRectEmpty()) return;

	// Fill background with black (letterboxing)
	CDC* pDC = pic.GetDC();
	if (pDC)
	{
		pDC->FillSolidRect(&rect, RGB(0, 0, 0));
		
		// Calculate aspect-preserving dimensions
		double srcAspect = (double)img.cols / img.rows;
		double dstAspect = (double)rect.Width() / rect.Height();
		
		int dstW = rect.Width();
		int dstH = rect.Height();
		int dstX = 0;
		int dstY = 0;
		
		if (srcAspect > dstAspect) {
			// Source is wider than destination: fit width
			dstH = (int)(dstW / srcAspect);
			dstY = (rect.Height() - dstH) / 2;
		} else {
			// Source is taller than destination: fit height
			dstW = (int)(dstH * srcAspect);
			dstX = (rect.Width() - dstW) / 2;
		}

		cv::Mat resized;
		cv::resize(img, resized, cv::Size(dstW, dstH));

		BITMAPINFO bitInfo;
		memset(&bitInfo, 0, sizeof(BITMAPINFO));
		bitInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitInfo.bmiHeader.biWidth = resized.cols;
		bitInfo.bmiHeader.biHeight = -resized.rows; 
		bitInfo.bmiHeader.biPlanes = 1;
		bitInfo.bmiHeader.biBitCount = 24;
		bitInfo.bmiHeader.biCompression = BI_RGB;

		::StretchDIBits(pDC->GetSafeHdc(), dstX, dstY, dstW, dstH,
			0, 0, resized.cols, resized.rows,
			resized.data, &bitInfo, DIB_RGB_COLORS, SRCCOPY);
		
		pic.ReleaseDC(pDC);
	}
}

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);

protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

// ==========================================================================
// CSRDlg Implementation
// ==========================================================================

CSRDlg::CSRDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_SR_DIALOG, pParent), m_pMotorManager(nullptr)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_pMotorManager = new MotorManager();

	// --- Theme Color Initialization (Refined Soft Light Theme) ---

	// App BG (soft light gray)
	m_clrAppBg      = RGB(235, 239, 244);   // #EBEFF4

	// Header
	m_clrHdrTop    = RGB(16, 45, 70);
	m_clrHdrBottom = RGB(24, 72, 110);
	m_clrHdrText   = RGB(255, 255, 255);
	m_clrHdrLine   = RGB(0, 188, 212);

	// System Control (Blue-Grey Series)
	m_clrSysHdr     = RGB(35, 45, 55);   // Softer Top
	m_clrSysBody    = RGB(52, 62, 72);   // Deep Gray Body
	m_clrSysBorder  = RGB(80, 92, 104);  // Muted Border
	m_clrSysText    = RGB(235, 240, 245);

	// Main Cards (White + Shadow)
	m_clrCardBg     = RGB(255, 255, 255);   // Pure White
	m_clrCardBorder = RGB(214, 222, 231);   // Faint Border
	m_clrShadow     = RGB(200, 208, 218);   // Soft Shadow
	m_clrMainText   = RGB(30, 35, 40);
	m_clrSubText    = RGB(120, 130, 140);

	// Status
	m_clrOkGreen    = RGB(46, 204, 113);
	m_clrDangerRed  = RGB(255, 59, 48);

	// Create Brushes
	m_brushAppBg.CreateSolidBrush(m_clrAppBg);
	m_brushCardBg.CreateSolidBrush(m_clrCardBg);

	px = py = pz = 0.0;
	fx = fy = fz = 0.0;
	ref_px = ref_py = ref_pz = 0.0;
	rel_px = rel_py = rel_pz = 0.0;

	Ning = false;
	motor_flag = false;
	done = 1;
	t0 = 0;
	t1 = 0;
}

CSRDlg::~CSRDlg()
{
	if (m_camera.isOpened()) m_camera.release();
	dhdClose();

	if (m_pMotorManager)
	{
		delete m_pMotorManager;
		m_pMotorManager = nullptr;
	}

	m_brushAppBg.DeleteObject();
	m_brushCardBg.DeleteObject();
}

void CSRDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT_MOTOR_STATUS, m_editMotorStatus);
	DDX_Control(pDX, IDC_EDIT_HAPTIC_STATUS, m_editHapticStatus);
	DDX_Control(pDX, IDC_EDIT_MASTER_POS, m_editMasterPos);
	DDX_Control(pDX, IDC_EDIT_MASTER_ENC, m_editMasterEnc);
	DDX_Control(pDX, IDC_EDIT_MASTER_FORCE, m_editMasterForce);
	DDX_Control(pDX, IDC_EDIT_POSE, m_editPose);
	DDX_Control(pDX, IDC_EDIT_BEND, m_editBend);
	DDX_Control(pDX, IDC_EDIT_GRIP_ANGLE, m_editGripAngle);
	DDX_Control(pDX, IDC_EDIT_GRIP_MOTOR, m_editGripMotor);
	DDX_Control(pDX, IDC_STATIC_CAMERA, m_picCamera);
}

BEGIN_MESSAGE_MAP(CSRDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_CTLCOLOR()
	ON_WM_QUERYDRAGICON()
	ON_WM_SIZE()
	ON_WM_TIMER()
	ON_BN_CLICKED(IDOK, &CSRDlg::OnBnClickedOk)
	ON_BN_CLICKED(IDC_BUTTON_STARTM, &CSRDlg::OnClickedButtonStartM)
	ON_BN_CLICKED(IDC_BUTTON_SHUTM, &CSRDlg::OnClickedButtonShutM)
	ON_BN_CLICKED(IDC_BUTTON_SPEEDM, &CSRDlg::OnClickedButtonSpeedM)
	ON_BN_CLICKED(IDC_BUTTON_ZEROM, &CSRDlg::OnClickedButtonZeroM)
	ON_BN_CLICKED(IDC_BUTTON_STARTH, &CSRDlg::OnClickedButtonStartH)
	ON_BN_CLICKED(IDC_BUTTON_ZEROH, &CSRDlg::OnClickedButtonZeroH)
	ON_BN_CLICKED(IDC_BUTTON_SHUTH, &CSRDlg::OnClickedButtonShutH)
END_MESSAGE_MAP()

BOOL CSRDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// Set Window Text
	SetWindowText(_T("消化道手术机器人控制系统"));

	// Initialize Fonts (Larger Sizes)
	m_fontTitle.CreateFont(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0, ANSI_CHARSET, 
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
	
	m_fontMain.CreatePointFont(95, _T("Segoe UI"));                 // 9.5pt
	m_fontLabel.CreatePointFont(120, _T("Segoe UI Semibold"));      // 12pt Card Titles

	SetFont(&m_fontMain);

	// Modernize Buttons
	UINT btnIds[] = { IDC_BUTTON_STARTM, IDC_BUTTON_SHUTM, IDC_BUTTON_SPEEDM, IDC_BUTTON_ZEROM,
					  IDC_BUTTON_STARTH, IDC_BUTTON_ZEROH, IDC_BUTTON_SHUTH, IDOK };
	for (UINT id : btnIds) {
		CWnd* pBtn = GetDlgItem(id);
		if (pBtn) {
			::SetWindowTheme(pBtn->GetSafeHwnd(), L"Explorer", NULL);
			pBtn->SetFont(&m_fontMain);
		}
	}

	// Hide old GroupBoxes and Titles
	const TCHAR* gbTitles[] = { _T("电机控制"), _T("主手控制"), _T("摄像头画面"), _T("控制参数"), _T("末端力实时曲线"), _T("Motor"), _T("Haptic"), _T("Camera View"), _T("Master Param"), _T("Robot Param"), _T("Force Feedback (N)"), NULL };
	for (int i=0; gbTitles[i]; ++i) {
		CWnd* pWnd = FindStaticByText(gbTitles[i]);
		if (pWnd) pWnd->ShowWindow(SW_HIDE);
	}
	// Hide static backgrounds
	UINT oldStaticIds[] = { 1019, 1020, 1021, 1022, 1023, 1024, 1025 };
	for(UINT id : oldStaticIds) {
		CWnd* pWnd = GetDlgItem(id);
		if(pWnd) pWnd->ShowWindow(SW_HIDE);
	}

	// Setup System Menu
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);
	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	t0 = dhdGetTime();

	m_editMotorStatus.SetWindowText(_T("Disconnected"));
	m_editHapticStatus.SetWindowText(_T("Disconnected"));

	// Camera Init
	if (m_camera.open(0)) { } else { m_camera.open(1); }
	SetTimer(2, 50, NULL);

	// Chart Init
	if (!m_ChartCtrl.GetSafeHwnd())
	{
		CRect rect(0,0,100,100); 
		m_ChartCtrl.Create(this, rect, 2000, WS_CHILD | WS_VISIBLE);
		m_ChartCtrl.EnableRefresh(true);
		m_ChartCtrl.GetTitle()->AddString(_T("")); 
		m_ChartCtrl.SetBackColor(RGB(255, 255, 255));
		m_ChartCtrl.SetBorderColor(m_clrCardBorder); 

		CChartStandardAxis* pBottomAxis = m_ChartCtrl.CreateStandardAxis(CChartCtrl::BottomAxis);
		pBottomAxis->SetMinMax(0, 100);
		pBottomAxis->SetTextColor(m_clrSubText);

		CChartStandardAxis* pLeftAxis = m_ChartCtrl.CreateStandardAxis(CChartCtrl::LeftAxis);
		pLeftAxis->SetTextColor(m_clrSubText);

		// Fx: Blue
		m_pLineSeries[0] = m_ChartCtrl.CreateLineSerie();
		m_pLineSeries[0]->SetColor(RGB(11, 92, 173)); 
		m_pLineSeries[0]->SetWidth(2);
		m_pLineSeries[0]->SetName(_T("Fx"));

		// Fy: Green
		m_pLineSeries[1] = m_ChartCtrl.CreateLineSerie();
		m_pLineSeries[1]->SetColor(RGB(46, 204, 113));
		m_pLineSeries[1]->SetWidth(2);
		m_pLineSeries[1]->SetName(_T("Fy"));

		// Fz: Orange
		m_pLineSeries[2] = m_ChartCtrl.CreateLineSerie();
		m_pLineSeries[2]->SetColor(RGB(245, 165, 36));
		m_pLineSeries[2]->SetWidth(2);
		m_pLineSeries[2]->SetName(_T("Fz"));
	}
	
	// Initial Window Size: smaller and comfortable (do NOT fill the screen)
	CRect work;
	SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);

	int workW = work.Width();
	int workH = work.Height();

	// ~70% of work area
	int targetW = (int)(workW * 0.70);
	int targetH = (int)(workH * 0.72);

	// caps (prevent too large / too small)
	targetW = max(980, min(targetW, 1200));
	targetH = max(660, min(targetH, 820));

	SetWindowPos(NULL, 0, 0, targetW, targetH, SWP_NOMOVE | SWP_NOZORDER);
	CenterWindow();

	LayoutUI();

	return TRUE;
}

void CSRDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// ==========================================================================
// Layout & Drawing
// ==========================================================================

void CSRDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);
	if (GetSafeHwnd()) LayoutUI();
}

CWnd* CSRDlg::FindStaticByText(const CString& text)
{
	CWnd* pWnd = GetWindow(GW_CHILD);
	while (pWnd)
	{
		CString str;
		pWnd->GetWindowText(str);
		if (str == text) return pWnd;
		pWnd = pWnd->GetNextWindow(GW_HWNDNEXT);
	}
	return nullptr;
}

void CSRDlg::LayoutUI()
{
	CRect clientRect;
	GetClientRect(&clientRect);
	if (clientRect.IsRectEmpty()) return;

	int width = clientRect.Width();
	int height = clientRect.Height();
	
	// Calc content area (below header)
	CRect content = clientRect;
	content.top += kHeaderHeight;
	content.DeflateRect(kMargin, kMargin);
	
	// --- Height Negotiation ---
	int minSysH = kTitleH + kPad + 8 * kBtnH + 7 * kBtnGap + kPad; 
	int minForceH = 200;
	
	int availH = content.Height();
	int forceH = max(minForceH, availH / 3);
	
	int topH = availH - (forceH + kGap);
	if (topH < minSysH) {
		int shortage = minSysH - topH;
		forceH = max(minForceH, forceH - shortage);
		topH = availH - (forceH + kGap);
	}
	
	// --- Bottom Area: Force Chart ---
	m_rectCardChart.SetRect(content.left, content.top + topH + kGap, content.right, content.bottom);
	
	// --- Top Area Split: Left (SysCtrl) vs Right (Camera+Params) ---
	int sysCtrlWidth = max(280, int(content.Width() * 0.28));
	
	m_rectCardSysCtrl.SetRect(content.left, content.top, content.left + sysCtrlWidth, content.top + topH);
	
	CRect rightCol = CRect(m_rectCardSysCtrl.right + kGap, content.top, content.right, content.top + topH);
	
	// --- Right Col Split: Top (Camera) vs Bottom (Params) ---
	int minCamH = 240; // keep camera view not too short -> less letterbox bars
	int camH = max(minCamH, int(rightCol.Height() * 0.40)); // reduce blank space
	int minParamH = 160;
	if (rightCol.Height() - camH - kGap < minParamH) {
		camH = rightCol.Height() - minParamH - kGap;
	}
	
	m_rectCardCamera.SetRect(rightCol.left, rightCol.top, rightCol.right, rightCol.top + camH);
	
	CRect paramRow = CRect(rightCol.left, m_rectCardCamera.bottom + kGap, rightCol.right, rightCol.bottom);
	
	// --- Param Row Split: Master vs Robot ---
	int masterW = (paramRow.Width() - kGap) / 2;
	
	m_rectCardMaster.SetRect(paramRow.left, paramRow.top, paramRow.left + masterW, paramRow.bottom);
	m_rectCardRobot.SetRect(m_rectCardMaster.right + kGap, paramRow.top, paramRow.right, paramRow.bottom);
	
	// =========================================================================================
	// Move Controls
	// =========================================================================================
	
	// 1. System Control Card (8 buttons vertically)
	{
		int startY = m_rectCardSysCtrl.top + kTitleH + kPad;
		int btnW = m_rectCardSysCtrl.Width() - 2 * kPad;
		
		UINT btnIds[] = { 
			IDC_BUTTON_STARTM, IDC_BUTTON_SPEEDM, IDC_BUTTON_ZEROM, IDC_BUTTON_SHUTM, 
			IDC_BUTTON_STARTH, IDC_BUTTON_ZEROH, IDC_BUTTON_SHUTH 
		};
		int cy = startY;
		
		// Calc exit button pos to ensure no overlap
		int yExit = m_rectCardSysCtrl.bottom - kPad - kBtnH;
		int yMax = yExit - kBtnGap; // other buttons must stay above this

		for(UINT id : btnIds) {
			if (cy + kBtnH > yMax) break; // prevent overlap with Exit
			
			CWnd* p = GetDlgItem(id);
			if(p) p->SetWindowPos(NULL, m_rectCardSysCtrl.left + kPad, cy, btnW, kBtnH, SWP_NOZORDER);
			cy += (kBtnH + kBtnGap);
		}
		
		// Exit button pinned at the very bottom
		CWnd* pExit = GetDlgItem(IDCANCEL);
		if (pExit) {
			int x = m_rectCardSysCtrl.left + kPad;
			pExit->SetWindowPos(NULL, x, yExit, btnW, kBtnH, SWP_NOZORDER);
		}
	}
	
	// 2. Camera Card (Centered with Aspect Ratio 16:9)
	if(m_picCamera.GetSafeHwnd()) {
		CRect area = m_rectCardCamera;
		area.top += kTitleH;
		area.DeflateRect(2, 2);

		const double aspect = 16.0 / 9.0;
		int aw = area.Width();
		int ah = area.Height();

		int w = aw;
		int h = (int)std::round(w / aspect);
		if (h > ah) { h = ah; w = (int)std::round(h * aspect); }

		int x = area.left + (aw - w) / 2;
		int y = area.top + (ah - h) / 2;

		m_picCamera.SetWindowPos(NULL, x, y, w, h, SWP_NOZORDER);
	}
	
	// 3. Master Param Card
	{
		int labelW = 110;  // was 70, too small for Chinese labels
		int rowH = 24;
		int startY = m_rectCardMaster.top + kTitleH + 10;
		int cxL = m_rectCardMaster.left + kPad;
		int cxE = cxL + labelW + 10;
		int cwE = m_rectCardMaster.right - kPad - cxE;
		int cy = startY;
		
		struct Item { const TCHAR* l; UINT id; };
		Item items[] = {
			{_T("电机:"), IDC_EDIT_MOTOR_STATUS},
			{_T("主手:"), IDC_EDIT_HAPTIC_STATUS},
			{_T("主手位姿:"), IDC_EDIT_MASTER_POS},
			{_T("主手编码:"), IDC_EDIT_MASTER_ENC},
			{_T("末端力:"), IDC_EDIT_MASTER_FORCE}
		};
		for(auto& it : items) {
			CWnd* pL = FindStaticByText(it.l);
			if(!pL && _tcscmp(it.l, _T("主手编码:"))==0) pL = FindStaticByText(_T("主手编码器:"));
			
			if(pL) pL->SetWindowPos(NULL, cxL, cy+2, labelW, 18, SWP_NOZORDER);
			CWnd* pE = GetDlgItem(it.id);
			if(pE) pE->SetWindowPos(NULL, cxE, cy, cwE, 20, SWP_NOZORDER);
			cy += rowH;
		}
	}
	
	// 4. Robot Param Card
	{
		int labelW = 110;  // was 70
		int rowH = 24;
		int startY = m_rectCardRobot.top + kTitleH + 10;
		int cxL = m_rectCardRobot.left + kPad;
		int cxE = cxL + labelW + 10;
		int cwE = m_rectCardRobot.right - kPad - cxE;
		int cy = startY;
		
		struct Item { const TCHAR* l; UINT id; };
		Item items[] = {
			{_T("空间位姿:"), IDC_EDIT_POSE},
			{_T("弯曲电机:"), IDC_EDIT_BEND},
			{_T("夹钳角度:"), IDC_EDIT_GRIP_ANGLE},
			{_T("夹钳电机:"), IDC_EDIT_GRIP_MOTOR}
		};
		for(auto& it : items) {
			CWnd* pL = FindStaticByText(it.l);
			if(pL) pL->SetWindowPos(NULL, cxL, cy+2, labelW, 18, SWP_NOZORDER);
			CWnd* pE = GetDlgItem(it.id);
			if(pE) pE->SetWindowPos(NULL, cxE, cy, cwE, 20, SWP_NOZORDER);
			cy += rowH;
		}
	}
	
	// 5. Force Chart Card
	if (m_ChartCtrl.GetSafeHwnd()) {
		CRect chartArea = m_rectCardChart;
		chartArea.top += kTitleH;
		chartArea.DeflateRect(kPad, kPad);
		m_ChartCtrl.SetWindowPos(NULL, chartArea.left, chartArea.top, chartArea.Width(), chartArea.Height(), SWP_NOZORDER);
	}
	
	Invalidate(); 
}

void CSRDlg::DrawRoundedRectFillBorder(CDC& dc, CRect rc, int radius, COLORREF fill, COLORREF border)
{
	CPen pen(PS_SOLID, 1, border);
	CBrush brush(fill);
	CPen* oldPen = dc.SelectObject(&pen);
	CBrush* oldBrush = dc.SelectObject(&brush);
	dc.RoundRect(rc, CPoint(radius, radius));
	dc.SelectObject(oldPen);
	dc.SelectObject(oldBrush);
}

void CSRDlg::DrawSysControlCard(CDC& dc, CRect rc)
{
	// Shadow
	CRect shadow = rc;
	shadow.OffsetRect(0, 4);
	DrawRoundedRectFillBorder(dc, shadow, kRadius, m_clrShadow, m_clrShadow);

	// D1) Draw Body (Deep Gray Rounded)
	DrawRoundedRectFillBorder(dc, rc, kRadius, m_clrSysBody, m_clrSysBorder);
	
	// D2) Draw Header (Deep Black, Top Rounded only)
	CRect rcHeader = rc;
	rcHeader.bottom = rcHeader.top + kTitleH;
	
	CPen pen(PS_SOLID, 1, m_clrSysBorder);
	CBrush brush(m_clrSysHdr);
	CPen* oldPen = dc.SelectObject(&pen);
	CBrush* oldBrush = dc.SelectObject(&brush);
	dc.RoundRect(rcHeader, CPoint(kRadius, kRadius));
	
	// Fix bottom corners
	CRect bottomHalf = rcHeader;
	bottomHalf.top += kRadius; 
	dc.FillSolidRect(&bottomHalf, m_clrSysHdr);
	
	dc.SelectObject(oldPen);
	dc.SelectObject(oldBrush);
	
	// D3) Draw Title
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(m_clrSysText);
	dc.SelectObject(&m_fontLabel);
	
	CRect rTitle = rcHeader;
	rTitle.left += 12;
	rTitle.top += 7; 
	dc.DrawText(_T("系统控制"), &rTitle, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void CSRDlg::DrawMainCardTitle(CDC& dc, CRect rc, CString title)
{
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(m_clrMainText);
	dc.SelectObject(&m_fontLabel);
	CRect rTitle = rc;
	rTitle.top += 10;
	rTitle.left += 14;
	rTitle.bottom = rTitle.top + 26; 
	dc.DrawText(title, &rTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void CSRDlg::DrawCardWithTitle(CDC& dc, CRect rc, int radius, CString title, COLORREF bg, COLORREF border, COLORREF text)
{
	// 1) Shadow
	CRect shadow = rc;
	shadow.OffsetRect(0, 4);
	DrawRoundedRectFillBorder(dc, shadow, radius, m_clrShadow, m_clrShadow);

	// 2) Card
	DrawRoundedRectFillBorder(dc, rc, radius, bg, border);

	// 3) Title text
	DrawMainCardTitle(dc, rc, title);

	// 4) Divider line
	CPen pen(PS_SOLID, 1, RGB(230, 235, 240));
	CPen* oldPen = dc.SelectObject(&pen);
	dc.MoveTo(rc.left + 12, rc.top + kTitleH);
	dc.LineTo(rc.right - 12, rc.top + kTitleH);
	dc.SelectObject(oldPen);
}

void CSRDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);
		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CPaintDC dc(this);
		CRect rect;
		GetClientRect(&rect);

		// 1. App Background
		dc.FillSolidRect(&rect, m_clrAppBg);

		// 2. Header
		CRect headerRect = rect;
		headerRect.bottom = kHeaderHeight;
		DrawGradient(&dc, headerRect, m_clrHdrTop, m_clrHdrBottom);
		dc.FillSolidRect(0, headerRect.bottom - 2, rect.Width(), 2, m_clrHdrLine);
		
		CFont* old = dc.SelectObject(&m_fontTitle);
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(m_clrHdrText);
		dc.DrawText(_T("消化道手术机器人控制系统"), &headerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		dc.SelectObject(old);

		// 3. Draw System Control Card (Two-Tone + Shadow)
		if (!m_rectCardSysCtrl.IsRectEmpty())
			DrawSysControlCard(dc, m_rectCardSysCtrl);
		
		// 4. Draw Main Cards (White + Shadow)
		if (!m_rectCardCamera.IsRectEmpty())
			DrawCardWithTitle(dc, m_rectCardCamera, kRadius, _T("Camera View"), m_clrCardBg, m_clrCardBorder, m_clrMainText);
			
		if (!m_rectCardMaster.IsRectEmpty())
			DrawCardWithTitle(dc, m_rectCardMaster, kRadius, _T("Master Param"), m_clrCardBg, m_clrCardBorder, m_clrMainText);
			
		if (!m_rectCardRobot.IsRectEmpty())
			DrawCardWithTitle(dc, m_rectCardRobot, kRadius, _T("Robot Param"), m_clrCardBg, m_clrCardBorder, m_clrMainText);
			
		if (!m_rectCardChart.IsRectEmpty())
			DrawCardWithTitle(dc, m_rectCardChart, kRadius, _T("Force Feedback (N)"), m_clrCardBg, m_clrCardBorder, m_clrMainText);
	}
}

BOOL CSRDlg::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}

HBRUSH CSRDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	int id = pWnd->GetDlgCtrlID();
	
	// Status Edits
	if (id == IDC_EDIT_MOTOR_STATUS || id == IDC_EDIT_HAPTIC_STATUS)
	{
		CString strText;
		pWnd->GetWindowText(strText);
		pDC->SelectObject(&m_fontLabel);
		if (strText.Find(_T("Connected")) >= 0 && strText.Find(_T("Dis")) == -1 && strText.Find(_T("Fail")) == -1)
			pDC->SetTextColor(m_clrOkGreen);
		else
			pDC->SetTextColor(m_clrDangerRed);
		pDC->SetBkColor(RGB(255, 255, 255));
		return (HBRUSH)GetStockObject(WHITE_BRUSH);
	}

	// Other ReadOnly Edits
	if (id == IDC_EDIT_MASTER_POS || id == IDC_EDIT_MASTER_ENC || id == IDC_EDIT_MASTER_FORCE ||
		id == IDC_EDIT_POSE || id == IDC_EDIT_BEND || id == IDC_EDIT_GRIP_ANGLE || id == IDC_EDIT_GRIP_MOTOR)
	{
		pDC->SetTextColor(m_clrMainText);
		pDC->SetBkColor(RGB(255, 255, 255));
		return (HBRUSH)GetStockObject(WHITE_BRUSH);
	}

	if (nCtlColor == CTLCOLOR_STATIC)
	{
		if (id == IDC_STATIC_CAMERA) return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
		
		pDC->SetBkMode(TRANSPARENT);
		
		CRect r; 
		pWnd->GetWindowRect(&r); 
		ScreenToClient(&r);
		
		// Check intersection with SysCtrl
		CRect intersect;
		if (intersect.IntersectRect(&r, &m_rectCardSysCtrl)) {
			pDC->SetTextColor(m_clrSysText);
		} else {
			pDC->SetTextColor(m_clrSubText); // Muted color for labels
		}
		
		// Use Main font by default for statics
		pDC->SelectObject(&m_fontMain);
		
		return (HBRUSH)GetStockObject(NULL_BRUSH);
	}

	if (nCtlColor == CTLCOLOR_EDIT || nCtlColor == CTLCOLOR_MSGBOX)
	{
		pDC->SetTextColor(m_clrMainText);
		pDC->SetBkColor(RGB(255, 255, 255));
		return (HBRUSH)GetStockObject(WHITE_BRUSH);
	}

	return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}

HCURSOR CSRDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CSRDlg::OnBnClickedOk()
{
	CDialogEx::OnOK();
}

// ==========================================================================
// Logic & Handlers (Unchanged)
// ==========================================================================

void CSRDlg::OnClickedButtonStartM()
{
	state = 1;

	if (!m_pMotorManager->Connect())
	{
		CString strError;
		strError.Format(_T("Can't open device! Error: 0x%08X"), m_pMotorManager->GetLastErrorCode());
		AfxMessageBox(strError, MB_ICONINFORMATION);
		return;
	}

	if (!m_pMotorManager->EnableMotors())
	{
		CString strError;
		strError.Format(_T("Failed to enable motors! Error: 0x%08X"), m_pMotorManager->GetLastErrorCode());
		AfxMessageBox(strError, MB_ICONWARNING);
	}

	QueryPerformanceFrequency(&iFreq);
	QueryPerformanceCounter(&iBegTime);

	maxon_state = TRUE;
	m_editMotorStatus.SetWindowText(_T("Connected"));
	m_editMotorStatus.Invalidate();
}

void CSRDlg::OnClickedButtonShutM()
{
	if (m_pMotorManager)
	{
		m_pMotorManager->DisableMotors();
	}
	maxon_state = FALSE;
	m_editMotorStatus.SetWindowText(_T("Disconnected"));
	m_editMotorStatus.Invalidate();
}

void CSRDlg::OnClickedButtonSpeedM()
{
	if (!maxon_state) {
		AfxMessageBox(_T("Please Start Motor (Start M)"));
		return;
	}

	m_pMotorManager->ConfigureMotorProfile(1, 16000, 10000, 10000, 2000);

	for (WORD nodeId = 2; nodeId <= 5; ++nodeId)
	{
		m_pMotorManager->ConfigureMotorProfile(nodeId, 16000, 10000, 10000, 2000);
	}
}

void CSRDlg::OnClickedButtonZeroM()
{
	if (!maxon_state) return;

	maxon5_position = 0; maxon4_position = 0; maxon3_position = 0; maxon2_position = 0; maxon1_position = 0;

	for (WORD nodeId = 1; nodeId <= 5; ++nodeId)
	{
		m_pMotorManager->MoveToPosition(nodeId, 0, true, true);
	}
}

void CSRDlg::OnClickedButtonStartH()
{
	if (dhdOpen() < 0) {
		m_editHapticStatus.SetWindowText(_T("Connect Failed"));
		m_editHapticStatus.Invalidate();
		AfxMessageBox(_T("Cannot open Haptic Device - Check connection or driver"), MB_ICONERROR);
		return;
	}

	m_editHapticStatus.SetWindowText(_T("Connected"));
	m_editHapticStatus.Invalidate();

	dhdEnableForce(DHD_ON);
	dhdEnableExpertMode();

	done = 0;
	SetTimer(1, 10, NULL);
}

void CSRDlg::OnClickedButtonZeroH()
{
	if (dhdGetPosition(&px, &py, &pz) < 0) {
		return;
	}

	dhdGetEnc(enc);
	offset_enc0 = enc[6];

	ref_px = px;
	ref_py = py;
	ref_pz = pz;

	Sleep(100);

	Ning = true;
	motor_flag = TRUE;
}

void CSRDlg::OnClickedButtonShutH()
{
	KillTimer(1);
	dhdClose();

	Ning = false;
	motor_flag = FALSE;
	done = 1;

	m_editHapticStatus.SetWindowText(_T("Disconnected"));
	m_editHapticStatus.Invalidate();
}

void CSRDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == 2)
	{
		if (m_camera.isOpened())
		{
			m_camera >> m_cameraFrame;
			if (!m_cameraFrame.empty())
			{
				DrawMatToPic(m_cameraFrame, m_picCamera);
			}
		}
	}

	if (nIDEvent == 1 && !done)
	{
		t1 = dhdGetTime();
		t0 = t1;

		if (dhdGetPosition(&px, &py, &pz) < DHD_NO_ERROR) {
			return;
		}

		if (dhdGetForce(&fx, &fy, &fz) < DHD_NO_ERROR) {
		}

		if (m_ChartCtrl.GetSafeHwnd())
		{
			m_pLineSeries[0]->AddPoint(t1, fx);
			m_pLineSeries[1]->AddPoint(t1, fy);
			m_pLineSeries[2]->AddPoint(t1, fz);
		}

		if (dhdGetEnc(enc) < 0) {
		}

		CString strUI;
		strUI.Format(_T("X: %.3f  Y: %.3f  Z: %.3f"), px, py, pz);
		m_editMasterPos.SetWindowText(strUI);

		strUI.Format(_T("%d, %d, %d, %d, %d, %d"), enc[0], enc[1], enc[2], enc[3], enc[4], enc[5]);
		m_editMasterEnc.SetWindowText(strUI);

		strUI.Format(_T("Fx: %.2f  Fy: %.2f  Fz: %.2f"), fx, fy, fz);
		m_editMasterForce.SetWindowText(strUI);

		if (Ning)
		{
			rel_px = px - ref_px;
			rel_py = py - ref_py;
			rel_pz = pz - ref_pz;

			int encnew = enc[6] - offset_enc0;
			if (encnew < 0) encnew = 0;

			// Gripper Angle
			double joint_jiaqian = -0.0000000000000977 * pow(10 * encnew, 3)
				- 0.000000006089 * pow(10 * encnew, 2)
				- 0.000895298679 * (10 * encnew)
				+ 69.834526975;

			// Gripper Motor QC
			double qc_val = 1.9 * 13 * encnew;
			int qc = (int)qc_val;

			// Pose Calculation
			pos_x = 125 + 300 * rel_px;
			pos_y = 300 * rel_py;
			pos_z = 300 * rel_pz;

			Eigen::MatrixXf T0_end = Eigen::MatrixXf::Zero(4, 4);
			Eigen::MatrixXf T1_end = Eigen::MatrixXf::Zero(4, 4);
			Eigen::MatrixXf T2_end = Eigen::MatrixXf::Zero(4, 4);
			Eigen::MatrixXf T012_end = Eigen::MatrixXf::Zero(4, 4);
			Eigen::MatrixXf Ts1_end = Eigen::MatrixXf::Zero(4, 4);

			double safe_pos_x = (abs(pos_x) < 0.0001) ? 0.0001 : pos_x;
			double safe_pos_y = (abs(pos_y) < 0.0001) ? 0.0001 : pos_y;

			bend_angle_end = abs(2 * atan(sqrt(pow(pos_z, 2) + pow(pos_y, 2)) / safe_pos_x));
			bend_direction_end = atan(pos_z / safe_pos_y);

			double belta1_end = atan(cos(bend_direction_end) * tan(bend_angle_end / 4));
			double belta2_end = asin(-sin(bend_direction_end) * sin(bend_angle_end / 4));

			if (abs(belta1_end) < 1e-6) belta1_end = 1e-6;
			if (abs(belta2_end) < 1e-6) belta2_end = 1e-6;

			T0_end << cos(belta1_end), sin(belta1_end), 0, (kJointSpacing / belta1_end)* tan(belta1_end / 2),
				-sin(belta1_end), cos(belta1_end), 0, 0,
				0, 0, 1, 0,
				0, 0, 0, 1;

			T1_end << cos(belta2_end), sin(belta2_end), 0, ((kJointSpacing / belta1_end) * tan(belta1_end / 2) + kJointThickness + (kJointSpacing / belta2_end) * tan(belta2_end / 2)),
				0, 0, 1, 0,
				sin(belta2_end), -cos(belta2_end), 0, 0,
				0, 0, 0, 1;

			T2_end << 1, 0, 0, (kJointThickness + (kJointSpacing / belta2_end) * tan(belta2_end / 2)),
				0, 0, -1, 0,
				0, 1, 0, 0,
				0, 0, 0, 1;

			T012_end = T0_end * T1_end * T2_end;
			Ts1_end = T012_end * T012_end * T012_end * T012_end * T012_end * T012_end;

			pos_x_real = Ts1_end(0, 3);
			pos_y_real = Ts1_end(1, 3);
			pos_z_real = Ts1_end(2, 3);

			delta_L1 = rel_py * 330000;
			delta_L2 = rel_pz * 280000;

			// Update UI
			strUI.Format(_T("X: %.2f Y: %.2f Z: %.2f"), pos_x_real, pos_y_real, pos_z_real);
			m_editPose.SetWindowText(strUI);

			strUI.Format(_T("M4: %ld, M5: %ld"), (long)(3 * delta_L1), (long)(-(3 * delta_L2)));
			m_editBend.SetWindowText(strUI);

			strUI.Format(_T("Angle: %.2f"), joint_jiaqian);
			m_editGripAngle.SetWindowText(strUI);

			strUI.Format(_T("M3: %d"), -qc);
			m_editGripMotor.SetWindowText(strUI);


			if (motor_flag && maxon_state)
			{
				m_pMotorManager->MoveToPosition(3, -qc, true, true);

				long targetPos4 = (long)(3 * delta_L1);
				m_pMotorManager->MoveToPosition(4, targetPos4, true, true);

				long targetPos5 = (long)(-(3 * delta_L2));
				m_pMotorManager->MoveToPosition(5, targetPos5, true, true);
			}
		}
		else
		{
			// Optional: Clear or set waiting text when not active
			m_editPose.SetWindowText(_T("Waiting..."));
			m_editBend.SetWindowText(_T(""));
			m_editGripAngle.SetWindowText(_T(""));
			m_editGripMotor.SetWindowText(_T(""));
		}
	}

	CDialogEx::OnTimer(nIDEvent);
}

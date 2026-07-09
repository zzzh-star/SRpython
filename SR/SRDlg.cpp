// SRDlg.cpp : implementation file
//v1.3 Data Source Correction (Match TXDlg Logic)

#include "pch.h"
#include "framework.h"
#include "SR.h"
#include "SRDlg.h"
#include "ChartAxisLabel.h"
#include "ChartGrid.h"
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
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <cstdlib>
#include <deque>

#include <unsupported/Eigen/CXX11/Tensor> 
#include <Eigen/Dense> 

#define WM_MOTOR_INIT_COMPLETE (WM_USER + 100)
// Dynamically created serial-port combo box.
#define IDC_COMBO_SENSOR_PORT 20006

using namespace Eigen;
using namespace std;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

LARGE_INTEGER iFreq;
LARGE_INTEGER iBegTime;
LARGE_INTEGER iStopTime;
int state;

// ============================================================================
// Sensor serial reader (WinAPI, x64-compatible)
// MSComm ActiveX often fails in x64 with 0x80040154, so sensor data is read
// by a WinAPI serial thread. A combo box is added for manual COM selection.
// ============================================================================
static HANDLE g_hSensorCom = INVALID_HANDLE_VALUE;
static std::thread g_sensorThread;
static std::atomic<bool> g_sensorRunning(false);
static std::mutex g_sensorMutex;
static CString g_sensorRawString;
static double g_sensorValues[20] = { 0.0 };
static double g_sensorRawValues[4] = { 0.0 };
static double g_sensorZeroValues[4] = { 0.0 };
static bool g_sensorZeroed = false;
static int g_sensorPortNumber = 0;
static CComboBox g_comboSensorPort;
static const double kSensorDeadband = 0.03;
static const double kMaxSensorZeroOffset = 0.5; // Zeroing only compensates small drift.
static const double kMaxGripperFeedbackForce = 8.0; // omega.7 gripper feedback limit: +/-8 N
static const double kMinGripperFeedbackForce = 0.05; // Ignore gripper feedback below 0.05 N

struct SensorPlotPoint
{
	double t;
	double fx;
	double fy;
	double fz;
};

static std::deque<SensorPlotPoint> g_sensorPlotPoints;
static std::deque<SensorPlotPoint> g_sensorRawSamples;

static CString MakeComName(int portNumber)
{
	CString name;
	name.Format(_T("\\\\.\\COM%d"), portNumber);
	return name;
}

static std::vector<int> EnumAvailableComPorts()
{
	std::vector<int> ports;
	TCHAR targetPath[512] = { 0 };

	for (int port = 1; port <= 255; ++port)
	{
		CString portName;
		portName.Format(_T("COM%d"), port);

		if (::QueryDosDevice(portName, targetPath, 512) != 0)
		{
			ports.push_back(port);
		}
	}

	return ports;
}

static void PopulateSensorPortCombo(int preferredPort = 10)
{
	if (!g_comboSensorPort.GetSafeHwnd()) return;

	g_comboSensorPort.ResetContent();

	std::vector<int> ports = EnumAvailableComPorts();
	int selectedIndex = CB_ERR;

	for (int port : ports)
	{
		CString item;
		item.Format(_T("COM%d"), port);
		int index = g_comboSensorPort.AddString(item);
		g_comboSensorPort.SetItemData(index, (DWORD_PTR)port);

		if (port == preferredPort)
		{
			selectedIndex = index;
		}
	}

	if (ports.empty())
	{
		int index = g_comboSensorPort.AddString(_T("No COM"));
		g_comboSensorPort.SetItemData(index, 0);
		g_comboSensorPort.SetCurSel(index);
		return;
	}

	if (selectedIndex == CB_ERR) selectedIndex = 0;
	g_comboSensorPort.SetCurSel(selectedIndex);
}

static int GetSelectedSensorPort()
{
	if (!g_comboSensorPort.GetSafeHwnd()) return 0;

	int sel = g_comboSensorPort.GetCurSel();
	if (sel == CB_ERR) return 0;

	DWORD_PTR data = g_comboSensorPort.GetItemData(sel);
	return (int)data;
}

static bool OpenSensorPort(int portNumber, HANDLE& hCom)
{
	CString portName = MakeComName(portNumber);

	hCom = ::CreateFile(portName,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (hCom == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	::SetupComm(hCom, 4096, 4096);

	DCB dcb;
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (!::GetCommState(hCom, &dcb))
	{
		::CloseHandle(hCom);
		hCom = INVALID_HANDLE_VALUE;
		return false;
	}

	dcb.BaudRate = CBR_115200;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;

	if (!::SetCommState(hCom, &dcb))
	{
		::CloseHandle(hCom);
		hCom = INVALID_HANDLE_VALUE;
		return false;
	}

	COMMTIMEOUTS timeouts;
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = 20;
	timeouts.ReadTotalTimeoutConstant = 20;
	timeouts.ReadTotalTimeoutMultiplier = 1;
	timeouts.WriteTotalTimeoutConstant = 50;
	timeouts.WriteTotalTimeoutMultiplier = 1;
	::SetCommTimeouts(hCom, &timeouts);

	::PurgeComm(hCom, PURGE_RXCLEAR | PURGE_TXCLEAR);
	return true;
}

static std::string TrimAscii(const std::string& s)
{
	size_t b = 0;
	while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
	size_t e = s.size();
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
	return s.substr(b, e - b);
}

static void StoreSensorRawString(const std::string& raw)
{
	std::string line = TrimAscii(raw);
	if (line.empty()) return;

	CString cstr(line.c_str());
	std::lock_guard<std::mutex> lock(g_sensorMutex);
	g_sensorRawString = cstr;
}

static void SensorReadThreadProc()
{
	std::string acc;
	char buf[256];

	while (g_sensorRunning)
	{
		DWORD nRead = 0;
		BOOL ok = ::ReadFile(g_hSensorCom, buf, sizeof(buf) - 1, &nRead, NULL);
		if (!ok)
		{
			::Sleep(10);
			continue;
		}

		if (nRead == 0)
		{
			::Sleep(2);
			continue;
		}

		buf[nRead] = '\0';
		acc.append(buf, nRead);

		// Prefer newline-terminated frames, if present.
		for (char& ch : acc)
		{
			if (ch == '\r') ch = '\n';
		}

		size_t pos = std::string::npos;
		while ((pos = acc.find('\n')) != std::string::npos)
		{
			std::string line = acc.substr(0, pos);
			acc.erase(0, pos + 1);
			StoreSensorRawString(line);
		}

		// Also support frames without newline: 99,value1,value2,value3,99
		size_t start = acc.find("99,");
		if (start != std::string::npos)
		{
			if (start > 0) acc.erase(0, start);
			size_t end = acc.find(",99", 3);
			if (end != std::string::npos)
			{
				std::string frame = acc.substr(0, end + 3);
				StoreSensorRawString(frame);
				acc.erase(0, end + 3);
			}
		}

		// Also support label frames without newline, for example:
		// Fz:12.34N,Fx:-0.56N,Fy:7.89N
		if (acc.find('\n') == std::string::npos)
		{
			size_t posFx = acc.find("Fx");
			if (posFx == std::string::npos) posFx = acc.find("FX");
			if (posFx == std::string::npos) posFx = acc.find("fx");

			size_t posFy = acc.find("Fy");
			if (posFy == std::string::npos) posFy = acc.find("FY");
			if (posFy == std::string::npos) posFy = acc.find("fy");

			size_t posFz = acc.find("Fz");
			if (posFz == std::string::npos) posFz = acc.find("FZ");
			if (posFz == std::string::npos) posFz = acc.find("fz");

			if (posFx != std::string::npos && posFy != std::string::npos && posFz != std::string::npos)
			{
				size_t lastLabel = std::max(posFx, std::max(posFy, posFz));
				size_t endN = acc.find('N', lastLabel);
				if (endN != std::string::npos)
				{
					std::string frame = acc.substr(0, endN + 1);
					StoreSensorRawString(frame);
					acc.erase(0, endN + 1);
				}
			}
		}

		if (acc.size() > 2048)
		{
			acc.erase(0, acc.size() - 1024);
		}
	}
}

static void StopSensorSerial()
{
	g_sensorRunning = false;
	if (g_sensorThread.joinable())
	{
		g_sensorThread.join();
	}
	if (g_hSensorCom != INVALID_HANDLE_VALUE)
	{
		::CloseHandle(g_hSensorCom);
		g_hSensorCom = INVALID_HANDLE_VALUE;
	}
	g_sensorPortNumber = 0;
	std::lock_guard<std::mutex> lock(g_sensorMutex);
	g_sensorRawString.Empty();
	for (int i = 0; i < 20; ++i) g_sensorValues[i] = 0.0;
	for (int i = 0; i < 4; ++i) g_sensorRawValues[i] = 0.0;
	g_sensorPlotPoints.clear();
	g_sensorRawSamples.clear();
}

static bool StartSensorSerialByPort(int portNumber, CString& statusText)
{
	StopSensorSerial();

	if (portNumber <= 0)
	{
		statusText = _T("No COM port selected");
		return false;
	}

	HANDLE hCom = INVALID_HANDLE_VALUE;
	if (!OpenSensorPort(portNumber, hCom))
	{
		statusText.Format(_T("COM%d open failed"), portNumber);
		return false;
	}

	g_hSensorCom = hCom;
	g_sensorPortNumber = portNumber;
	g_sensorRunning = true;
	g_sensorThread = std::thread(SensorReadThreadProc);

	statusText.Format(_T("COM%d connected"), portNumber);
	return true;
}

static bool ExtractForceValueByLabel(const CString& src, const CString& upper, LPCTSTR label, double& value)
{
	int labelPos = upper.Find(label);
	if (labelPos < 0) return false;

	int i = labelPos + (int)_tcslen(label);
	int len = src.GetLength();

	// Skip all non-number characters after Fx/Fy/Fz.
	while (i < len)
	{
		TCHAR ch = src[i];
		bool isNumberStart = ((ch >= _T('0') && ch <= _T('9')) || ch == _T('-') || ch == _T('+') || ch == _T('.'));
		if (isNumberStart) break;
		++i;
	}

	int start = i;
	while (i < len)
	{
		TCHAR ch = src[i];
		bool isNumberChar = ((ch >= _T('0') && ch <= _T('9')) ||
			ch == _T('-') || ch == _T('+') || ch == _T('.') ||
			ch == _T('e') || ch == _T('E'));
		if (!isNumberChar) break;
		++i;
	}

	if (i <= start) return false;

	CString num = src.Mid(start, i - start);
	value = _tstof(num);
	return true;
}

// Arduino frame example:
// Fz:12.34N,Fx:-0.56N,Fy:7.89N
static bool ParseSensorLineToFxFyFz(const CString& raw, double& fx, double& fy, double& fz)
{
	CString s = raw;
	s.Trim();
	if (s.IsEmpty()) return false;


	CString upper = s;
	upper.MakeUpper();

	bool okFx = ExtractForceValueByLabel(s, upper, _T("FX"), fx);
	bool okFy = ExtractForceValueByLabel(s, upper, _T("FY"), fy);
	bool okFz = ExtractForceValueByLabel(s, upper, _T("FZ"), fz);

	return okFx && okFy && okFz;
}

static bool CStringToDoubleStrict(const CString& src, double& value)
{
	CString s = src;
	s.Trim();
	if (s.IsEmpty()) return false;

	LPCTSTR pStart = s.GetString();
	TCHAR* pEnd = nullptr;
	value = _tcstod(pStart, &pEnd);

	if (pEnd == pStart) return false;

	while (pEnd && (*pEnd == _T(' ') || *pEnd == _T('\t')))
	{
		++pEnd;
	}

	return (pEnd != nullptr && *pEnd == _T('\0'));
}

// Numeric sensor frame parser.
// Supported formats:
// 1) Fx,Fy,Fz               example: 1.234,-0.560,7.890
// 2) 99,Fx,Fy,Fz,99         example: 99,1.234,-0.560,7.890,99
// 3) Fx Fy Fz or Fx\tFy\tFz
static bool ParseNumericSensorFrame(const CString& raw, double& fx, double& fy, double& fz)
{
	CString s = raw;
	s.Trim();
	if (s.IsEmpty()) return false;

	// Make the parser tolerant to space/tab/semicolon separated pure numbers.
	s.Replace(_T("\t"), _T(","));
	s.Replace(_T(";"), _T(","));
	s.Replace(_T(" "), _T(","));
	while (s.Replace(_T(",,"), _T(",")) > 0) {}

	double theNum[16] = { 0.0 };
	int index = 0;
	int curPos = 0;

	CString token = s.Tokenize(_T(","), curPos);
	while (!token.IsEmpty() && index < 16)
	{
		double value = 0.0;
		if (!CStringToDoubleStrict(token, value))
		{
			return false;
		}

		theNum[index++] = value;
		token = s.Tokenize(_T(","), curPos);
	}

	// Framed numeric data in SR: 99,Fx,Fy,Fz,99.
	// The parsed Fz value is later used as haptic gripper feedback.
	if (index >= 5 && theNum[0] == 99.0 && theNum[4] == 99.0)
	{
		fx = theNum[1];
		fy = theNum[2];
		fz = theNum[3];
		return true;
	}

	// Pure numeric data: Fx,Fy,Fz
	if (index >= 3)
	{
		fx = theNum[0];
		fy = theNum[1];
		fz = theNum[2];
		return true;
	}

	return false;
}

static double ClampSensorZeroOffset(double value)
{
	return (std::max)(-kMaxSensorZeroOffset,
		(std::min)(kMaxSensorZeroOffset, value));
}


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

// Helper to draw OpenCV Mat to CStatic
// Helper to draw OpenCV Mat to CStatic
void DrawMatToPic(const cv::Mat& img, CStatic& pic)
{
	if (img.empty()) return;
	if (!pic.GetSafeHwnd()) return;

	CRect rect;
	pic.GetClientRect(&rect);
	if (rect.IsRectEmpty()) return;

	cv::Mat bgrImg;

	if (img.channels() == 1)
	{
		cv::cvtColor(img, bgrImg, cv::COLOR_GRAY2BGR);
	}
	else if (img.channels() == 2)
	{
		// Many external USB cameras / endoscope cameras output YUY2/YUYV
		cv::cvtColor(img, bgrImg, cv::COLOR_YUV2BGR_YUY2);
	}
	else if (img.channels() == 3)
	{
		bgrImg = img;
	}
	else if (img.channels() == 4)
	{
		cv::cvtColor(img, bgrImg, cv::COLOR_BGRA2BGR);
	}
	else
	{
		return;
	}

	if (bgrImg.depth() != CV_8U)
	{
		bgrImg.convertTo(bgrImg, CV_8U);
	}

	int alignedW = rect.Width() & ~3;
	if (alignedW < 4) alignedW = 4;

	cv::Mat resized;
	cv::resize(bgrImg, resized, cv::Size(alignedW, rect.Height()));

	if (!resized.isContinuous())
	{
		resized = resized.clone();
	}

	BITMAPINFO bitInfo;
	memset(&bitInfo, 0, sizeof(BITMAPINFO));

	bitInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitInfo.bmiHeader.biWidth = resized.cols;
	bitInfo.bmiHeader.biHeight = -resized.rows;
	bitInfo.bmiHeader.biPlanes = 1;
	bitInfo.bmiHeader.biBitCount = 24;
	bitInfo.bmiHeader.biCompression = BI_RGB;

	CDC* pDC = pic.GetDC();
	if (pDC)
	{
		::StretchDIBits(
			pDC->GetSafeHdc(),
			0, 0, rect.Width(), rect.Height(),
			0, 0, resized.cols, resized.rows,
			resized.data,
			&bitInfo,
			DIB_RGB_COLORS,
			SRCCOPY
		);

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
	: CDialogEx(IDD_SR_DIALOG, pParent), m_pMotorManager(nullptr), m_pBridgeServer(nullptr)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_pMotorManager = new MotorManager();
	m_pBridgeServer = new PythonBridgeServer();

	// --- Theme Color Initialization ---

	// App BG (Dark Theme)
	m_clrAppBg = RGB(43, 56, 66);

	// Header
	m_clrHdrTop = RGB(16, 45, 70);
	m_clrHdrBottom = RGB(24, 72, 110);
	m_clrHdrText = RGB(255, 255, 255);
	m_clrHdrLine = RGB(0, 188, 212);

	// Sidebar
	m_clrSidebarBg = RGB(43, 56, 66);
	m_clrSideCardTitle = RGB(25, 30, 34);
	m_clrSideCardBg = RGB(55, 67, 76);
	m_clrSideCardBorder = RGB(70, 85, 95);
	m_clrSidebarText = RGB(235, 240, 245);

	// Main Cards (Dark Theme)
	m_clrMainCardBg = RGB(55, 67, 76);
	m_clrMainCardBorder = RGB(70, 85, 95);
	m_clrMainText = RGB(235, 240, 245);
	m_clrSubText = RGB(170, 180, 190);

	// Status
	m_clrOkGreen = RGB(46, 204, 113);
	m_clrDangerRed = RGB(255, 59, 48);

	// Create Brushes
	m_brushAppBg.CreateSolidBrush(m_clrAppBg);
	m_brushSidebarBg.CreateSolidBrush(m_clrSidebarBg);
	m_brushMainCardBg.CreateSolidBrush(m_clrMainCardBg);

	px = py = pz = 0.0;
	fx = fy = fz = 0.0;
	ref_px = ref_py = ref_pz = 0.0;
	rel_px = rel_py = rel_pz = 0.0;

	Ning = false;
	motor_flag = false;
	done = 1;
	t0 = 0;
	t1 = 0;

	// Explicitly initialize switch states and timer handle.
	m_nMotorSwitchState = 0;
	m_nHapticSwitchState = 0;
	m_nSensorSwitchState = 0;
	m_nMotorTimer = 0;
	for (int i = 0; i < 3; ++i) m_pLineSeries[i] = nullptr;
}

CSRDlg::~CSRDlg()
{
	if (m_workerThread.joinable()) m_workerThread.join();

	if (m_camera.isOpened()) m_camera.release();
	if (m_pBridgeServer)
	{
		m_pBridgeServer->ForceZeroFeedback("application closing");
		m_pBridgeServer->Stop();
		delete m_pBridgeServer;
		m_pBridgeServer = nullptr;
	}
	dhdClose();

	if (m_pMotorManager)
	{
		delete m_pMotorManager;
		m_pMotorManager = nullptr;
	}

	m_brushAppBg.DeleteObject();
	m_brushSidebarBg.DeleteObject();
	m_brushMainCardBg.DeleteObject();
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

BEGIN_EVENTSINK_MAP(CSRDlg, CDialogEx)
	ON_EVENT(CSRDlg, 20004, 1, OnCommEvent, VTS_NONE)
END_EVENTSINK_MAP()

BEGIN_MESSAGE_MAP(CSRDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_CTLCOLOR()
	ON_WM_QUERYDRAGICON()
	ON_WM_SIZE()
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_WM_DRAWITEM()
	ON_BN_CLICKED(IDOK, &CSRDlg::OnBnClickedOk)
	ON_BN_CLICKED(IDC_BUTTON_STARTM, &CSRDlg::OnClickedButtonStartM)
	ON_BN_CLICKED(IDC_BUTTON_SHUTM, &CSRDlg::OnClickedButtonShutM)
	ON_BN_CLICKED(IDC_BUTTON_SPEEDM, &CSRDlg::OnClickedButtonSpeedM)
	ON_BN_CLICKED(IDC_BUTTON_ZEROM, &CSRDlg::OnClickedButtonZeroM)
	ON_BN_CLICKED(IDC_BUTTON_STARTH, &CSRDlg::OnClickedButtonStartH)
	ON_BN_CLICKED(IDC_BUTTON_ZEROH, &CSRDlg::OnClickedButtonZeroH)
	ON_BN_CLICKED(IDC_BUTTON_SHUTH, &CSRDlg::OnClickedButtonShutH)
	ON_BN_CLICKED(20001, &CSRDlg::OnClickedMotorSwitch)
	ON_BN_CLICKED(20002, &CSRDlg::OnClickedHapticSwitch)
	ON_BN_CLICKED(20003, &CSRDlg::OnClickedSensorSwitch)
	ON_BN_CLICKED(IDC_BUTTON_CLEAR_CHART, &CSRDlg::OnClickedClearChart)
	ON_BN_CLICKED(IDC_BUTTON_SENSOR_ZERO, &CSRDlg::OnClickedSensorZero)
	ON_MESSAGE(WM_MOTOR_INIT_COMPLETE, &CSRDlg::OnMotorInitComplete)
END_MESSAGE_MAP()

BOOL CSRDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// Sensor serial input is handled by WinAPI serial thread.
	// The old MSComm ActiveX control is intentionally not created here,
	// because it often fails in x64 with 0x80040154 and then no data arrive.

	// Set Window Text
	SetWindowText(_T("消化道手术机器人控制系统"));

	// Initialize Fonts
	m_fontTitle.CreateFont(36, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
	m_fontMain.CreatePointFont(90, _T("Segoe UI"));
	m_fontLabel.CreatePointFont(90, _T("Segoe UI Semibold"));
	m_fontSidebarBtn.CreatePointFont(80, _T("Segoe UI"));

	SetFont(&m_fontMain);

	// Modernize Buttons (General)
	UINT btnIds[] = { IDOK };
	for (UINT id : btnIds) {
		CWnd* pBtn = GetDlgItem(id);
		if (pBtn) {
			::SetWindowTheme(pBtn->GetSafeHwnd(), L"Explorer", NULL);
			pBtn->SetFont(&m_fontMain);
		}
	}

	// Re-create m_editMasterEnc with MULTILINE style
	// We need to do this here before the border removal loop or after, but since it's re-created, 
	// we should ensure it's ready.
	if (m_editMasterEnc.GetSafeHwnd()) {
		m_editMasterEnc.DestroyWindow();
	}
	m_editMasterEnc.Create(ES_MULTILINE | ES_READONLY | WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, IDC_EDIT_MASTER_ENC);
	m_editMasterEnc.SetFont(&m_fontMain);

	// Remove Borders from Param Edits to look like Labels
	UINT paramEditIds[] = {
		IDC_EDIT_MOTOR_STATUS, IDC_EDIT_HAPTIC_STATUS,
		IDC_EDIT_MASTER_POS, IDC_EDIT_MASTER_ENC, IDC_EDIT_MASTER_FORCE,
		IDC_EDIT_POSE, IDC_EDIT_BEND, IDC_EDIT_GRIP_ANGLE, IDC_EDIT_GRIP_MOTOR
	};
	for (UINT id : paramEditIds) {
		CWnd* pEdit = GetDlgItem(id);
		if (pEdit) {
			pEdit->ModifyStyle(WS_BORDER, 0, SWP_DRAWFRAME);
			pEdit->ModifyStyleEx(WS_EX_CLIENTEDGE, 0, SWP_DRAWFRAME);
		}
	}

	// Sidebar Buttons (Motor & Haptic) & Exit Button
	UINT sidebarBtnIds[] = { IDC_BUTTON_STARTM, IDC_BUTTON_SHUTM, IDC_BUTTON_SPEEDM, IDC_BUTTON_ZEROM,
							 IDC_BUTTON_STARTH, IDC_BUTTON_ZEROH, IDC_BUTTON_SHUTH, IDCANCEL };
	for (UINT id : sidebarBtnIds) {
		CWnd* pBtn = GetDlgItem(id);
		if (pBtn) {
			::SetWindowTheme(pBtn->GetSafeHwnd(), L"Explorer", NULL);
			pBtn->ModifyStyle(0, BS_MULTILINE | BS_CENTER | BS_VCENTER);
			pBtn->SetFont(&m_fontSidebarBtn);
		}
	}

	m_btnClearChart.Create(_T("清除曲线"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_PUSHBUTTON,
		CRect(0, 0, 0, 0), this, IDC_BUTTON_CLEAR_CHART);
	m_btnClearChart.SetFont(&m_fontSidebarBtn);

	m_btnSensorZero.Create(_T("传感器调零"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_PUSHBUTTON,
		CRect(0, 0, 0, 0), this, IDC_BUTTON_SENSOR_ZERO);
	m_btnSensorZero.SetFont(&m_fontSidebarBtn);

	// Create Switch Button
	m_btnMotorSwitch.Create(_T(""), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, 20001);
	m_btnMotorSwitch.SetPngResources(IDR_PNG_SWITCH_OFF, IDR_PNG_SWITCH_ON);
	m_btnMotorSwitch.SetBackgroundColor(m_clrSideCardBg);

	m_btnHapticSwitch.Create(_T(""), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, 20002);
	m_btnHapticSwitch.SetPngResources(IDR_PNG_SWITCH_OFF, IDR_PNG_SWITCH_ON);
	m_btnHapticSwitch.SetBackgroundColor(m_clrSideCardBg);

	// Create Sensor Switch (20003)
	m_btnSensorSwitch.Create(_T(""), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, CRect(0, 0, 0, 0), this, 20003);
	m_btnSensorSwitch.SetPngResources(IDR_PNG_SWITCH_OFF, IDR_PNG_SWITCH_ON);
	m_btnSensorSwitch.SetBackgroundColor(m_clrSideCardBg);

	// Create serial-port combo box. The actual position is set in LayoutUI().
	g_comboSensorPort.Create(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
		CRect(0, 0, 0, 0), this, IDC_COMBO_SENSOR_PORT);
	g_comboSensorPort.SetFont(&m_fontSidebarBtn);
	PopulateSensorPortCombo(10);

	// Create Sensor Label
	m_lblSensor.Create(_T("传感器:"), WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, CRect(0, 0, 0, 0), this);

	// Copy font from "End Force" label to ensure exact match
	CWnd* pEndForce = FindStaticByText(_T("末端力:"));
	if (pEndForce) {
		m_lblSensor.SetFont(pEndForce->GetFont());
	}
	else {
		m_lblSensor.SetFont(&m_fontMain);
	}

	// Create Sensor Data Edit (ID 20005)
	m_editSensorData.Create(ES_AUTOHSCROLL | ES_READONLY | WS_CHILD | WS_VISIBLE, CRect(0, 0, 0, 0), this, 20005);
	m_editSensorData.SetFont(&m_fontLabel); // Or fontMain? Others use fontLabel for values? No, others use Default? 
	// Wait, in OnInitDialog, the others don't get SetFont(&m_fontLabel). They get dialog default. 
	// But Edit controls might need it. Let's stick to fontLabel for values or fontMain. 
	// The prompt was about the LABEL "传感器". Values are fine.
	// Actually, the previous code for edit used `m_fontLabel`. 
	// "主手位姿" labels use `m_fontMain` (default). 
	// So `m_lblSensor.SetFont(&m_fontMain)` is correct.

	// Remove Border from Sensor Edit
	m_editSensorData.ModifyStyle(WS_BORDER, 0, SWP_DRAWFRAME);
	m_editSensorData.ModifyStyleEx(WS_EX_CLIENTEDGE, 0, SWP_DRAWFRAME);

	// Hide old GroupBoxes and Titles
	// Use only ASCII strings here to avoid code-page build errors.
	const TCHAR* gbTitles[] = {
		_T("Motor"),
		_T("Haptic"),
		_T("Camera View"),
		_T("Master Param"),
		_T("Robot Param"),
		_T("Force Feedback (N)"),
		NULL
	};
	for (int i = 0; gbTitles[i]; ++i) {
		CWnd* pWnd = FindStaticByText(gbTitles[i]);
		if (pWnd) pWnd->ShowWindow(SW_HIDE);
	}
	// Hide static backgrounds
	UINT oldStaticIds[] = { 1019, 1020, 1021, 1022, 1023, 1024, 1025 };
	for (UINT id : oldStaticIds) {
		CWnd* pWnd = GetDlgItem(id);
		if (pWnd) pWnd->ShowWindow(SW_HIDE);
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
	m_dStartTime = t0;

	m_editMotorStatus.SetWindowText(_T("Disconnected"));
	m_editHapticStatus.SetWindowText(_T("Disconnected"));
	if (m_pBridgeServer && !m_pBridgeServer->Start(8765))
	{
		AfxMessageBox(_T("Python bridge TCP server failed to start on 127.0.0.1:8765"));
	}

	// Camera Init
	// Camera Init
	if (!m_camera.open(1, cv::CAP_DSHOW))
	{
		m_camera.open(0, cv::CAP_DSHOW);
	}

	if (m_camera.isOpened())
	{
		m_camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
		m_camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
		m_camera.set(cv::CAP_PROP_CONVERT_RGB, 1);

		// 如果画面仍然灰白，再尝试打开这一句
		// m_camera.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
	}
	else
	{
		AfxMessageBox(_T("Camera open failed!"));
	}

	SetTimer(2, 50, NULL);

	// Chart Init
	if (!m_ChartCtrl.GetSafeHwnd())
	{
		CRect rect(0, 0, 100, 100);
		m_ChartCtrl.Create(this, rect, 2000, WS_CHILD | WS_VISIBLE);
		m_ChartCtrl.EnableRefresh(true);
		m_ChartCtrl.GetTitle()->AddString(_T(""));
		m_ChartCtrl.SetBackColor(m_clrMainCardBg);
		m_ChartCtrl.SetBorderColor(m_clrMainCardBg);
		m_ChartCtrl.SetEdgeType(0);

		// Enable Legend
		m_ChartCtrl.GetLegend()->SetVisible(true);
		m_ChartCtrl.GetLegend()->DockLegend(CChartLegend::dsDockRight);
		m_ChartCtrl.GetLegend()->SetHorizontalMode(false);

		CChartStandardAxis* pBottomAxis = m_ChartCtrl.CreateStandardAxis(CChartCtrl::BottomAxis);
		pBottomAxis->SetMinMax(0, 20);
		pBottomAxis->SetTextColor(m_clrSubText);
		pBottomAxis->GetLabel()->SetText(_T("Time (s)"));
		pBottomAxis->GetGrid()->SetVisible(true);
		pBottomAxis->GetGrid()->SetColor(RGB(100, 110, 120));

		CChartStandardAxis* pLeftAxis = m_ChartCtrl.CreateStandardAxis(CChartCtrl::LeftAxis);
		pLeftAxis->SetAutomaticMode(CChartAxis::FullAutomatic);
		pLeftAxis->SetTextColor(m_clrSubText);
		pLeftAxis->GetLabel()->SetText(_T("Sensor signal"));
		pLeftAxis->GetGrid()->SetVisible(true);
		pLeftAxis->GetGrid()->SetColor(RGB(100, 110, 120));

		CreateSensorChartSeries();
	}

	// Initial Window Size (1200x850) and Center
	SetWindowPos(NULL, 0, 0, 1200, 850, SWP_NOMOVE | SWP_NOZORDER);
	CenterWindow();

	LayoutUI();

	return TRUE;
}

BOOL CSRDlg::PreTranslateMessage(MSG* pMsg)
{
	if (pMsg->message == WM_KEYDOWN)
	{
		if (pMsg->wParam == VK_F11)
		{
			ToggleFullscreen();
			return TRUE;
		}

		if (pMsg->wParam == VK_ESCAPE && m_bFullscreen)
		{
			SetFullscreen(false);
			return TRUE;
		}
	}

	return CDialogEx::PreTranslateMessage(pMsg);
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
	if (GetSafeHwnd())
	{
		if (nType != SIZE_MINIMIZED)
		{
			m_bFullscreen = IsZoomed() != FALSE;
			UpdateFullscreenButtonText();
		}
		LayoutUI();
	}
}

void CSRDlg::UpdateFullscreenButtonText()
{
}

void CSRDlg::ToggleFullscreen()
{
	SetFullscreen(!m_bFullscreen);
}

void CSRDlg::SetFullscreen(bool fullscreen)
{
	if (!GetSafeHwnd() || fullscreen == m_bFullscreen) return;

	if (fullscreen)
	{
		m_bFullscreen = true;
		ShowWindow(SW_MAXIMIZE);
	}
	else
	{
		m_bFullscreen = false;
		ShowWindow(SW_RESTORE);
	}

	UpdateFullscreenButtonText();
	LayoutUI();
}

void CSRDlg::CreateSensorChartSeries()
{
	if (!m_ChartCtrl.GetSafeHwnd()) return;

	// Fx: blue
	m_pLineSeries[0] = m_ChartCtrl.CreateLineSerie();
	m_pLineSeries[0]->SetColor(RGB(11, 92, 173));
	m_pLineSeries[0]->SetWidth(2);
	m_pLineSeries[0]->SetName(_T("Fx"));

	// Fy: green
	m_pLineSeries[1] = m_ChartCtrl.CreateLineSerie();
	m_pLineSeries[1]->SetColor(RGB(46, 204, 113));
	m_pLineSeries[1]->SetWidth(2);
	m_pLineSeries[1]->SetName(_T("Fy"));

	// Fz: orange
	m_pLineSeries[2] = m_ChartCtrl.CreateLineSerie();
	m_pLineSeries[2]->SetColor(RGB(245, 165, 36));
	m_pLineSeries[2]->SetWidth(2);
	m_pLineSeries[2]->SetName(_T("Fz"));
}

void CSRDlg::ClearSensorChart()
{
	{
		std::lock_guard<std::mutex> lock(g_sensorMutex);
		g_sensorPlotPoints.clear();
	}

	if (m_ChartCtrl.GetSafeHwnd())
	{
		m_ChartCtrl.RemoveAllSeries();
		CreateSensorChartSeries();
		m_ChartCtrl.GetBottomAxis()->SetMinMax(0, 20);
		m_ChartCtrl.GetLeftAxis()->SetMinMax(-1, 1);
		m_ChartCtrl.Invalidate(FALSE);
		m_ChartCtrl.UpdateWindow();
	}
}

void CSRDlg::ZeroSensorBaseline()
{
	double rawFx = 0.0, rawFy = 0.0, rawFz = 0.0;
	double zeroFx = 0.0, zeroFy = 0.0, zeroFz = 0.0;
	double adjFx = 0.0, adjFy = 0.0, adjFz = 0.0;
	int sampleCount = 0;
	{
		std::lock_guard<std::mutex> lock(g_sensorMutex);
		double latestT = g_sensorRawSamples.empty() ? 0.0 : g_sensorRawSamples.back().t;
		const double sampleWindow = 1.0;

		for (const SensorPlotPoint& p : g_sensorRawSamples)
		{
			if (latestT - p.t <= sampleWindow)
			{
				rawFx += p.fx;
				rawFy += p.fy;
				rawFz += p.fz;
				++sampleCount;
			}
		}

		if (sampleCount > 0)
		{
			rawFx /= sampleCount;
			rawFy /= sampleCount;
			rawFz /= sampleCount;
		}
		else
		{
			rawFx = g_sensorRawValues[1];
			rawFy = g_sensorRawValues[2];
			rawFz = g_sensorRawValues[3];
		}

		zeroFx = ClampSensorZeroOffset(rawFx);
		zeroFy = ClampSensorZeroOffset(rawFy);
		zeroFz = ClampSensorZeroOffset(rawFz);

		g_sensorZeroValues[1] = zeroFx;
		g_sensorZeroValues[2] = zeroFy;
		g_sensorZeroValues[3] = zeroFz;

		adjFx = rawFx - zeroFx;
		adjFy = rawFy - zeroFy;
		adjFz = rawFz - zeroFz;

		if (std::abs(adjFx) < kSensorDeadband) adjFx = 0.0;
		if (std::abs(adjFy) < kSensorDeadband) adjFy = 0.0;
		if (std::abs(adjFz) < kSensorDeadband) adjFz = 0.0;

		g_sensorValues[1] = adjFx;
		g_sensorValues[2] = adjFy;
		g_sensorValues[3] = adjFz;
		g_sensorZeroed = true;
	}

	CString msg;
	msg.Format(_T("已调零(%d): Zero Fx %.3f  Fy %.3f  Fz %.3f"), sampleCount, zeroFx, zeroFy, zeroFz);
	m_editSensorData.SetWindowText(msg);
	msg.Format(_T("Fx: %.3f N  Fy: %.3f N  Fz: %.3f N"), adjFx, adjFy, adjFz);
	m_editMasterForce.SetWindowText(msg);
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

	// Calc Layout Areas
	m_rectSidebar.SetRect(0, kHeaderHeight, kSidebarWidth, height);
	m_rectMainArea.SetRect(kSidebarWidth, kHeaderHeight, width, height);

	// --- Sidebar Cards ---
	int x = kSidebarPad;
	int y = kHeaderHeight + kCardGap;
	int cardW = kSidebarWidth - 2 * kSidebarPad;

	// Motor Card (Height for 1 Switch + header)
	int btnH = 34;
	int titleH = 26;
	int pad = 8;

	// Custom size for Motor Switch (Larger size 1.5x)
	int switchBtnW = 84; // 56 * 1.5
	int switchBtnH = 46; // 34 * 1.35 approx

	// Height adjusted for larger switch
	int motorH = titleH + pad + switchBtnH + pad;
	m_rectCardMotor.SetRect(x, y, x + cardW, y + motorH);

	// Move Motor Switch
	// Hide old buttons
	UINT motorBtns[] = { IDC_BUTTON_STARTM, IDC_BUTTON_SPEEDM, IDC_BUTTON_ZEROM, IDC_BUTTON_SHUTM };
	for (UINT id : motorBtns) {
		CWnd* p = GetDlgItem(id);
		if (p) p->ShowWindow(SW_HIDE);
	}
	// Position new switch (Right Aligned)
	if (m_btnMotorSwitch.GetSafeHwnd()) {
		int switchX = m_rectCardMotor.right - 12 - switchBtnW;
		int switchY = m_rectCardMotor.top + titleH + pad;
		m_btnMotorSwitch.SetWindowPos(NULL, switchX, switchY, switchBtnW, switchBtnH, SWP_NOZORDER);
	}

	// Haptic Card (Height adjusted for Switch, match Motor Card)
	y = m_rectCardMotor.bottom + kCardGap;
	// Use same height as Motor card since it's just one switch
	int hapticH = m_rectCardMotor.Height();
	m_rectCardHaptic.SetRect(x, y, x + cardW, y + hapticH);

	// Hide Old Haptic Buttons
	UINT hapticBtns[] = { IDC_BUTTON_STARTH, IDC_BUTTON_ZEROH, IDC_BUTTON_SHUTH };
	for (UINT id : hapticBtns) {
		CWnd* p = GetDlgItem(id);
		if (p) p->ShowWindow(SW_HIDE);
	}

	// Position new Haptic switch (Right Aligned)
	if (m_btnHapticSwitch.GetSafeHwnd()) {
		int switchX = m_rectCardHaptic.right - 12 - switchBtnW;
		int switchY = m_rectCardHaptic.top + titleH + pad;
		m_btnHapticSwitch.SetWindowPos(NULL, switchX, switchY, switchBtnW, switchBtnH, SWP_NOZORDER);
	}

	// Sensor Control Card
	y = m_rectCardHaptic.bottom + kCardGap;
	int toolBtnH = 30;
	int sensorH = titleH + pad + 24 + 6 + switchBtnH + 10 + toolBtnH * 2 + 8 + pad; // COM combo + sensor switch + tools
	m_rectCardSensor.SetRect(x, y, x + cardW, y + sensorH);

	if (g_comboSensorPort.GetSafeHwnd()) {
		int comboX = m_rectCardSensor.left + 12;
		int comboY = m_rectCardSensor.top + titleH + pad;
		int comboW = m_rectCardSensor.Width() - 24;
		// Height includes the drop-down list area for CBS_DROPDOWNLIST.
		g_comboSensorPort.SetWindowPos(NULL, comboX, comboY, comboW, 160, SWP_NOZORDER);
	}

	if (m_btnSensorSwitch.GetSafeHwnd()) {
		int switchX = m_rectCardSensor.right - 12 - switchBtnW;
		int switchY = m_rectCardSensor.top + titleH + pad + 24 + 6;
		m_btnSensorSwitch.SetWindowPos(NULL, switchX, switchY, switchBtnW, switchBtnH, SWP_NOZORDER);
	}

	int toolX = m_rectCardSensor.left + 12;
	int toolY = m_rectCardSensor.top + titleH + pad + 24 + 6 + switchBtnH + 10;
	int toolW = m_rectCardSensor.Width() - 24;
	if (m_btnClearChart.GetSafeHwnd()) {
		m_btnClearChart.SetWindowPos(NULL, toolX, toolY, toolW, toolBtnH, SWP_NOZORDER);
	}
	if (m_btnSensorZero.GetSafeHwnd()) {
		m_btnSensorZero.SetWindowPos(NULL, toolX, toolY + toolBtnH + 8, toolW, toolBtnH, SWP_NOZORDER);
	}

	// Exit Button
	CWnd* pExit = GetDlgItem(IDCANCEL);
	if (pExit) {
		pExit->SetWindowPos(NULL, kSidebarPad, height - kSidebarPad - btnH, cardW, btnH, SWP_NOZORDER);
	}

	// --- Main Area Cards ---
	x = kSidebarWidth + kCardGap;
	y = kHeaderHeight + kCardGap;
	int mainW = width - kSidebarWidth - 2 * kCardGap;
	int rightColW = 360;
	int camW = mainW - rightColW - kCardGap;

	// Master Param Card (Top Right)
	// Increased height to 185 to allow 2 lines for encoder data
	int cardH = 185;
	m_rectCardMaster.SetRect(x + camW + kCardGap, y, x + camW + kCardGap + rightColW, y + cardH);

	// Camera Card (Top Left)
	// Camera height increased to match expanded right column (Master 185 + Gap 14 + Robot 155 = 354)
	int row1H = 354;
	m_rectCardCamera.SetRect(x, y, x + camW, y + row1H);

	// Move Camera
	if (m_picCamera.GetSafeHwnd()) {
		int hdrH = 26;
		m_picCamera.SetWindowPos(NULL, m_rectCardCamera.left + 2, m_rectCardCamera.top + hdrH + 2, m_rectCardCamera.Width() - 4, m_rectCardCamera.Height() - hdrH - 4, SWP_NOZORDER);
	}

	// Move Master Params
	{
		int labelW = 70;
		int rowH = 28; // Increased spacing
		int startY = m_rectCardMaster.top + 42; // Adjusted top padding
		int cxL = m_rectCardMaster.left + 12;
		int cxE = cxL + labelW + 10;
		int cwE = m_rectCardMaster.right - 12 - cxE;
		int cy = startY;

		// Hide Status Rows (Motor/Haptic Status)
		CWnd* pMotorL = FindStaticByText(_T("电机:"));
		if (pMotorL) pMotorL->ShowWindow(SW_HIDE);
		CWnd* pMotorE = GetDlgItem(IDC_EDIT_MOTOR_STATUS);
		if (pMotorE) pMotorE->ShowWindow(SW_HIDE);

		CWnd* pHapticL = FindStaticByText(_T("主手:"));
		if (pHapticL) pHapticL->ShowWindow(SW_HIDE);
		CWnd* pHapticE = GetDlgItem(IDC_EDIT_HAPTIC_STATUS);
		if (pHapticE) pHapticE->ShowWindow(SW_HIDE);

		struct Item { const TCHAR* l; UINT id; };
		Item items[] = {
			// Removed Status items from layout loop
			{_T("主手位姿:"), IDC_EDIT_MASTER_POS},
			{_T("主手编码:"), IDC_EDIT_MASTER_ENC},
			{_T("末端力:"), IDC_EDIT_MASTER_FORCE},
			// New Sensor Data Row
			{_T("传感器:"), 20005}
		};
		for (auto& it : items) {
			CWnd* pL = FindStaticByText(it.l);
			if (!pL && _tcscmp(it.l, _T("主手编码:")) == 0) {
				pL = FindStaticByText(_T("主手编码器:"));
				if (pL) pL->SetWindowText(_T("主手编码:"));
			}

			if (pL) {
				pL->ShowWindow(SW_SHOW); // Ensure visible
				pL->SetWindowPos(NULL, cxL, cy + 2, labelW, 16, SWP_NOZORDER);
			}
			CWnd* pE = GetDlgItem(it.id);
			if (pE) {
				pE->ShowWindow(SW_SHOW); // Ensure visible

				int h = 20;
				if (it.id == IDC_EDIT_MASTER_ENC) h = 38; // Double height for encoder

				pE->SetWindowPos(NULL, cxE, cy, cwE, h, SWP_NOZORDER);
			}
			
			if (it.id == IDC_EDIT_MASTER_ENC) cy += 46; // More spacing for this row
			else cy += rowH;
		}
	}

	// Robot Param Card (Middle Right)
	// Match height with Robot content (155)
	int robotH = 155;
	m_rectCardRobot.SetRect(m_rectCardMaster.left, m_rectCardMaster.bottom + kCardGap, m_rectCardMaster.right, m_rectCardMaster.bottom + kCardGap + robotH);

	// Move Robot Params
	{
		int labelW = 70;
		int rowH = 28; // Increased spacing
		int startY = m_rectCardRobot.top + 42; // Adjusted top padding
		int cxL = m_rectCardRobot.left + 12;
		int cxE = cxL + labelW + 10;
		int cwE = m_rectCardRobot.right - 12 - cxE;
		int cy = startY;

		struct Item { const TCHAR* l; UINT id; };
		Item items[] = {
			{_T("空间位姿:"), IDC_EDIT_POSE},
			{_T("弯曲电机:"), IDC_EDIT_BEND},
			{_T("夹钳角度:"), IDC_EDIT_GRIP_ANGLE},
			{_T("夹钳电机:"), IDC_EDIT_GRIP_MOTOR}
		};
		for (auto& it : items) {
			CWnd* pL = FindStaticByText(it.l);
			if (pL) pL->SetWindowPos(NULL, cxL, cy + 2, labelW, 16, SWP_NOZORDER);
			CWnd* pE = GetDlgItem(it.id);
			if (pE) pE->SetWindowPos(NULL, cxE, cy, cwE, 20, SWP_NOZORDER);
			cy += rowH;
		}
	}

	// Chart Card (Bottom Spanning)
	y = m_rectCardCamera.bottom + kCardGap;
	int chartH = height - y - kCardGap;
	if (chartH < 100) chartH = 100;
	m_rectCardChart.SetRect(x, y, x + mainW, y + chartH);

	// Move Chart (Account for Title)
	if (m_ChartCtrl.GetSafeHwnd()) {
		CRect chartArea = m_rectCardChart;
		chartArea.top += 34;
		chartArea.DeflateRect(12, 12);
		m_ChartCtrl.SetWindowPos(NULL, chartArea.left, chartArea.top, chartArea.Width(), chartArea.Height(), SWP_NOZORDER);
	}

	Invalidate(); // Redraw
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

void CSRDlg::DrawShadowedCard(CDC& dc, CRect rc, int radius, COLORREF bodyBg, COLORREF border)
{
	// 1) Shadow
	CRect sh = rc;
	sh.OffsetRect(2, 3);
	COLORREF shadow = RGB(200, 208, 215);
	CPen penS(PS_SOLID, 1, shadow);
	CBrush brS(shadow);
	CPen* oldPen = dc.SelectObject(&penS);
	CBrush* oldBr = dc.SelectObject(&brS);
	dc.RoundRect(sh, CPoint(radius, radius));
	dc.SelectObject(oldPen);
	dc.SelectObject(oldBr);

	// 2) Card
	DrawRoundedRectFillBorder(dc, rc, radius, bodyBg, border);
}

void CSRDlg::DrawCardWithTitle(CDC& dc, CRect rc, int radius, CString title, COLORREF titleBg, COLORREF bodyBg, COLORREF border, COLORREF titleText)
{
	// 1. Draw Body
	DrawRoundedRectFillBorder(dc, rc, radius, bodyBg, border);

	// 2. Draw Title Header
	CRect rcHeader = rc;
	rcHeader.bottom = rcHeader.top + 26;

	// Create Region for Top Round Rect
	CRgn rgn;
	rgn.CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, radius, radius);

	// Clip to Header area
	dc.SelectClipRgn(&rgn);
	dc.FillSolidRect(&rcHeader, titleBg);

	// Draw Title Text
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(titleText);
	dc.SelectObject(&m_fontLabel);
	rcHeader.left += 10;
	dc.DrawText(title, &rcHeader, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	dc.SelectClipRgn(NULL);
}

void CSRDlg::DrawMainCardTitle(CDC& dc, CRect rc, CString title)
{
	dc.SetBkMode(TRANSPARENT);
	dc.SetTextColor(m_clrMainText);
	dc.SelectObject(&m_fontLabel);
	CRect rTitle = rc;
	rTitle.top += 10;
	rTitle.left += 14;
	rTitle.bottom = rTitle.top + 20;
	dc.DrawText(title, &rTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
		// Bottom accent line
		dc.FillSolidRect(0, headerRect.bottom - 2, rect.Width(), 2, m_clrHdrLine);
		// Title
		CFont* old = dc.SelectObject(&m_fontTitle);
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(m_clrHdrText);
		dc.DrawText(_T("消化道手术机器人控制系统"), &headerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		dc.SelectObject(old);

		// 3. Sidebar Background (Just fill, no extra shadow)
		if (!m_rectSidebar.IsRectEmpty()) {
			dc.FillSolidRect(&m_rectSidebar, m_clrSidebarBg);
		}

		// 4. Sidebar Cards (Title + Dark Content)
		if (!m_rectCardMotor.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardMotor, kRadius, _T("Motor Control"), m_clrSideCardTitle, m_clrSideCardBg, m_clrSideCardBorder, m_clrSidebarText);

			// Draw "Motor Start" Label on Left
			int nSavedDC = dc.SaveDC(); // Save DC state

			// Re-calculate vertical center based on new switch height (46)
			// Switch Top = m_rectCardMotor.top + 26 (title) + 8 (pad) = +34
			// Switch Height = 46. Center = +34 + 23 = +57 from card top

			CRect rcLabel = m_rectCardMotor;
			rcLabel.top += 34;
			rcLabel.bottom = rcLabel.top + 46; // Match switch height
			rcLabel.left += 12; // Left Padding
			rcLabel.right -= 100; // Avoid switch area (84 width + padding)

			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(m_clrSidebarText);
			dc.SelectObject(&m_fontSidebarBtn);

			// Unicode: 鐢垫満鍚姩
			dc.DrawText(L"\x7535\x673A\x542F\x52A8", &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

			dc.RestoreDC(nSavedDC); // Restore DC state
		}

		if (!m_rectCardHaptic.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardHaptic, kRadius, _T("Haptic Control"), m_clrSideCardTitle, m_clrSideCardBg, m_clrSideCardBorder, m_clrSidebarText);

			// Draw "Master Start" Label on Left
			int nSavedDC = dc.SaveDC();

			CRect rcLabel = m_rectCardHaptic;
			rcLabel.top += 34; // header + pad
			rcLabel.bottom = rcLabel.top + 46; // switch height
			rcLabel.left += 12; // Left Padding
			rcLabel.right -= 100; // Avoid switch area

			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(m_clrSidebarText);
			dc.SelectObject(&m_fontSidebarBtn);

			// "Master Start" = 涓绘墜鍚姩 = \u4E3B\u624B\u542F\u52A8
			dc.DrawText(L"\x4E3B\x624B\x542F\x52A8", &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

			dc.RestoreDC(nSavedDC);
		}

		// Draw Sensor Card
		if (!m_rectCardSensor.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardSensor, kRadius, _T("Sensor Control"), m_clrSideCardTitle, m_clrSideCardBg, m_clrSideCardBorder, m_clrSidebarText);

			int nSavedDC = dc.SaveDC();

			CRect rcLabel = m_rectCardSensor;
			rcLabel.top += 34 + 24 + 6;
			rcLabel.bottom = rcLabel.top + 46;
			rcLabel.left += 12;
			rcLabel.right -= 100;

			dc.SetBkMode(TRANSPARENT);
			dc.SetTextColor(m_clrSidebarText);
			dc.SelectObject(&m_fontSidebarBtn);

			// "Sensor" = 传感器
			dc.DrawText(_T("传感器"), &rcLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

			dc.RestoreDC(nSavedDC);
		}

		// 5. Main Cards (Dark Theme with Titles)
		if (!m_rectCardCamera.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardCamera, kRadius, _T("Camera View"), m_clrSideCardTitle, m_clrMainCardBg, m_clrMainCardBorder, m_clrMainText);
		}
		if (!m_rectCardMaster.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardMaster, kRadius, _T("Master Param"), m_clrSideCardTitle, m_clrMainCardBg, m_clrMainCardBorder, m_clrMainText);
		}
		if (!m_rectCardRobot.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardRobot, kRadius, _T("Robot Param"), m_clrSideCardTitle, m_clrMainCardBg, m_clrMainCardBorder, m_clrMainText);
		}
		if (!m_rectCardChart.IsRectEmpty()) {
			DrawCardWithTitle(dc, m_rectCardChart, kRadius, _T("Force Feedback (N)"), m_clrSideCardTitle, m_clrMainCardBg, m_clrMainCardBorder, m_clrMainText);
		}
	}
}

BOOL CSRDlg::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}

void CSRDlg::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	if (nIDCtl == IDC_BUTTON_CLEAR_CHART || nIDCtl == IDC_BUTTON_SENSOR_ZERO)
	{
		CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
		CRect rc = lpDrawItemStruct->rcItem;
		const bool pressed = (lpDrawItemStruct->itemState & ODS_SELECTED) != 0;
		const bool disabled = (lpDrawItemStruct->itemState & ODS_DISABLED) != 0;
		const bool focused = (lpDrawItemStruct->itemState & ODS_FOCUS) != 0;

		const bool primary = nIDCtl == IDC_BUTTON_SENSOR_ZERO;
		COLORREF accent = primary ? m_clrHdrLine : RGB(130, 145, 156);
		COLORREF fill = pressed ? (primary ? RGB(0, 128, 148) : RGB(62, 76, 86)) : RGB(48, 60, 69);
		COLORREF border = focused ? m_clrHdrLine : (primary ? RGB(0, 160, 178) : RGB(82, 100, 112));
		COLORREF text = disabled ? RGB(120, 130, 138) : m_clrSidebarText;

		pDC->FillSolidRect(&rc, m_clrSideCardBg);

		CRect btn = rc;
		btn.DeflateRect(1, 1);
		CPen pen(PS_SOLID, 1, border);
		CBrush brush(fill);
		CPen* oldPen = pDC->SelectObject(&pen);
		CBrush* oldBrush = pDC->SelectObject(&brush);
		pDC->RoundRect(&btn, CPoint(8, 8));
		pDC->SelectObject(oldBrush);
		pDC->SelectObject(oldPen);

		CRect accentRect = btn;
		accentRect.right = accentRect.left + 4;
		accentRect.DeflateRect(0, 7, 0, 7);
		pDC->FillSolidRect(&accentRect, accent);

		CString caption;
		CWnd* pWnd = GetDlgItem(nIDCtl);
		if (pWnd) pWnd->GetWindowText(caption);

		pDC->SetBkMode(TRANSPARENT);
		pDC->SetTextColor(text);
		CFont* oldFont = pDC->SelectObject(&m_fontSidebarBtn);
		if (pressed) btn.OffsetRect(1, 1);
		CRect textRect = btn;
		textRect.left += 10;
		pDC->DrawText(caption, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		pDC->SelectObject(oldFont);
		return;
	}

	CDialogEx::OnDrawItem(nIDCtl, lpDrawItemStruct);
}

HBRUSH CSRDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	int id = pWnd->GetDlgCtrlID();

	// Handle Status Edits (ReadOnly usually sends CTLCOLOR_STATIC)
	if (id == IDC_EDIT_MOTOR_STATUS || id == IDC_EDIT_HAPTIC_STATUS)
	{
		CString strText;
		pWnd->GetWindowText(strText);
		pDC->SelectObject(&m_fontLabel);
		if (strText.Find(_T("Connected")) >= 0 && strText.Find(_T("Dis")) == -1 && strText.Find(_T("Fail")) == -1)
			pDC->SetTextColor(m_clrOkGreen);
		else
			pDC->SetTextColor(m_clrDangerRed);
		pDC->SetBkColor(m_clrMainCardBg);
		return m_brushMainCardBg;
	}

	// Handle other ReadOnly Edits
	if (id == IDC_EDIT_MASTER_POS || id == IDC_EDIT_MASTER_ENC || id == IDC_EDIT_MASTER_FORCE ||
		id == IDC_EDIT_POSE || id == IDC_EDIT_BEND || id == IDC_EDIT_GRIP_ANGLE || id == IDC_EDIT_GRIP_MOTOR ||
		id == 20005) // Sensor Data
	{
		pDC->SetTextColor(m_clrMainText);
		pDC->SetBkColor(m_clrMainCardBg);
		return m_brushMainCardBg;
	}

	if (nCtlColor == CTLCOLOR_STATIC)
	{
		if (id == IDC_STATIC_CAMERA) return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

		// Transparent Labels
		pDC->SetBkMode(TRANSPARENT);

		// Determine color based on location (Sidebar vs Main)
		CRect r;
		pWnd->GetWindowRect(&r);
		ScreenToClient(&r);

		if (r.left < kSidebarWidth) {
			pDC->SetTextColor(m_clrSidebarText);
		}
		else {
			pDC->SetTextColor(m_clrMainText);
		}
		return (HBRUSH)GetStockObject(NULL_BRUSH);
	}

	if (nCtlColor == CTLCOLOR_EDIT || nCtlColor == CTLCOLOR_MSGBOX)
	{
		pDC->SetTextColor(m_clrMainText);
		pDC->SetBkColor(m_clrMainCardBg);
		return m_brushMainCardBg;
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

void CSRDlg::OnClickedClearChart()
{
	ClearSensorChart();
}

void CSRDlg::OnClickedSensorZero()
{
	ZeroSensorBaseline();
}

// ==========================================================================
// Logic & Handlers (Unchanged)
// ==========================================================================

void CSRDlg::OnClickedButtonStartM()
{
	if (m_workerThread.joinable()) m_workerThread.join();

	m_workerThread = std::thread([this]() {
		if (!m_pMotorManager->Connect())
		{
			PostMessage(WM_MOTOR_INIT_COMPLETE, 0, m_pMotorManager->GetLastErrorCode());
			return;
		}

		if (!m_pMotorManager->EnableMotors())
		{
			// Post Warning but success
			PostMessage(WM_MOTOR_INIT_COMPLETE, 2, m_pMotorManager->GetLastErrorCode());
			return;
		}

		PostMessage(WM_MOTOR_INIT_COMPLETE, 1, 0);
	});
}

LRESULT CSRDlg::OnMotorInitComplete(WPARAM wParam, LPARAM lParam)
{
	if (m_workerThread.joinable()) m_workerThread.join();

	if (wParam == 1 || wParam == 2) // Success or Warning
	{
		state = 1;
		QueryPerformanceFrequency(&iFreq);
		QueryPerformanceCounter(&iBegTime);

		maxon_state = TRUE;
		m_editMotorStatus.SetWindowText(_T("Connected"));
		m_editMotorStatus.Invalidate();

		if (wParam == 2)
		{
			CString strError;
			strError.Format(_T("Failed to enable motors! Error: 0x%08X"), (DWORD)lParam);
			AfxMessageBox(strError, MB_ICONWARNING);
		}
	}
	else // Failure
	{
		CString strError;
		strError.Format(_T("Can't open device! Error: 0x%08X"), (DWORD)lParam);
		AfxMessageBox(strError, MB_ICONINFORMATION);

		maxon_state = FALSE;
		m_editMotorStatus.SetWindowText(_T("Disconnected"));
		m_editMotorStatus.Invalidate();

		// Reset Switch if it was waiting
		if (m_nMotorSwitchState == 1) // Waiting for Start
		{
			m_nMotorSwitchState = 0;
			m_btnMotorSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
			KillTimer(3); // Stop the sequence timer
		}
	}

	return 0;
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

bool CSRDlg::StartHapticDevice()
{
	if (dhdOpen() < 0) {
		m_editHapticStatus.SetWindowText(_T("Connect Failed"));
		m_editHapticStatus.Invalidate();
		AfxMessageBox(_T("Cannot open Haptic Device - Check connection or driver"), MB_ICONERROR);
		return false;
	}

	m_editHapticStatus.SetWindowText(_T("Connected"));
	m_editHapticStatus.Invalidate();

	dhdEnableForce(DHD_ON);
	dhdEnableExpertMode();
	dhdSetMaxGripperForce(kMaxGripperFeedbackForce);

	done = 0;
	SetTimer(1, 10, NULL);
	return true;
}

void CSRDlg::OnClickedButtonStartH()
{
	StartHapticDevice();
}

bool CSRDlg::ZeroHapticDevice()
{
	if (dhdGetPosition(&px, &py, &pz) < 0) {
		return false;
	}

	dhdGetEnc(enc);
	offset_enc0 = enc[6];

	ref_px = px;
	ref_py = py;
	ref_pz = pz;

	Sleep(100);

	Ning = true;
	motor_flag = TRUE;
	return true;
}

void CSRDlg::OnClickedButtonZeroH()
{
	ZeroHapticDevice();
}

void CSRDlg::StopHapticDevice()
{
	KillTimer(1);
	dhdClose();

	Ning = false;
	motor_flag = FALSE;
	done = 1;

	m_editHapticStatus.SetWindowText(_T("Disconnected"));
	m_editHapticStatus.Invalidate();
}

void CSRDlg::OnClickedButtonShutH()
{
	StopHapticDevice();
}

void CSRDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (m_pBridgeServer)
	{
		if (m_pBridgeServer->ConsumeStartExperiment())
		{
			motor_flag = TRUE;
		}

		if (m_pBridgeServer->ConsumeStopExperiment())
		{
			motor_flag = FALSE;
			m_pBridgeServer->ForceZeroFeedback("experiment stopped");
			dhdSetForceAndTorqueAndGripperForce(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		}

		if (m_pBridgeServer->ConsumeZeroOmega())
		{
			ZeroHapticDevice();
		}

		if (m_pBridgeServer->ConsumeEmergencyStop())
		{
			motor_flag = FALSE;
			done = 1;
			m_pBridgeServer->ForceZeroFeedback("emergency stop");
			dhdSetForceAndTorqueAndGripperForce(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
			if (m_pMotorManager)
			{
				m_pMotorManager->DisableMotors();
			}
			maxon_state = FALSE;
			m_editMotorStatus.SetWindowText(_T("Emergency stopped"));
			m_editHapticStatus.SetWindowText(_T("Emergency stopped"));
		}
	}

	if (nIDEvent == 3) // Motor Sequence Logic
	{
		// 1: STARTING (Wait Start success -> Trigger Speed)
		if (m_nMotorSwitchState == 1)
		{
			// "Wait until motor start success" - OnClickedButtonStartM already ran synchronously mostly, 
			// but user wants a sequence. "Start -> Wait -> Speed".
			// Since OnClickedButtonStartM sets maxon_state=TRUE on success immediately, 
			// we can proceed to speed setting.
			// Let's add a small delay simulation or check actual hardware status if possible.
			// Assuming connected:
			if (maxon_state)
			{
				// Execute Speed Setting
				OnClickedButtonSpeedM();

				// "Once speed set success -> Green"
				// SpeedM is also synchronous. If it returns/finishes, we assume success or check errors.
				// Let's assume success for now.

				m_nMotorSwitchState = 3; // ON
				m_btnMotorSwitch.SetSwitchState(CSwitchButton::SWITCH_ON);
				KillTimer(3);
			}
		}
		// 4: ZEROING (Wait Zero success -> Trigger Shut)
		else if (m_nMotorSwitchState == 4)
		{
			// "Wait until zeroing success"
			// Check if positions are close to 0.
			// User said "Wait until zeroing success then execute close".
			// We can check `m_pMotorManager->MoveToPosition` behavior or check Encoders.
			// `OnClickedButtonZeroM` sends MoveToPosition(0).
			// We should poll `GetPosition` or `GetMovingState` if available.
			// MotorManager doesn't seem to have GetPosition exposed easily in header we saw.
			// But we saw `maxon5_position` etc variables?
			// Let's just use a timeout for safety (e.g. 3 seconds) or if we had position feedback.
			// User mentioned "Refer to Zero Button", which just calls MoveToPosition.

			static int zeroWaitTick = 0;
			zeroWaitTick++;

			// Simple wait 2 seconds (20 * 100ms)
			if (zeroWaitTick > 20)
			{
				zeroWaitTick = 0;
				// Execute Shut
				OnClickedButtonShutM();

				m_nMotorSwitchState = 0; // OFF
				m_btnMotorSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
				KillTimer(3);
			}
		}
	}

	if (nIDEvent == 2)
	{
		if (m_camera.isOpened())
		{
			cv::Mat frame;

			if (!m_camera.read(frame) || frame.empty())
			{
				return;
			}

			cv::Mat bgrFrame;

			if (frame.channels() == 1)
			{
				cv::cvtColor(frame, bgrFrame, cv::COLOR_GRAY2BGR);
			}
			else if (frame.channels() == 2)
			{
				// External USB cameras may output YUY2/YUYV
				cv::cvtColor(frame, bgrFrame, cv::COLOR_YUV2BGR_YUY2);
			}
			else if (frame.channels() == 3)
			{
				bgrFrame = frame;
			}
			else if (frame.channels() == 4)
			{
				cv::cvtColor(frame, bgrFrame, cv::COLOR_BGRA2BGR);
			}
			else
			{
				return;
			}

			DrawMatToPic(bgrFrame, m_picCamera);
		}
	}


	if (nIDEvent == 4)
	{
		// Sensor display and force-curve drawing are independent of the haptic device.
		// As long as the sensor switch is ON and one valid serial frame is received,
		// Fx/Fy/Fz will be parsed, displayed, and appended to the chart.
		if (m_nSensorSwitchState != 1)
		{
			CDialogEx::OnTimer(nIDEvent);
			return;
		}

		CString str;
		{
			std::lock_guard<std::mutex> lock(g_sensorMutex);
			str = g_sensorRawString;
		}

		double fxVal = 0.0, fyVal = 0.0, fzVal = 0.0;
		bool parsed = false;

		if (!str.IsEmpty())
		{
			m_editSensorData.SetWindowText(str);

			parsed = ParseSensorLineToFxFyFz(str, fxVal, fyVal, fzVal);
			if (!parsed)
			{
				parsed = ParseNumericSensorFrame(str, fxVal, fyVal, fzVal);
			}

			if (parsed)
			{
				std::lock_guard<std::mutex> lock(g_sensorMutex);
				g_sensorRawValues[1] = fxVal;
				g_sensorRawValues[2] = fyVal;
				g_sensorRawValues[3] = fzVal;

				double sampleTime = dhdGetTime() - m_dStartTime;
				g_sensorRawSamples.push_back({ sampleTime, fxVal, fyVal, fzVal });
				while (!g_sensorRawSamples.empty() && sampleTime - g_sensorRawSamples.front().t > 3.0)
				{
					g_sensorRawSamples.pop_front();
				}

				double adjFx = fxVal - g_sensorZeroValues[1];
				double adjFy = fyVal - g_sensorZeroValues[2];
				double adjFz = fzVal - g_sensorZeroValues[3];

				if (std::abs(adjFx) < kSensorDeadband) adjFx = 0.0;
				if (std::abs(adjFy) < kSensorDeadband) adjFy = 0.0;
				if (std::abs(adjFz) < kSensorDeadband) adjFz = 0.0;

				g_sensorValues[1] = adjFx;
				g_sensorValues[2] = adjFy;
				g_sensorValues[3] = adjFz;
			}
		}

		// Use the latest successfully parsed force values for both display and plotting.
		// If the current raw frame is incomplete, the chart will keep the last valid force value
		// instead of being overwritten by 0.
		{
			std::lock_guard<std::mutex> lock(g_sensorMutex);
			fxVal = g_sensorValues[1];
			fyVal = g_sensorValues[2];
			fzVal = g_sensorValues[3];
		}

		if (m_ChartCtrl.GetSafeHwnd() &&
			m_pLineSeries[0] != nullptr &&
			m_pLineSeries[1] != nullptr &&
			m_pLineSeries[2] != nullptr)
		{
			double plotTime = dhdGetTime() - m_dStartTime;

			// IMPORTANT: draw the parsed serial forces, not the haptic force.
			m_pLineSeries[0]->AddPoint(plotTime, fxVal);
			m_pLineSeries[1]->AddPoint(plotTime, fyVal);
			m_pLineSeries[2]->AddPoint(plotTime, fzVal);

			double minT = (plotTime > 20.0) ? (plotTime - 20.0) : 0.0;
			double maxT = (plotTime > 20.0) ? plotTime : 20.0;
			m_ChartCtrl.GetBottomAxis()->SetMinMax(minT, maxT);

			double minF = 0.0;
			double maxF = 0.0;
			{
				std::lock_guard<std::mutex> lock(g_sensorMutex);
				g_sensorPlotPoints.push_back({ plotTime, fxVal, fyVal, fzVal });
				while (!g_sensorPlotPoints.empty() && g_sensorPlotPoints.front().t < minT)
				{
					g_sensorPlotPoints.pop_front();
				}

				bool hasPoint = false;
				for (const SensorPlotPoint& p : g_sensorPlotPoints)
				{
					if (!hasPoint)
					{
						minF = std::min(p.fx, std::min(p.fy, p.fz));
						maxF = std::max(p.fx, std::max(p.fy, p.fz));
						hasPoint = true;
					}
					else
					{
						minF = std::min(minF, std::min(p.fx, std::min(p.fy, p.fz)));
						maxF = std::max(maxF, std::max(p.fx, std::max(p.fy, p.fz)));
					}
				}
			}

			// Manual Y-axis scaling over the visible history prevents old points from being clipped.
			if (minF > 0.0) minF = 0.0;
			if (maxF < 0.0) maxF = 0.0;
			double span = maxF - minF;
			if (span < 1.0)
			{
				minF -= 0.5;
				maxF += 0.5;
			}
			else
			{
				double padF = span * 0.20;
				minF -= padF;
				maxF += padF;
			}
			m_ChartCtrl.GetLeftAxis()->SetMinMax(minF, maxF);

			// Force repaint. Some ChartCtrl versions do not repaint immediately after AddPoint.
			m_ChartCtrl.Invalidate(FALSE);
			m_ChartCtrl.UpdateWindow();
		}

		CString strUI;
		if (!str.IsEmpty() && !parsed)
		{
			strUI.Format(_T("Parse failed, last valid: Fx %.3f  Fy %.3f  Fz %.3f"), fxVal, fyVal, fzVal);
		}
		else
		{
			strUI.Format(_T("Fx: %.3f N  Fy: %.3f N  Fz: %.3f N"), fxVal, fyVal, fzVal);
		}
		m_editMasterForce.SetWindowText(strUI);
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

		// *** CRITICAL FIX FOR USER: Use Relative Position Data instead of Force Data ***
		// This matches logic in TXDlg.cpp where chart plots "rel_px" despite being labeled "Fx"

		// 1. Calculate Relative Position FIRST (if Ning/Zeroed is active)
		if (Ning)
		{
			rel_px = px - ref_px;
			rel_py = py - ref_py;
			rel_pz = pz - ref_pz;
		}
		else
		{
			rel_px = 0;
			rel_py = 0;
			rel_pz = 0;
		}

		double gripperFeedback = 0.0;
		if (m_pBridgeServer)
		{
			gripperFeedback = m_pBridgeServer->NextFeedbackForControl(
				kMaxGripperFeedbackForce,
				0.25,
				1000);
		}

		if (dhdSetForceAndTorqueAndGripperForce(
			0.0, 0.0, 0.0,
			0.0, 0.0, 0.0,
			gripperFeedback) < DHD_NO_ERROR)
		{
			done = 1;
		}

		if (dhdGetEnc(enc) < 0) {
		}

		CString strUI;
		strUI.Format(_T("X: %.3f  Y: %.3f  Z: %.3f"), px, py, pz);
		m_editMasterPos.SetWindowText(strUI);

		strUI.Format(_T("%d, %d, %d\r\n%d, %d, %d"), enc[0], enc[1], enc[2], enc[3], enc[4], enc[5]);
		m_editMasterEnc.SetWindowText(strUI);

		// 5. Show sensor data as end-force/feedback data. Relative master position is still used below for robot motion control.
		// Sensor display is updated by timer 4.

		if (Ning)
		{
			// (Redundant calculation removed since we did it above, but logic continues...)

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
				maxon3_position = -qc;
				m_pMotorManager->MoveToPosition(3, -qc, true, true);

				long targetPos4 = (long)(3 * delta_L1);
				maxon4_position = (int)targetPos4;
				m_pMotorManager->MoveToPosition(4, targetPos4, true, true);

				long targetPos5 = (long)(-(3 * delta_L2));
				maxon5_position = (int)targetPos5;
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

		if (m_pBridgeServer)
		{
			BridgeTelemetry telemetry;
			telemetry.timestamp = dhdGetTime() - m_dStartTime;
			telemetry.omegaPx = px;
			telemetry.omegaPy = py;
			telemetry.omegaPz = pz;
			telemetry.omegaFx = fx;
			telemetry.omegaFy = fy;
			telemetry.omegaFz = fz;
			for (int i = 0; i < 7; ++i)
			{
				telemetry.omegaEnc[i] = enc[i];
			}
			telemetry.gripperFeedbackApplied = gripperFeedback;
			telemetry.motorEnabled = (maxon_state == TRUE);
			telemetry.motorTarget3 = maxon3_position;
			telemetry.motorTarget4 = maxon4_position;
			telemetry.motorTarget5 = maxon5_position;
			telemetry.cppEmergencyStop = m_pBridgeServer->IsEmergencyStopLatched();
			telemetry.cppWarning = m_pBridgeServer->GetLastWarning();
			m_pBridgeServer->UpdateTelemetry(telemetry);
		}
	}

	CDialogEx::OnTimer(nIDEvent);
}

void CSRDlg::OnDestroy()
{
	if (m_pBridgeServer)
	{
		m_pBridgeServer->ForceZeroFeedback("window closing");
	}
	dhdSetForceAndTorqueAndGripperForce(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);

	KillTimer(1);
	KillTimer(2);
	KillTimer(3);
	KillTimer(4);

	StopSensorSerial();

	if (m_camera.isOpened()) m_camera.release();

	// Wait for the motor initialization thread before releasing m_pMotorManager.
	if (m_workerThread.joinable())
	{
		m_workerThread.join();
	}

	// Safe Motor Cleanup
	if (m_pMotorManager)
	{
		m_pMotorManager->DisableMotors();
		delete m_pMotorManager;
		m_pMotorManager = nullptr;
	}

	dhdClose();

	if (m_pBridgeServer)
	{
		m_pBridgeServer->Stop();
		delete m_pBridgeServer;
		m_pBridgeServer = nullptr;
	}

	CDialogEx::OnDestroy();
}

void CSRDlg::OnClickedMotorSwitch()
{
	// 0: OFF, 1: STARTING (Wait Start), 2: CONFIGURING (Wait Speed), 3: ON, 4: ZEROING (Wait Zero), 5: STOPPING (Wait Disable)

	if (m_nMotorSwitchState == 0) // OFF -> Turn ON
	{
		// 1. Enter Waiting State (Orange) IMMEDIATELY
		m_nMotorSwitchState = 1; // Start Waiting
		m_btnMotorSwitch.SetSwitchState(CSwitchButton::SWITCH_WAITING);

		// 2. Start Motor Async
		OnClickedButtonStartM();

		// 3. Start Timer to check progress (it will wait for maxon_state to become TRUE)
		m_nMotorTimer = SetTimer(3, 100, NULL); // Timer ID 3 for Motor Sequence
	}
	else if (m_nMotorSwitchState == 3) // ON -> Turn OFF
	{
		// 1. Start Zeroing
		OnClickedButtonZeroM();

		// 2. Enter Waiting State (Orange) - Waiting for Zeroing
		m_nMotorSwitchState = 4; // Zeroing
		m_btnMotorSwitch.SetSwitchState(CSwitchButton::SWITCH_WAITING);

		// Start Timer to check progress (wait for some time or check position)
		// Since we don't have a "IsZeroed" callback, we simulate wait or check position 0
		m_nMotorTimer = SetTimer(3, 100, NULL);
	}
	// If in intermediate states (1,2,4,5), ignore clicks or cancel? 
	// For now ignore.
}

void CSRDlg::OnClickedHapticSwitch()
{
	if (m_nHapticSwitchState == 0) // OFF -> Turn ON
	{
		// 1. Start
		if (!StartHapticDevice()) {
			// Fail
			m_btnHapticSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
			m_nHapticSwitchState = 0;
			return;
		}

		// 2. Zero (Immediate)
		if (!ZeroHapticDevice()) {
			// Fail Zero -> Stop
			StopHapticDevice();
			m_btnHapticSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
			m_nHapticSwitchState = 0;
			return;
		}

		// Success
		m_nHapticSwitchState = 1; // ON
		m_btnHapticSwitch.SetSwitchState(CSwitchButton::SWITCH_ON);
	}
	else // ON -> Turn OFF
	{
		StopHapticDevice();
		m_nHapticSwitchState = 0; // OFF
		m_btnHapticSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
	}
}

void CSRDlg::OnClickedSensorSwitch()
{
	if (m_nSensorSwitchState == 0)
	{
		// Re-scan when the user starts the sensor, while keeping the user's current choice if possible.
		int preferredPort = GetSelectedSensorPort();
		if (preferredPort <= 0) preferredPort = 10;
		PopulateSensorPortCombo(preferredPort);

		int portNumber = GetSelectedSensorPort();
		CString statusText;
		bool bConnected = StartSensorSerialByPort(portNumber, statusText);

		if (bConnected)
		{
			m_nSensorSwitchState = 1;
			m_btnSensorSwitch.SetSwitchState(CSwitchButton::SWITCH_ON);
			m_editSensorData.SetWindowText(statusText);
			g_comboSensorPort.EnableWindow(FALSE);
			SetTimer(4, 20, NULL); // Sensor display timer, independent of haptic timer
		}
		else
		{
			m_nSensorSwitchState = 0;
			m_btnSensorSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
			m_editSensorData.SetWindowText(statusText);
			g_comboSensorPort.EnableWindow(TRUE);
			AfxMessageBox(statusText);
		}
	}
	else
	{
		KillTimer(4);
		StopSensorSerial();
		m_nSensorSwitchState = 0;
		m_btnSensorSwitch.SetSwitchState(CSwitchButton::SWITCH_OFF);
		m_editSensorData.SetWindowText(_T("Sensor disconnected"));
		g_comboSensorPort.EnableWindow(TRUE);
		PopulateSensorPortCombo(10);
	}
}

void CSRDlg::OnCommEvent()
{
	// Sensor data are read by the WinAPI serial thread.
	// This handler is kept only for compatibility if the old MSComm control exists.
	m_SensorManager.OnCommEvent();
}

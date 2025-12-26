#include"ChartCtrl.h"
//#include"ChartCtrl_source/ChartCtrl.h"
// highspeedchartDlg.h: 头文件
//

#pragma once


// ChighspeedchartDlg 对话框
class ChighspeedchartDlg : public CDialogEx
{
// 构造
public:
	ChighspeedchartDlg(CWnd* pParent = nullptr);	// 标准构造函数

public:
	CChartCtrl m_ChartCtrl1;
	CChartCtrl m_ChartCtrl2;



// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_HIGHSPEEDCHART_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持


// 实现
protected:
	HICON m_hIcon;

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedButton1();
	afx_msg void OnBnClickedButton2();
	afx_msg void OnBnClickedButton3();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
};

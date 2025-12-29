#pragma once
#include "framework.h"
#include <string>
#include <vector>
#include <mutex>
#include "CMSCOMM1.h" // Assuming this is the generated wrapper for MSComm

// Callback or Interface for data updates could be used, 
// but for simplicity we'll expose data via getters or a buffer.

class SensorManager
{
public:
	SensorManager();
	~SensorManager();

	// 1. Auto-Enumeration
	std::vector<CString> FindAvailablePorts();

	// 2. Connection
	bool AutoConnect(); // Scans and tries to connect to first available? Or specific logic?
	bool Connect(CString portName);
	void Disconnect();
	bool IsConnected() const { return m_bConnected; }

	// 3. Data Processing (Called by external Timer or OnComm)
	void ProcessBuffer(); 
	
	// Getters for UI
	double GetSensorValue(int index); // Return parsed values
	CString GetLastRawString();

	// CMSCOMM Event Handler Helper
	void OnCommEvent();

	// We need to attach the MSComm control from the Dialog
	void AttachComm(CMSCOMM1* pComm);

private:
	CMSCOMM1* m_pComm;
	bool m_bConnected;
	
	// Buffer
	static const int BUFFER_SIZE = 4096;
	char m_rxBuffer[BUFFER_SIZE];
	int m_rxHead;
	int m_rxTail; // Not circular for now, just append string
	std::string m_strBuffer; // Easier to handle line parsing
	
	std::mutex m_mutex;
	std::vector<double> m_parsedData;

	// Helper to set settings: "115200,n,8,1"
	void ConfigurePort();
};

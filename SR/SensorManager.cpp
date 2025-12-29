#include "pch.h"
#include "SensorManager.h"
#include <vector>
#include <string>
#include <sstream>
#include <iostream>

SensorManager::SensorManager() 
	: m_pComm(nullptr), m_bConnected(false), m_rxHead(0), m_rxTail(0)
{
}

SensorManager::~SensorManager()
{
	Disconnect();
}

void SensorManager::AttachComm(CMSCOMM1* pComm)
{
	m_pComm = pComm;
}

std::vector<CString> SensorManager::FindAvailablePorts()
{
	std::vector<CString> ports;
	for (int i = 1; i <= 255; i++)
	{
		CString strPort;
		strPort.Format(_T("\\\\.\\COM%d"), i);

		HANDLE hPort = ::CreateFile(strPort, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (hPort != INVALID_HANDLE_VALUE)
		{
			CloseHandle(hPort);
			strPort.Format(_T("COM%d"), i);
			ports.push_back(strPort);
		}
	}
	return ports;
}

bool SensorManager::AutoConnect()
{
	if (IsConnected()) return true;

	std::vector<CString> ports = FindAvailablePorts();
	if (ports.empty()) return false;

	// Simple logic: Try to connect to the last one (usually the USB one plugged in)
	// Or try all. For now, try the last one as it's often the new device.
	// In a real scenario, we might handshake.
	for (auto it = ports.rbegin(); it != ports.rend(); ++it)
	{
		if (Connect(*it)) return true;
	}
	return false;
}

bool SensorManager::Connect(CString portName)
{
	if (!m_pComm) return false;
	
	Disconnect();

	// Remove "COM" prefix to get ID if needed, but CommPort takes ID
	// "COM3" -> 3
	int portID = _ttoi(portName.Mid(3));
	if (portID <= 0) return false;

	try
	{
		m_pComm->put_CommPort(portID);
		m_pComm->put_Settings(_T("115200,n,8,1"));
		m_pComm->put_InputMode(1); // 1 = Binary
		m_pComm->put_RThreshold(1); // Trigger OnComm for every char
		m_pComm->put_InputLen(0); // Read all
		m_pComm->put_PortOpen(TRUE);
		
		m_bConnected = true;
		return true;
	}
	catch (...)
	{
		m_bConnected = false;
		return false;
	}
}

void SensorManager::Disconnect()
{
	if (m_pComm && m_bConnected)
	{
		try {
			if (m_pComm->get_PortOpen())
				m_pComm->put_PortOpen(FALSE);
		} catch(...) {}
	}
	m_bConnected = false;
	m_strBuffer.clear();
}

void SensorManager::OnCommEvent()
{
	if (!m_pComm || !m_bConnected) return;

	// Check for Receive Event
	if (m_pComm->get_CommEvent() == 2) // 2 = comEvReceive
	{
		VARIANT inputData = m_pComm->get_Input();
		
		// Convert VARIANT (SafeArray of bytes) to string/char
		if (inputData.vt == (VT_ARRAY | VT_UI1))
		{
			COleSafeArray safeArray(inputData);
			long len = 0;
			safeArray.GetUBound(1, &len);
			len++; // 0-based index
			
			if (len > 0)
			{
				BYTE* pData = nullptr;
				safeArray.AccessData((void**)&pData);
				
				std::lock_guard<std::mutex> lock(m_mutex);
				// Append to internal string buffer
				// Not efficient for huge streams but fine for CSV lines
				m_strBuffer.append((char*)pData, len);
				
				safeArray.UnaccessData();
			}
		}
	}
}

void SensorManager::ProcessBuffer()
{
	std::lock_guard<std::mutex> lock(m_mutex);

	// Look for valid frames: 99, ..., 99
	// Frame Header/Footer = 99
	// Protocol: CSV format.
	// Strategy: Find "99", then find next "99". 
	// Warning: "99" might be part of a number (e.g. 1.99). 
	// The requirement says "First data point is 99", "Last is 99".
	// Implies comma separated: "99, 12.3, 45.6, 99"
	// So we look for the pattern in CSV.
	
	// We need to process line by line or delimiters?
	// Assuming the stream comes in chunks.
	
	size_t startPos = 0;
	while (true)
	{
		// Find "99" (simplified for ASCII CSV logic)
		// Better approach: Split by some delimiter? 
		// If data is continuously streaming, we might rely on commas.
		
		// Let's assume the frame ends with newline or we just parse the sequence of numbers.
		// "99, val1, val2, ..., 99"
		
		// Find first 99
		// This is tricky with raw string stream without line breaks.
		// We'll search for "99," or ",99" or start of buffer?
		
		// Implementation based on Prompt: "99 (Header) ... 99 (Footer)"
		// Let's try to find a sequence starting with 99 and ending with 99.
		
		// For robustness, let's look for the *last* complete frame in the buffer
		// to display the most recent data, and discard old.
		
		// Check if we have enough data
		if (m_strBuffer.size() < 4) break; 
		
		// Find first 99
		// Note: atoi/atof is sensitive.
		
		// Simple approach: Tokenize everything by comma
		// Then scan tokens.
		
		// Copy buffer to process
		std::string temp = m_strBuffer;
		
		// If buffer gets too large, clear it to prevent memory issues
		if (m_strBuffer.size() > 4096) {
			// Keep last 1024 bytes
			m_strBuffer = m_strBuffer.substr(m_strBuffer.size() - 1024);
		}
		
		// We will implement a simplified parser here:
		// Convert commas to spaces
		for (char& c : temp) if (c == ',') c = ' ';
		
		std::stringstream ss(temp);
		std::vector<double> tokens;
		double val;
		while (ss >> val) {
			tokens.push_back(val);
		}
		
		// Scan tokens for 99 ... 99 pattern
		// We want the *latest* valid frame.
		int foundStart = -1;
		int foundEnd = -1;
		
		for (int i = 0; i < (int)tokens.size(); ++i)
		{
			// Check for Header 99
			// Note: strict equality check for double 99.0
			if (abs(tokens[i] - 99.0) < 0.001)
			{
				// Potential Header
				// But could also be a Footer for previous frame.
				// Or Footer for current frame.
				
				// If we have a start, this could be the end
				if (foundStart != -1)
				{
					// Valid frame found?
					// Minimum frame size: 99, 99 (empty?) or 99, data, 99.
					// Let's assume at least one data point? 
					// The prompt says "First... Last...".
					
					foundEnd = i;
					
					// Store this frame as candidate and keep searching for newer ones
					// Extract data between start and end
					std::vector<double> currentFrame;
					for (int k = foundStart + 1; k < foundEnd; ++k) {
						currentFrame.push_back(tokens[k]);
					}
					
					if (!currentFrame.empty()) {
						m_parsedData = currentFrame;
					}
					
					// Reset start to current (since 99 could be start of next?)
					// Or consume it?
					// Protocol "99, data, 99" usually implies 99 is exclusive.
					// Let's assume "99, data, 99" then next frame "99, data, 99".
					foundStart = i; 
				}
				else
				{
					foundStart = i;
				}
			}
		}
		
		break; // Done processing current snapshot
	}
}

double SensorManager::GetSensorValue(int index)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (index >= 0 && index < (int)m_parsedData.size()) {
		return m_parsedData[index];
	}
	return 0.0;
}

CString SensorManager::GetLastRawString()
{
    // Return formatted CSV of current data
    std::lock_guard<std::mutex> lock(m_mutex);
    CString str;
    for(size_t i=0; i<m_parsedData.size(); ++i) {
        CString s;
        s.Format(_T("%.2f "), m_parsedData[i]);
        str += s;
    }
    return str;
}

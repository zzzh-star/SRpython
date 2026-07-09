#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct BridgeTelemetry
{
	double timestamp = 0.0;
	double omegaPx = 0.0;
	double omegaPy = 0.0;
	double omegaPz = 0.0;
	double omegaFx = 0.0;
	double omegaFy = 0.0;
	double omegaFz = 0.0;
	int omegaEnc[7] = { 0 };
	double gripperFeedbackApplied = 0.0;
	bool motorEnabled = false;
	long motorTarget3 = 0;
	long motorTarget4 = 0;
	long motorTarget5 = 0;
	bool cppEmergencyStop = false;
	std::string cppWarning;
};

class PythonBridgeServer
{
public:
	PythonBridgeServer();
	~PythonBridgeServer();

	bool Start(unsigned short port = 8765);
	void Stop();

	void UpdateTelemetry(const BridgeTelemetry& telemetry);
	double NextFeedbackForControl(double limit, double smoothingAlpha, unsigned long timeoutMs);
	void ForceZeroFeedback(const char* warning = nullptr);

	bool ConsumeStartExperiment();
	bool ConsumeStopExperiment();
	bool ConsumeZeroOmega();
	bool ConsumeEmergencyStop();

	bool IsClientConnected() const;
	bool IsTimedOut(unsigned long timeoutMs) const;
	bool IsEmergencyStopLatched() const;
	std::string GetLastWarning() const;

private:
	void ServerThreadMain(unsigned short port);
	void CloseClient();
	void HandleLine(const std::string& line);
	void SendAck(const char* command, bool success, const char* message);
	void SendError(const char* source, const char* message);
	void SendTelemetry();

	static bool ExtractStringField(const std::string& json, const char* key, std::string& value);
	static bool ExtractDoubleField(const std::string& json, const char* key, double& value);
	static std::string EscapeJson(const std::string& value);
	static unsigned long NowMs();

	std::thread m_thread;
	std::atomic<bool> m_running;
	std::atomic<bool> m_clientConnected;

	mutable std::mutex m_stateMutex;
	BridgeTelemetry m_telemetry;
	double m_targetFeedback;
	double m_appliedFeedback;
	unsigned long m_lastClientMessageMs;
	unsigned long m_lastTelemetrySendMs;
	std::string m_warning;

	std::atomic<bool> m_startExperimentRequested;
	std::atomic<bool> m_stopExperimentRequested;
	std::atomic<bool> m_zeroOmegaRequested;
	std::atomic<bool> m_emergencyStopRequested;
	std::atomic<bool> m_emergencyStopLatched;

	uintptr_t m_listenSocket;
	uintptr_t m_clientSocket;
};

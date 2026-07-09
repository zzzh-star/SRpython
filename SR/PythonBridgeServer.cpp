#include "pch.h"
#include "PythonBridgeServer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <winsock.h>

#pragma comment(lib, "ws2_32.lib")

namespace
{
	const uintptr_t kInvalidSocketValue = static_cast<uintptr_t>(INVALID_SOCKET);
	const unsigned long kTelemetryIntervalMs = 20;

	SOCKET ToSocket(uintptr_t value)
	{
		return static_cast<SOCKET>(value);
	}
}

PythonBridgeServer::PythonBridgeServer()
	: m_running(false),
	m_clientConnected(false),
	m_targetFeedback(0.0),
	m_appliedFeedback(0.0),
	m_lastClientMessageMs(0),
	m_lastTelemetrySendMs(0),
	m_startExperimentRequested(false),
	m_stopExperimentRequested(false),
	m_zeroOmegaRequested(false),
	m_emergencyStopRequested(false),
	m_emergencyStopLatched(false),
	m_listenSocket(kInvalidSocketValue),
	m_clientSocket(kInvalidSocketValue)
{
}

PythonBridgeServer::~PythonBridgeServer()
{
	Stop();
}

bool PythonBridgeServer::Start(unsigned short port)
{
	if (m_running)
	{
		return true;
	}

	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
	{
		return false;
	}

	m_running = true;
	m_thread = std::thread(&PythonBridgeServer::ServerThreadMain, this, port);
	return true;
}

void PythonBridgeServer::Stop()
{
	if (!m_running && !m_thread.joinable())
	{
		return;
	}

	m_running = false;

	if (m_thread.joinable())
	{
		m_thread.join();
	}

	WSACleanup();
}

void PythonBridgeServer::UpdateTelemetry(const BridgeTelemetry& telemetry)
{
	std::lock_guard<std::mutex> lock(m_stateMutex);
	m_telemetry = telemetry;
	m_telemetry.gripperFeedbackApplied = m_appliedFeedback;
	m_telemetry.cppEmergencyStop = m_emergencyStopLatched;
	if (!m_warning.empty())
	{
		m_telemetry.cppWarning = m_warning;
	}
}

double PythonBridgeServer::NextFeedbackForControl(double limit, double smoothingAlpha, unsigned long timeoutMs)
{
	std::lock_guard<std::mutex> lock(m_stateMutex);

	const unsigned long now = NowMs();
	if (m_emergencyStopLatched || !m_clientConnected || now - m_lastClientMessageMs > timeoutMs)
	{
		m_targetFeedback = 0.0;
		if (m_clientConnected && now - m_lastClientMessageMs > timeoutMs)
		{
			m_warning = "python heartbeat timeout; feedback forced to zero";
		}
	}

	const double limitedTarget = (std::max)(-limit, (std::min)(limit, m_targetFeedback));
	const double alpha = (std::max)(0.0, (std::min)(1.0, smoothingAlpha));
	m_appliedFeedback += alpha * (limitedTarget - m_appliedFeedback);

	if (std::abs(m_appliedFeedback) < 0.000001)
	{
		m_appliedFeedback = 0.0;
	}

	return m_appliedFeedback;
}

void PythonBridgeServer::ForceZeroFeedback(const char* warning)
{
	std::lock_guard<std::mutex> lock(m_stateMutex);
	m_targetFeedback = 0.0;
	m_appliedFeedback = 0.0;
	if (warning)
	{
		m_warning = warning;
	}
}

bool PythonBridgeServer::ConsumeStartExperiment()
{
	return m_startExperimentRequested.exchange(false);
}

bool PythonBridgeServer::ConsumeStopExperiment()
{
	return m_stopExperimentRequested.exchange(false);
}

bool PythonBridgeServer::ConsumeZeroOmega()
{
	return m_zeroOmegaRequested.exchange(false);
}

bool PythonBridgeServer::ConsumeEmergencyStop()
{
	return m_emergencyStopRequested.exchange(false);
}

bool PythonBridgeServer::IsClientConnected() const
{
	return m_clientConnected;
}

bool PythonBridgeServer::IsTimedOut(unsigned long timeoutMs) const
{
	std::lock_guard<std::mutex> lock(m_stateMutex);
	return !m_clientConnected || NowMs() - m_lastClientMessageMs > timeoutMs;
}

bool PythonBridgeServer::IsEmergencyStopLatched() const
{
	return m_emergencyStopLatched;
}

std::string PythonBridgeServer::GetLastWarning() const
{
	std::lock_guard<std::mutex> lock(m_stateMutex);
	return m_warning;
}

void PythonBridgeServer::ServerThreadMain(unsigned short port)
{
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
	{
		m_running = false;
		return;
	}

	m_listenSocket = static_cast<uintptr_t>(listenSocket);

	u_long nonBlocking = 1;
	ioctlsocket(listenSocket, FIONBIO, &nonBlocking);

	BOOL reuseAddr = TRUE;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuseAddr), sizeof(reuseAddr));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);

	if (bind(listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
		listen(listenSocket, 1) == SOCKET_ERROR)
	{
		m_running = false;
		closesocket(listenSocket);
		m_listenSocket = kInvalidSocketValue;
		return;
	}

	std::string receiveBuffer;

	while (m_running)
	{
		if (!m_clientConnected)
		{
			SOCKET client = accept(listenSocket, nullptr, nullptr);
			if (client != INVALID_SOCKET)
			{
				u_long clientNonBlocking = 1;
				ioctlsocket(client, FIONBIO, &clientNonBlocking);
				m_clientSocket = static_cast<uintptr_t>(client);
				m_clientConnected = true;
				receiveBuffer.clear();
				{
					std::lock_guard<std::mutex> lock(m_stateMutex);
					m_lastClientMessageMs = NowMs();
					m_warning.clear();
				}
				SendAck("connect", true, "python bridge connected");
			}
		}
		else
		{
			SOCKET client = ToSocket(m_clientSocket);
			char buffer[1024];
			int received = recv(client, buffer, sizeof(buffer), 0);
			if (received > 0)
			{
				receiveBuffer.append(buffer, buffer + received);
				size_t pos = std::string::npos;
				while ((pos = receiveBuffer.find('\n')) != std::string::npos)
				{
					std::string line = receiveBuffer.substr(0, pos);
					receiveBuffer.erase(0, pos + 1);
					HandleLine(line);
				}
			}
			else if (received == 0)
			{
				CloseClient();
				ForceZeroFeedback("python disconnected; feedback forced to zero");
			}
			else
			{
				const int error = WSAGetLastError();
				if (error != WSAEWOULDBLOCK)
				{
					CloseClient();
					ForceZeroFeedback("python socket error; feedback forced to zero");
				}
			}

			SendTelemetry();
		}

		Sleep(2);
	}

	CloseClient();
	closesocket(listenSocket);
	m_listenSocket = kInvalidSocketValue;
}

void PythonBridgeServer::CloseClient()
{
	SOCKET client = ToSocket(m_clientSocket);
	if (client != INVALID_SOCKET)
	{
		shutdown(client, SD_BOTH);
		closesocket(client);
		m_clientSocket = kInvalidSocketValue;
	}
	m_clientConnected = false;
}

void PythonBridgeServer::HandleLine(const std::string& line)
{
	std::string type;
	if (!ExtractStringField(line, "type", type))
	{
		SendError("PythonBridgeServer", "missing message type");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		m_lastClientMessageMs = NowMs();
	}

	if (type == "heartbeat")
	{
		SendAck("heartbeat", true, "ok");
		return;
	}

	if (type == "set_omega_feedback")
	{
		if (m_emergencyStopLatched)
		{
			SendAck("set_omega_feedback", false, "emergency stop latched");
			return;
		}

		double feedback = 0.0;
		if (!ExtractDoubleField(line, "gripper_feedback", feedback))
		{
			SendError("PythonBridgeServer", "invalid feedback command");
			return;
		}

		std::lock_guard<std::mutex> lock(m_stateMutex);
		m_targetFeedback = feedback;
		m_warning.clear();
		SendAck("set_omega_feedback", true, "feedback updated");
		return;
	}

	if (type == "command")
	{
		std::string command;
		if (!ExtractStringField(line, "command", command))
		{
			SendError("PythonBridgeServer", "missing command");
			return;
		}

		if (command == "start_experiment")
		{
			m_startExperimentRequested = true;
		}
		else if (command == "stop_experiment")
		{
			m_stopExperimentRequested = true;
			ForceZeroFeedback("experiment stopped; feedback forced to zero");
		}
		else if (command == "zero_omega")
		{
			m_zeroOmegaRequested = true;
		}
		else if (command == "emergency_stop")
		{
			m_emergencyStopLatched = true;
			m_emergencyStopRequested = true;
			ForceZeroFeedback("emergency stop latched");
		}
		else
		{
			SendError("PythonBridgeServer", "unknown command");
			return;
		}

		SendAck(command.c_str(), true, "command accepted");
		return;
	}

	SendError("PythonBridgeServer", "unknown message type");
}

void PythonBridgeServer::SendAck(const char* command, bool success, const char* message)
{
	if (!m_clientConnected)
	{
		return;
	}

	std::ostringstream out;
	out << "{\"type\":\"ack\",\"command\":\"" << EscapeJson(command)
		<< "\",\"success\":" << (success ? "true" : "false")
		<< ",\"message\":\"" << EscapeJson(message) << "\"}\n";
	const std::string text = out.str();
	send(ToSocket(m_clientSocket), text.c_str(), static_cast<int>(text.size()), 0);
}

void PythonBridgeServer::SendError(const char* source, const char* message)
{
	if (!m_clientConnected)
	{
		return;
	}

	std::ostringstream out;
	out << "{\"type\":\"error\",\"source\":\"" << EscapeJson(source)
		<< "\",\"message\":\"" << EscapeJson(message) << "\"}\n";
	const std::string text = out.str();
	send(ToSocket(m_clientSocket), text.c_str(), static_cast<int>(text.size()), 0);
}

void PythonBridgeServer::SendTelemetry()
{
	const unsigned long now = NowMs();
	if (now - m_lastTelemetrySendMs < kTelemetryIntervalMs)
	{
		return;
	}
	m_lastTelemetrySendMs = now;

	BridgeTelemetry telemetry;
	{
		std::lock_guard<std::mutex> lock(m_stateMutex);
		telemetry = m_telemetry;
	}

	std::ostringstream out;
	out << std::fixed << std::setprecision(6);
	out << "{\"type\":\"telemetry\""
		<< ",\"timestamp\":" << telemetry.timestamp
		<< ",\"omega_px\":" << telemetry.omegaPx
		<< ",\"omega_py\":" << telemetry.omegaPy
		<< ",\"omega_pz\":" << telemetry.omegaPz
		<< ",\"omega_fx\":" << telemetry.omegaFx
		<< ",\"omega_fy\":" << telemetry.omegaFy
		<< ",\"omega_fz\":" << telemetry.omegaFz;

	for (int i = 0; i < 7; ++i)
	{
		out << ",\"omega_enc" << i << "\":" << telemetry.omegaEnc[i];
	}

	out << ",\"gripper_feedback_applied\":" << telemetry.gripperFeedbackApplied
		<< ",\"motor_enabled\":" << (telemetry.motorEnabled ? "true" : "false")
		<< ",\"motor_target_3\":" << telemetry.motorTarget3
		<< ",\"motor_target_4\":" << telemetry.motorTarget4
		<< ",\"motor_target_5\":" << telemetry.motorTarget5
		<< ",\"cpp_emergency_stop\":" << (telemetry.cppEmergencyStop ? "true" : "false")
		<< ",\"cpp_warning\":\"" << EscapeJson(telemetry.cppWarning) << "\"}\n";

	const std::string text = out.str();
	int sent = send(ToSocket(m_clientSocket), text.c_str(), static_cast<int>(text.size()), 0);
	if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
	{
		CloseClient();
		ForceZeroFeedback("telemetry send failed; feedback forced to zero");
	}
}

bool PythonBridgeServer::ExtractStringField(const std::string& json, const char* key, std::string& value)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos) return false;
	pos = json.find('"', pos + 1);
	if (pos == std::string::npos) return false;
	size_t end = json.find('"', pos + 1);
	if (end == std::string::npos) return false;
	value = json.substr(pos + 1, end - pos - 1);
	return true;
}

bool PythonBridgeServer::ExtractDoubleField(const std::string& json, const char* key, double& value)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + needle.size());
	if (pos == std::string::npos) return false;
	++pos;
	while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
	size_t end = pos;
	while (end < json.size())
	{
		char ch = json[end];
		if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E'))
		{
			break;
		}
		++end;
	}
	if (end == pos) return false;
	value = atof(json.substr(pos, end - pos).c_str());
	return true;
}

std::string PythonBridgeServer::EscapeJson(const std::string& value)
{
	std::string escaped;
	for (char ch : value)
	{
		if (ch == '"' || ch == '\\')
		{
			escaped.push_back('\\');
		}
		if (ch == '\r' || ch == '\n')
		{
			escaped.push_back(' ');
		}
		else
		{
			escaped.push_back(ch);
		}
	}
	return escaped;
}

unsigned long PythonBridgeServer::NowMs()
{
	return static_cast<unsigned long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

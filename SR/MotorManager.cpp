#include "pch.h"
#include "MotorManager.h"
#include <iostream>


//初始化
MotorManager::MotorManager()
	: m_KeyHandle(nullptr), m_SubKeyHandle(nullptr), m_ulErrorCode(0)
{
}


//析构函数
MotorManager::~MotorManager()
{
	Disconnect();
}


bool MotorManager::Connect()
{
	//1.打开主设备
	m_KeyHandle = VCS_OpenDevice(
		(char*)deviceName,				//const是只读的，这里要用(char*)，防止报错
		(char*)protocolStackName,
		(char*)interfaceName,
		(char*)portName,
		&m_ulErrorCode);
	if (!m_KeyHandle)
	{
		return false;
	}

	//2.打开子设备
	m_SubKeyHandle = VCS_OpenSubDevice(
		m_KeyHandle,
		(char*)deviceName,
		(char*)subProtocolStackName,
		&m_ulErrorCode
	);
	if (!m_SubKeyHandle)
	{
		// 如果子设备打开失败，应该关闭主设备
		VCS_CloseDevice(m_KeyHandle, &m_ulErrorCode);
		m_KeyHandle = nullptr;
		return false;
	}

	//3.设置协议栈参数
	if (!VCS_SetProtocolStackSettings(m_KeyHandle, BAUDRATE, TIMEOUT, &m_ulErrorCode))
	{
		return false;
	}
//这里没有清除错误
	if (!VCS_SetProtocolStackSettings(m_SubKeyHandle, BAUDRATE, TIMEOUT, &m_ulErrorCode))
	{
		return false;
	}
	return true;
}


HANDLE MotorManager::GetHandleForNode(WORD nodeId) const
{
	if (nodeId == 1) return m_KeyHandle;
	return m_SubKeyHandle;
}


void MotorManager::Disconnect()
{
	
	// 尝试关闭所有设备，忽略错误
	VCS_CloseAllDevices(&m_ulErrorCode);

	m_KeyHandle = nullptr;
	m_SubKeyHandle = nullptr;
}


bool MotorManager::EnableMotors()
{
	if (!IsConnected()) return false;
	// 清除故障并使能各个节点
	// 根据原程序逻辑：
	// Node 1 在 KeyHandle 上
	// Node 2, 3, 4, 5 在 SubKeyHandle 上
	//这里只启用了2, 3, 4, 5，没有启用1。

//在这里清除故障

	// Node 2-5 (按照原程序逻辑)
	// 如果需要 Node 1，可以在这里添加
	// HANDLE handle1 = GetHandleForNode(1);
	// SetEnableState(handle1, 1);
	// SetOperationMode(handle1, 1, -1);

	//使能
	WORD nodeIds[] = { 2, 3, 4, 5 };
	for (WORD nodeId : nodeIds)
	{
		HANDLE handle = GetHandleForNode(nodeId);						//获取句柄
		if (!ClearFault(handle, nodeId)) return false;					//清除故障信息
		if (!SetEnableState(handle, nodeId)) return false;				//设置使能
		if (!SetOperationMode(handle, nodeId, -1)) return false;		//设置操作模式
		Sleep(20);
	}
	return true;

}


bool MotorManager::DisableMotors()
{
	if (!IsConnected()) return false;

	// Node 1-5
	for (WORD nodeId = 1; nodeId <= 5; ++nodeId)
	{
		HANDLE handle = GetHandleForNode(nodeId);
		SetDisableState(handle, nodeId);
	}
	Disconnect();
	return true;
}


bool MotorManager::ConfigureMotorProfile(WORD nodeId, DWORD maxVelocity, DWORD acceleration, DWORD deceleration, DWORD maxFollowingError)
{
	HANDLE handle = GetHandleForNode(nodeId);
	if (!handle) return false;

	// 1. 激活 Profile Position Mode (Mode 1)
	if (!VCS_ActivateProfilePositionMode(handle, nodeId, &m_ulErrorCode)) return false;

	// 2. 设置最大速度
	if (!VCS_SetMaxProfileVelocity(handle, nodeId, maxVelocity, &m_ulErrorCode)) return false;

	// 3. 设置运动配置 (Velocity, Acc, Dec)，这里maxVelocity参数暂时设置为16000
	if (!VCS_SetPositionProfile(handle, nodeId, maxVelocity, acceleration, deceleration, &m_ulErrorCode)) return false;

	// 4. 设置最大跟随误差
	if (!VCS_SetMaxFollowingError(handle, nodeId, maxFollowingError, &m_ulErrorCode)) return false;

	return true;
}


bool MotorManager::MoveToPosition(WORD nodeId, long position, bool absolute, bool immediately)
{
	HANDLE handle = GetHandleForNode(nodeId);
	if (!handle) return false;

	if (!VCS_MoveToPosition(handle, nodeId, position, absolute, immediately, &m_ulErrorCode))
	{
		return false;
	}
	return true;
}



bool MotorManager::ClearFault(HANDLE handles,WORD nodeId)
{
	return VCS_ClearFault(handles, nodeId, &m_ulErrorCode);
}


bool MotorManager::SetEnableState(HANDLE handles, WORD nodeId)
{
	return VCS_SetEnableState(handles, nodeId, &m_ulErrorCode);
}


bool MotorManager::SetDisableState(HANDLE handles, WORD nodeId)
{
	return VCS_SetDisableState(handles, nodeId, &m_ulErrorCode);
}


bool MotorManager::SetOperationMode(HANDLE handles, WORD nodeId, __int8 mode)
{
	return VCS_SetOperationMode(handles, nodeId, mode, &m_ulErrorCode);
}
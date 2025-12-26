#pragma once
#include "Definitions.h"
class MotorManager
{
public:
	MotorManager();
	~MotorManager();

	//初始化并连接设备，设置通讯协议
	bool Connect();

	//关闭设备
	void Disconnect();

	// 使能所有电机
	bool EnableMotors();

	//失能所有电机
	bool DisableMotors();

	// 配置电机运动参数 (Profile Position Mode)
	// nodeId: 电机ID
	// maxVelocity: 最大速度
	// acceleration: 加速度
	// deceleration: 减速度
	// maxFollowingError: 最大跟随误差
	bool ConfigureMotorProfile(WORD nodeId, DWORD maxVelocity, DWORD acceleration, DWORD deceleration, DWORD maxFollowingError);

	// 移动电机到指定位置
	// nodeId: 电机ID
	// position: 目标位置 (单位: quad counts)
	// absolute: true=绝对位置, false=相对位置
	// immediately: true=立即执行
	bool MoveToPosition(WORD nodeId, long position, bool absolute = true, bool immediately = true);

	//获取错误代码
	DWORD GetLastErrorCode() const { return m_ulErrorCode; }

	//判断电机是否连接
	bool IsConnected() const { return(m_KeyHandle !=nullptr && m_SubKeyHandle != nullptr); }


private:
	HANDLE m_KeyHandle;										//设备句柄
	HANDLE m_SubKeyHandle;									//子设备句柄
	DWORD m_ulErrorCode;									//错误代码
	//DWORD用于存放正胜数，1.错误代码。2.状态标志



	//硬编码的配置参数
	const char* deviceName = "EPOS2";						//设备名称
	const char* protocolStackName = "MAXON SERIAL V2";		//通讯协议
	const char* interfaceName = "USB";						//物理接口
	const char* portName = "USB0";							//端口
	const char* subProtocolStackName = "CANopen";			//子设备通讯协议

	const DWORD BAUDRATE = 1000000;							//波特率
	const DWORD TIMEOUT = 500;								//超时时间500ms


	// 内部辅助函数：根据NodeID获取对应的Handle
	HANDLE GetHandleForNode(WORD nodeId) const;

	//清除故障
	bool ClearFault(HANDLE handles, WORD nodeId);

	//重写使能函数
	bool SetEnableState(HANDLE handles, WORD nodeId);

	//重写失能函数
	bool SetDisableState(HANDLE handles, WORD nodeId);

	//重写设置操作模式函数
	bool SetOperationMode(HANDLE handles, WORD nodeId, __int8 mode);

};


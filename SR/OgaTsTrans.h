#ifndef OGATSTRANS_H_INCLUDED
#define OGATSTRANS_H_INCLUDED


#define TSENS_NUM_MAX	5					// 接続されうるセンサの最大数
#define CAP_NUM_MAX		(TSENS_NUM_MAX * 2)	// それを構成するキャパシタの数

// キャパシタの詳細
typedef struct _EachCapDetail {
	WORD raw;
	WORD base;
	WORD span;
} EachCapDetail;

// 各センサデータ
typedef struct _TSensDat {
	WORD Cmain;		// 主検出
	WORD Cref;		// リファレンス
	WORD Cdiff;		// 差分、即ちこのセンサ出力(Cmain - Cref)
	EachCapDetail CmainDetail;
	EachCapDetail CrefDetail;
} TSensDat;

// 評価キットボードから収得されるデータ(1軸多チャンネル)
typedef struct _TSensDevInfo {
	TSensDat tSensor[TSENS_NUM_MAX];
	EachCapDetail capacitor[CAP_NUM_MAX];
} TSensDevInfo;

// 3軸センサ評価キット向け拡張
typedef struct _TSensDevInfo3DEx {
	short dx;
	short dy;
	short dz;
	unsigned char buttons;
} TSensDevInfo3DEx;

// 3軸センサ・ボタンマスク
#define TSENS3D_BUTTONS_MASK_A	0x01
#define TSENS3D_BUTTONS_MASK_B	0x02
#define TSENS3D_BUTTONS_MASK_L	TSENS3D_BUTTONS_MASK_A
#define TSENS3D_BUTTONS_MASK_R	TSENS3D_BUTTONS_MASK_B


// 関数プロトタイプ
// 返値
//	成功：TRUE
//	失敗：FALSE
#ifndef DLLAPI
#ifdef __cplusplus
#define DLLAPI extern "C" __declspec(dllimport)
#else
#define DLLAPI extern __declspec(dllimport)
#endif
#endif

// low-level API
DLLAPI DWORD openTSensDevPID( HANDLE *tSensDevHandle, USHORT usbPid );
DLLAPI DWORD closeTSensDev( HANDLE tSensDevHandle ); // デバイスをクローズ
DLLAPI DWORD getTSensDevInfo( HANDLE tSensDevHandle, TSensDevInfo *tSensDevInfo );
DLLAPI DWORD getTSensDevInfo3D( HANDLE tSensDevHandle, TSensDevInfo *tSensDevInfo, TSensDevInfo3DEx *tSensDevInfo3D );
// high-level API
DLLAPI DWORD openTSensDev( HANDLE *tSensDevHandle );
DLLAPI DWORD openTSensDev1D( HANDLE *tSensDevHandle ); // 1軸キットをオープン
DLLAPI DWORD openTSensDev3D( HANDLE *tSensDevHandle ); // 3軸キットをオープン
DLLAPI DWORD getTSensDat1D( HANDLE tSensDevHandle, 
	int *Sensor0, int *Sensor1, int *Sensor2 ); // 1軸キットからデータ収得(3個のみ)
DLLAPI DWORD getTSensDat3D( HANDLE tSensDevHandle, 
	int *X, int *Y, int *Z, unsigned char *buttons ); // 3軸キットからデータ収得


#endif

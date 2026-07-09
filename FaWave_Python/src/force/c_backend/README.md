# C Backend (已废弃/历史备份)

此目录用于存放和编译从下位机 `Keil C` 移植的原始解耦算法。

**注意：当前软件主流程已不再依赖此 C 语言后端 DLL，已全面切换为单纯使用 `Python解耦`。此目录仅作为历史备份和扩展参考保留。主程序运行无需编译或加载 `force_decoder.dll`。**

## 文件说明 (历史)

- `app_ForceOut.c` / `app_ForceOut.h`：下位机原始三维力算法源码文件。
- `force_wrapper.c`：C 接口包装层，暴露出用于 Python `ctypes` 调用的接口。
- `build_force_dll.bat`：Windows 编译脚本。

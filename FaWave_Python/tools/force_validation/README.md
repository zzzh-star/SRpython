# 三维力解耦准确性验证

本目录提供了用于验证 Python 端三维力解耦算法准确性的测试工具。

## 背景与目的
1. **C 算法是参考标准**: 解耦逻辑源自 Firmware 文件夹中的 `app_ForceOut.c` 和 `app_ForceOut.h`。C DLL 后端最接近真实的下位机硬件表现。
2. **Python 版本验证**: Python 版本（位于 `force_decoder.py`）通过同输入序列与 C 输出逐点比较来进行验证。
3. **比较字段**: 包括 Fx/Fy/Fz 和 d1~d4，以确保 Python 的算法准确映射单精度的 C 语言解耦结果。

## 误差指标与阈值
- **D1~D4**: `abs error < 1e-5 V`
- **Fx/Fy/Fz**: `abs error < 1e-3 N`

## 测试流程
1. **生成测试向量**: 运行 `python3 generate_test_vectors.py` 生成一系列基础 CSV 数据流（例如阶跃响应、噪声基线、长期漂移）。
2. **生成 C 参考数据**: 编译并运行 C 测试外壳 `force_ref_main` 以针对测试 CSV 输出参考 C 解耦数据（需在 Windows 下编译，或者使用交叉编译环境运行测试框架获取）。
3. **生成 Python 移植数据**: 运行 `python3 run_python_decoder.py` 生成 Python 解耦输出结果。
4. **验证误差报告**: 运行 `python3 compare_force_decoder.py`。该脚本会自动比对两组 CSV，输出最大的绝对误差值，并验证是否在阈值内。

## 配置与降级
- 软件推荐的 `backend` 默认使用 `c_dll`。
- 如果 C DLL 不存在或加载失败，系统会自动降级为 Python 移植，但会在界面状态标明 "未验证"，除非配置声明验证已通过。
- UI 交互状态下，可在“操作控制”面板手动一键切换。

import argparse
import pandas as pd
import numpy as np

def print_stats(name, df, columns):
    print(f"\n--- {name} 统计 ---")
    for col in columns:
        if col in df.columns:
            data = df[col].dropna()
            if len(data) == 0:
                print(f"{col}: 无有效数据")
                continue
            mean = data.mean()
            min_val = data.min()
            max_val = data.max()
            span = max_val - min_val
            std = data.std()
            print(f"{col:<10} | 均值: {mean:8.4f} | 最小值: {min_val:8.4f} | 最大值: {max_val:8.4f} | 范围: {span:8.4f} | 标准差: {std:8.4f}")
        else:
            print(f"{col}: 未在文件中找到")

def main():
    parser = argparse.ArgumentParser(description="检查保存后的数据文件，判断单位及解耦可信性")
    parser.add_argument("--file", required=True, help="要检查的 CSV 或 XLSX 文件路径")
    args = parser.parse_args()

    print(f"正在读取文件: {args.file}")
    try:
        if args.file.lower().endswith(".csv"):
            df = pd.read_csv(args.file)
        elif args.file.lower().endswith(".xlsx"):
            df = pd.read_excel(args.file)
        else:
            print("不支持的文件格式，仅支持 .csv 或 .xlsx")
            return
    except Exception as e:
        print(f"读取文件失败: {e}")
        return

    print(f"成功读取 {len(df)} 行数据。")

    # Check Meta Data fields
    meta_cols = ["InputUnit", "InputScaleToV", "Algorithm", "DecoderStatus", "AcquisitionMode", "TaskMode", "AlarmEvent"]
    print("\n--- 元数据配置 ---")
    for col in meta_cols:
        if col in df.columns:
            val = df[col].iloc[-1] if len(df) > 0 else "未知"
            print(f"{col}: {val}")
        else:
            print(f"{col}: 未在文件中找到")

    # Print statistics for the different sets
    ch_cols = ["CH1_V", "CH2_V", "CH3_V", "CH4_V"]
    print_stats("原始电压通道 (CH1_V ~ CH4_V)", df, ch_cols)

    d_cols = ["D1_V", "D2_V", "D3_V", "D4_V"]
    print_stats("差分电压 (D1_V ~ D4_V)", df, d_cols)

    f_cols = ["Fx_N", "Fy_N", "Fz_N"]
    print_stats("三维力输出 (Fx_N ~ Fz_N)", df, f_cols)

    # Intelligence Warning Checks
    print("\n--- 分析建议 ---")

    if all(c in df.columns for c in ch_cols):
        # Sample an arbitrary channel to get the scale
        sample_mean = df["CH1_V"].mean()

        if -10 < sample_mean < 10:
            print("ℹ️ 通道数据看起来像 V 级电压。")
        elif sample_mean > 100 or sample_mean < -100:
            print("⚠️ 通道数据可能是 mV 级，需要确认 input_scale_to_v。")

    if "InputUnit" in df.columns and "InputScaleToV" in df.columns:
        unit = df["InputUnit"].iloc[-1]
        scale = df["InputScaleToV"].iloc[-1]
        if unit == "mV" or scale == 0.001:
            print("❌ 当前 FaWave 真实设备已确认返回单位为 V，默认不应使用 0.001，除非接入的是其他 mV 输出设备。")

if __name__ == "__main__":
    main()

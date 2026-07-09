import sys
import os
import time
import json
from pathlib import Path

# Add src to the sys path so we can load the modules directly
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.communication.mock_client import MockClient
from src.protocol.fawave_protocol import FaWaveProtocol

def main():
    print("--- 三维力解耦 Mock 仿真测试 ---")
    config_path = os.path.join(os.path.dirname(__file__), '..', 'config', 'default_config.json')

    with open(config_path, 'r', encoding='utf-8') as f:
        config = json.load(f)

    # Setup Mock client
    client = MockClient(config)
    client.connect()

    protocol = FaWaveProtocol(config)

    # Setup decoder logic just like the AcquisitionWorker does
    force_cfg = config.get("force_decoder", {})

    from src.force.force_decoder import ForceDecoder
    decoder = ForceDecoder(config)
    decoder_backend_str = "Python解耦"

    input_unit = force_cfg.get("input_unit", "V")
    input_scale_to_v = force_cfg.get("input_scale_to_v", 1.0)

    print(f"解耦算法: {decoder_backend_str}")
    print(f"输入单位: {input_unit} | 换算系数: {input_scale_to_v}")

    total_time_ms = 12000  # 12 seconds
    interval_ms = 20
    start_time = time.time()

    last_print_sec = -1

    print("\n--- 开始仿真测试 (持续 12 秒) ---")

    for i in range(total_time_ms // interval_ms):
        t_ms = i * interval_ms
        t_sec = t_ms // 1000

        # Advance the mock client time artificially if needed or rely on its internal clock
        raw_frame = client.receive(35)
        data = protocol.parse_response_frame(raw_frame)

        ch_v = [
            data.get("ch1", 0.0) * input_scale_to_v,
            data.get("ch2", 0.0) * input_scale_to_v,
            data.get("ch3", 0.0) * input_scale_to_v,
            data.get("ch4", 0.0) * input_scale_to_v
        ]

        if not getattr(decoder, 'initialized', False):
            decoder.initialize(ch_v)

        res = decoder.update(ch_v, t_ms)

        if t_sec > last_print_sec:
            last_print_sec = t_sec

            status = res.get("status", "未知")
            fx, fy, fz = res.get("fx", 0), res.get("fy", 0), res.get("fz", 0)
            in_deadzone = res.get("in_deadzone", False)
            d = res.get("d", [0,0,0,0])
            baseline = res.get("baseline", [0,0,0,0])

            print(f"[{t_sec}s | {t_ms}ms] 状态: {status}")
            print(f"  CH(V): [{ch_v[0]:.4f}, {ch_v[1]:.4f}, {ch_v[2]:.4f}, {ch_v[3]:.4f}]")
            print(f"  D(V) : [{d[0]:.4f}, {d[1]:.4f}, {d[2]:.4f}, {d[3]:.4f}]")
            if baseline[0] != 0.0:
                print(f"  Base : [{baseline[0]:.4f}, {baseline[1]:.4f}, {baseline[2]:.4f}, {baseline[3]:.4f}]")
            print(f"  Force: Fx={fx:.4f}, Fy={fy:.4f}, Fz={fz:.4f} (死区内={in_deadzone})")
            print("-" * 50)

        # Optional: Actually sleep to simulate real-time processing or let it run hot
        time.sleep(interval_ms / 1000.0)

    client.disconnect()

    print("\n--- 测试总结 ---")

    # Assess final state
    if status == "基线建立中":
        print("测试结果：❌ 需要检查")
        print("原因：运行 12 秒后仍处于基线建立中。")
    elif fx == 0.0 and fy == 0.0 and fz == 0.0 and not in_deadzone:
        print("测试结果：❌ 需要检查")
        print("原因：已启用解耦，但 Fx/Fy/Fz 长期为 0，可能处于死区但未正确标记，或输入幅值过小。")
    elif status == "死区内":
         print("测试结果：⚠️ 警告")
         print("原因：已启用解耦，但最终仍处于死区内。请检查 mock 配置中 dynamic_amplitude_v 是否足够大越过死区阈值。")
    else:
        print("测试结果：✅ 通过")
        print("原因：基线建立完成，三维力输出已响应动态输入。")

if __name__ == "__main__":
    main()

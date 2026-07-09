import argparse
import signal
import sys
import time
from pathlib import Path

from src.config_manager import load_config
from src.cpp_bridge_client import CppBridgeClient
from src.experiment_logger import ExperimentLogger
from src.force_processor import ForceProcessor
from src.force_sensor import create_force_sensor
from src.grasp_state_machine import GraspStateMachine
from src.omega_feedback_policy import OmegaFeedbackPolicy
from src.sma_controller import SMAController


APP_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = str(APP_DIR / "config" / "hardware_config.json")


def run_cli(config_path: str):
    config = load_config(config_path)
    bridge_cfg = config["cpp_bridge"]
    force_cfg = config["force_sensor"]
    state_cfg = config["grasp_state"]
    feedback_cfg = config["omega_feedback"]

    bridge = CppBridgeClient(
        bridge_cfg["host"],
        int(bridge_cfg["port"]),
        heartbeat_interval_ms=int(bridge_cfg["heartbeat_interval_ms"]),
        timeout_ms=int(bridge_cfg["timeout_ms"]),
    )
    sensor = create_force_sensor(force_cfg)
    processor = ForceProcessor(force_cfg)
    state_machine = GraspStateMachine(state_cfg)
    feedback_policy = OmegaFeedbackPolicy(feedback_cfg, state_cfg)
    sma = SMAController(config["sma"])
    logger = ExperimentLogger(config["logger"])

    running = True

    def stop(_signum=None, _frame=None):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("Python main-control simulation starting.")
    print(f"Connecting C++ bridge at {bridge_cfg['host']}:{bridge_cfg['port']} ...")

    bridge.connect()
    sensor.start()
    sma.start()
    logger.start()

    send_interval = 1.0 / float(feedback_cfg.get("send_rate_hz", 50))
    next_send = 0.0
    sample_index = 0
    last_state = None

    try:
        bridge.send_command("start_experiment")

        while running:
            loop_start = time.monotonic()
            raw_force = sensor.read()
            force = processor.update(raw_force)
            state, changed = state_machine.update(force["fn"], loop_start)
            feedback = feedback_policy.compute(force["fn"], state)

            if changed:
                sma.on_grasp_state_changed(state)
                print(f"state -> {state.value}")

            if loop_start >= next_send:
                bridge.send_feedback(feedback)
                next_send = loop_start + send_interval

            telemetry = bridge.latest_telemetry()
            logger.write_row({
                "timestamp": time.time(),
                "sample_index": sample_index,
                "fx": force["fx"],
                "fy": force["fy"],
                "fz": force["fz"],
                "fn": force["fn"],
                "ft": force["ft"],
                "grasp_state": state.value,
                "gripper_feedback_target": feedback,
                "gripper_feedback_applied": telemetry.get("gripper_feedback_applied", 0.0),
                "omega_px": telemetry.get("omega_px", 0.0),
                "omega_py": telemetry.get("omega_py", 0.0),
                "omega_pz": telemetry.get("omega_pz", 0.0),
                "cpp_warning": telemetry.get("cpp_warning", ""),
                "sma_command": sma.last_command,
                "sma_temperature": sma.last_temperature,
            })

            if state != last_state or sample_index % 25 == 0:
                print(
                    f"Fn={force['fn']:.4f} Ft={force['ft']:.4f} "
                    f"state={state.value} feedback={feedback:.3f} "
                    f"applied={telemetry.get('gripper_feedback_applied', 0.0):.3f}"
                )
                last_state = state

            sample_index += 1
            elapsed = time.monotonic() - loop_start
            time.sleep(max(0.0, min(0.02, send_interval - elapsed)))

    finally:
        print("Stopping Python main-control simulation.")
        try:
            bridge.send_feedback(0.0)
            bridge.send_command("stop_experiment")
        except Exception:
            pass
        sma.close()
        sensor.stop()
        logger.stop()
        bridge.close()


def main():
    parser = argparse.ArgumentParser(description="Python main-control simulation for SR/Omega bridge.")
    parser.add_argument(
        "--config",
        default=DEFAULT_CONFIG_PATH,
        help="Path to config JSON. Default opens hardware_config.json.",
    )
    parser.add_argument(
        "--ui",
        action="store_true",
        default=True,
        help="Start the PySide6 UI. This is enabled by default.",
    )
    parser.add_argument(
        "--cli",
        action="store_false",
        dest="ui",
        help="Run CLI loop instead of UI.",
    )
    args = parser.parse_args()
    if args.ui:
        from src.ui_main import run_ui
        run_ui(args.config)
    else:
        run_cli(args.config)


if __name__ == "__main__":
    sys.exit(main())

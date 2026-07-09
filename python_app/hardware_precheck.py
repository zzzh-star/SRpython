import importlib
import json
import sys
from pathlib import Path

from src.force_sensor import parse_force_frame
from src.grasp_state_machine import GraspState
from src.serial_tools import list_serial_ports
from src.sma_controller import SMAController


def check_import(name: str):
    module = importlib.import_module(name)
    version = getattr(module, "__version__", getattr(module, "VERSION", ""))
    print(f"[OK] import {name} {version}")


def warn_configured_port(port_name: str, ports: list[dict], label: str):
    match = next((port for port in ports if port.get("device", "").upper() == port_name.upper()), None)
    if not match:
        print(f"[WARN] {label} configured port {port_name} is not currently listed by Windows")
        return
    desc = str(match.get("description", ""))
    hwid = str(match.get("hwid", ""))
    print(f"[OK] {label} configured port found:", match)
    if "蓝牙" in desc or "Bluetooth" in desc or "BTHENUM" in hwid:
        print(f"[WARN] {label} port {port_name} looks like a Bluetooth virtual COM port; verify this is really the hardware device")


def main():
    print("[INFO] Python:", sys.executable)
    print("[INFO] Version:", sys.version)

    for name in ["PySide6", "pyqtgraph", "serial"]:
        check_import(name)

    ports = list_serial_ports()
    print("[INFO] serial ports:")
    if ports:
        for port in ports:
            print("  ", port)
    else:
        print("   none")

    config_path = Path("python_app/config/hardware_config.json")
    config = json.loads(config_path.read_text(encoding="utf-8"))
    print("[OK] config:", config_path)
    print("[INFO] force sensor:", config["force_sensor"]["port"], config["force_sensor"]["baud"], "simulation=", config["force_sensor"]["simulation"])
    print("[INFO] sma:", config["sma"]["port"], config["sma"]["baud"], "simulation=", config["sma"]["simulation"])
    warn_configured_port(config["force_sensor"]["port"], ports, "force sensor")
    warn_configured_port(config["sma"]["port"], ports, "SMA")

    samples = [
        "1.234,-0.560,7.890",
        "99,1.234,-0.560,7.890,99",
        "Fz:12.34N,Fx:-0.56N,Fy:7.89N",
    ]
    for sample in samples:
        parsed = parse_force_frame(sample)
        if not parsed:
            raise RuntimeError(f"force parser failed: {sample}")
    print("[OK] force parser")

    sim_sma = dict(config["sma"])
    sim_sma["simulation"] = True
    sim_sma["cooling_time_ms"] = 0
    sma = SMAController(sim_sma)
    sma.start()
    sma.on_grasp_state_changed(GraspState.CONTACT)
    print("[OK] SMA command sample:", sma.last_command)
    sma.close()
    print("[OK] precheck complete")


if __name__ == "__main__":
    main()

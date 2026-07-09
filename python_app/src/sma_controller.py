import time

from .grasp_state_machine import GraspState

try:
    import serial
except ImportError:
    serial = None


class SMAController:
    def __init__(self, config: dict):
        self.config = config
        self.simulation = bool(config.get("simulation", True))
        self.port = str(config.get("port", "COM7"))
        self.baud = int(config.get("baud", 115200))
        self.timeout_s = float(config.get("timeout_ms", 100)) / 1000.0
        self.channel = str(config.get("channel", "L")).upper()
        self.cooling_time_s = float(config.get("cooling_time_ms", 1000)) / 1000.0
        self.over_repeat_s = float(config.get("over_repeat_interval_ms", 1500)) / 1000.0
        self.last_command = ""
        self.last_temperature = 25.0
        self.last_response = ""
        self.recent_responses = []
        self._last_fire_time = 0.0
        self._last_over_time = 0.0
        self._serial = None

    def start(self):
        if self.simulation:
            print("[SMA simulation] enabled")
            return
        if serial is None:
            raise RuntimeError("pyserial is required for real SMA serial mode")
        self._serial = serial.Serial(self.port, self.baud, timeout=self.timeout_s, write_timeout=self.timeout_s)
        time.sleep(2.0)
        self._serial.reset_input_buffer()
        self.set_alarm_temp(float(self.config.get("alarm_temp", 43.0)))

    def close(self):
        try:
            self.stop()
        finally:
            if self._serial:
                self._serial.close()
                self._serial = None

    def on_grasp_state_changed(self, state: GraspState):
        now = time.monotonic()
        if now - self._last_fire_time < self.cooling_time_s:
            return

        if state == GraspState.NONE:
            self.stop()
        elif state == GraspState.CONTACT:
            self._send_hold(float(self.config.get("temp_contact", 32.0)))
        elif state == GraspState.STABLE:
            self._send_hold(float(self.config.get("temp_stable", 34.0)))
        elif state == GraspState.OVER:
            self._send_hold(float(self.config.get("temp_over", 37.0)))
            self._last_over_time = now

    def update_over_repeat(self, state: GraspState):
        now = time.monotonic()
        if state == GraspState.OVER and now - self._last_over_time >= self.over_repeat_s:
            self._send_hold(float(self.config.get("temp_over", 37.0)))
            self._last_over_time = now

    def stop(self):
        self._send("@S\n")

    def emergency_stop(self):
        self._send("x")

    def set_alarm_temp(self, alarm_temp: float):
        alarm_temp = max(30.0, min(43.0, alarm_temp))
        self._send(f"@A{alarm_temp:.0f}\n")

    def read_temperature(self) -> float | None:
        response = self._send("@T\n", read_response=True, expected_prefix="TEMP:")
        if response.startswith("TEMP:"):
            value = response.split(":", 1)[1].strip()
            if value.lower() == "nan":
                return None
            try:
                self.last_temperature = float(value)
                return self.last_temperature
            except ValueError:
                return None
        return None

    def _send_hold(self, target_temp: float):
        target_temp = min(float(self.config.get("alarm_temp", 43.0)), target_temp)
        self._send(f"@H{self.channel}{target_temp:.0f}\n")

    def _send(self, command: str, read_response: bool = False, expected_prefix: str = "") -> str:
        self.last_command = command.strip()
        self._last_fire_time = time.monotonic()
        if self.simulation:
            print(f"[SMA simulation] {self.last_command}")
            if read_response and self.last_command == "@T":
                return f"TEMP:{self.last_temperature:.2f}"
            return ""
        if self._serial is None:
            raise RuntimeError("SMA serial port is not open")
        self._serial.write(command.encode("ascii"))
        self._serial.flush()
        if read_response:
            self.last_response = self._read_response(expected_prefix)
            return self.last_response
        self._drain_responses()
        return ""

    def _read_response(self, expected_prefix: str = "") -> str:
        deadline = time.monotonic() + max(self.timeout_s, 0.1)
        last_line = ""
        while time.monotonic() < deadline:
            line = self._serial.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            last_line = line
            self._remember_response(line)
            if not expected_prefix or line.startswith(expected_prefix):
                return line
        return last_line

    def _drain_responses(self):
        deadline = time.monotonic() + min(max(self.timeout_s, 0.02), 0.2)
        while time.monotonic() < deadline:
            waiting = getattr(self._serial, "in_waiting", 0)
            if waiting <= 0:
                break
            line = self._serial.readline().decode("utf-8", errors="replace").strip()
            if line:
                self.last_response = line
                self._remember_response(line)

    def _remember_response(self, line: str):
        self.last_response = line
        self.recent_responses.append(line)
        self.recent_responses = self.recent_responses[-20:]

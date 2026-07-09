import math
import random
import re
import time

try:
    import serial
except ImportError:
    serial = None


def create_force_sensor(config: dict):
    if bool(config.get("simulation", True)):
        return SimulatedForceSensor(config)
    return SerialForceSensor(config)


def parse_force_frame(raw: str) -> dict | None:
    text = raw.strip()
    if not text:
        return None

    labeled = {}
    for label, value in re.findall(r"\b(FX|FY|FZ|Fx|Fy|Fz|fx|fy|fz)\b\s*[:=]?\s*([-+]?\d+(?:\.\d+)?)", text):
        labeled[label.lower()] = float(value)
    if {"fx", "fy", "fz"}.issubset(labeled):
        return {"fx": labeled["fx"], "fy": labeled["fy"], "fz": labeled["fz"]}

    normalized = text.replace("\t", ",").replace(";", ",").replace(" ", ",")
    parts = [part for part in normalized.split(",") if part]
    numbers = []
    for part in parts:
        try:
            numbers.append(float(part))
        except ValueError:
            continue

    if len(numbers) >= 5 and numbers[0] == 99.0 and numbers[4] == 99.0:
        return {"fx": numbers[1], "fy": numbers[2], "fz": numbers[3]}
    if len(numbers) >= 3:
        return {"fx": numbers[0], "fy": numbers[1], "fz": numbers[2]}
    return None


class SerialForceSensor:
    def __init__(self, config: dict):
        self.config = config
        self.port = str(config.get("port", "COM5"))
        self.baud = int(config.get("baud", 115200))
        self.timeout_s = float(config.get("timeout_ms", 20)) / 1000.0
        self._serial = None
        self._latest = {"fx": 0.0, "fy": 0.0, "fz": 0.0}
        self._buffer = ""
        self.last_raw_frame = ""
        self.last_parse_ok = False
        self.last_parse_error = ""

    def start(self):
        if serial is None:
            raise RuntimeError("pyserial is required for real force sensor serial mode")
        self._serial = serial.Serial(self.port, self.baud, timeout=self.timeout_s)
        self._serial.reset_input_buffer()

    def stop(self):
        if self._serial:
            self._serial.close()
            self._serial = None

    def read(self) -> dict:
        if self._serial is None:
            return dict(self._latest)

        chunk = self._serial.read(256)
        if chunk:
            self._buffer += chunk.decode("utf-8", errors="replace").replace("\r", "\n")
            self._consume_buffer()
        return dict(self._latest)

    def _consume_buffer(self):
        while "\n" in self._buffer:
            line, self._buffer = self._buffer.split("\n", 1)
            parsed = parse_force_frame(line)
            if parsed:
                self._latest = parsed
                self.last_raw_frame = line
                self.last_parse_ok = True
                self.last_parse_error = ""
            elif line.strip():
                self.last_raw_frame = line
                self.last_parse_ok = False
                self.last_parse_error = "unrecognized newline frame"

        start = self._buffer.find("99,")
        if start >= 0:
            if start > 0:
                self._buffer = self._buffer[start:]
            end = self._buffer.find(",99", 3)
            if end >= 0:
                frame = self._buffer[:end + 3]
                parsed = parse_force_frame(frame)
                if parsed:
                    self._latest = parsed
                    self.last_raw_frame = frame
                    self.last_parse_ok = True
                    self.last_parse_error = ""
                self._buffer = self._buffer[end + 3:]

        if all(label in self._buffer.lower() for label in ["fx", "fy", "fz"]):
            end = self._buffer.find("N", max(self._buffer.lower().find("fx"), self._buffer.lower().find("fy"), self._buffer.lower().find("fz")))
            if end >= 0:
                frame = self._buffer[:end + 1]
                parsed = parse_force_frame(frame)
                if parsed:
                    self._latest = parsed
                    self.last_raw_frame = frame
                    self.last_parse_ok = True
                    self.last_parse_error = ""
                self._buffer = self._buffer[end + 1:]

        if len(self._buffer) > 2048:
            self._buffer = self._buffer[-1024:]


class SimulatedForceSensor:
    def __init__(self, config: dict):
        self.config = config
        self.cycle_s = float(config.get("sim_cycle_s", 12.0))
        self.noise_n = float(config.get("sim_noise_n", 0.0005))
        self._start_time = 0.0
        self._running = False
        self.last_raw_frame = "simulation"
        self.last_parse_ok = True
        self.last_parse_error = ""

    def start(self):
        self._start_time = time.monotonic()
        self._running = True

    def stop(self):
        self._running = False

    def read(self) -> dict:
        if not self._running:
            return {"fx": 0.0, "fy": 0.0, "fz": 0.0}

        t = (time.monotonic() - self._start_time) % self.cycle_s
        p = t / self.cycle_s

        if p < 0.20:
            fn = 0.001
        elif p < 0.40:
            fn = 0.004 + (p - 0.20) / 0.20 * 0.006
        elif p < 0.70:
            fn = 0.014 + 0.003 * math.sin((p - 0.40) / 0.30 * math.pi)
        elif p < 0.86:
            fn = 0.023 + 0.004 * math.sin((p - 0.70) / 0.16 * math.pi)
        else:
            fn = max(0.0, 0.020 * (1.0 - (p - 0.86) / 0.14))

        noise = random.uniform(-self.noise_n, self.noise_n)
        fx = 0.25 * fn * math.sin(2.0 * math.pi * p) + random.uniform(-self.noise_n, self.noise_n)
        fy = 0.18 * fn * math.cos(2.0 * math.pi * p) + random.uniform(-self.noise_n, self.noise_n)
        fz = fn + noise
        return {"fx": fx, "fy": fy, "fz": fz}

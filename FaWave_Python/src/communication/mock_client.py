import struct
import math
import time
import numpy as np
from .base_client import BaseClient

class MockClient(BaseClient):
    def __init__(self, config):
        self.config = config
        self._connected = False
        self.start_time = 0
        self.frame_length = config.get("frame_length", 35)

        protocol_config = config.get("protocol", {})
        self.header_hex = protocol_config.get("header_hex", "5AA5")
        self.float_endian = protocol_config.get("float_endian", "<")
        self.data_offset = protocol_config.get("data_offset", 5)
        self.trailer_offset = protocol_config.get("trailer_offset", 21)

        self.header_bytes = bytes.fromhex(self.header_hex)

        self.mock_cfg = config.get("mock", {})
        self.force_demo_enabled = self.mock_cfg.get("force_demo_enabled", True)
        self.baseline_duration_s = self.mock_cfg.get("baseline_duration_s", 6.0)
        self.baseline_noise_v = self.mock_cfg.get("baseline_noise_v", 0.001)
        self.baseline_v = self.mock_cfg.get("baseline_v", [-1.844, -1.842, -1.845, -1.846])
        self.dynamic_amplitude_v = self.mock_cfg.get("dynamic_amplitude_v", [0.080, 0.060, 0.050, 0.040])
        self.safety_alarm_demo_enabled = self.mock_cfg.get("safety_alarm_demo_enabled", True)
        self.alarm_demo_start_s = self.mock_cfg.get("alarm_demo_start_s", self.baseline_duration_s + 2.0)
        self.alarm_demo_period_s = self.mock_cfg.get("alarm_demo_period_s", 7.0)

        matrix_cfg = config.get("force_decoder", {}).get("matrix", [
            [39.2375, 29.8622, 24.3702, 32.1236],
            [17.6342, -0.177, -20.266, 0.3688],
            [-7.9958, 19.9397, -6.9973, -17.1562],
        ])
        self.force_matrix = np.asarray(matrix_cfg, dtype=float)
        try:
            # Minimum-norm voltage delta for a target [Fz, Fx, Fy] force vector.
            self.force_to_voltage = self.force_matrix.T @ np.linalg.inv(self.force_matrix @ self.force_matrix.T)
        except Exception:
            self.force_to_voltage = None

    def connect(self):
        self._connected = True
        self.start_time = time.time()

    def disconnect(self):
        self._connected = False

    def send(self, data: bytes):
        if not self._connected:
            raise ConnectionError("Mock Client is not connected.")
        # Mock client just ignores the request data
        pass

    def receive(self, length: int) -> bytes:
        if not self._connected:
            raise ConnectionError("Mock Client is not connected.")

        import random
        t = time.time() - self.start_time

        if self.force_demo_enabled:
            values = []
            for i in range(4):
                noise = (random.random() - 0.5) * 2 * self.baseline_noise_v
                if t <= self.baseline_duration_s:
                    values.append(self.baseline_v[i] + noise)
                else:
                    if i == 0:
                        delta = self.dynamic_amplitude_v[i] * math.sin(2 * math.pi * 0.8 * t)
                    elif i == 1:
                        delta = self.dynamic_amplitude_v[i] * math.cos(2 * math.pi * 0.6 * t)
                    elif i == 2:
                        delta = self.dynamic_amplitude_v[i] * math.sin(2 * math.pi * 0.5 * t + 0.7)
                    else:
                        delta = self.dynamic_amplitude_v[i] * math.cos(2 * math.pi * 0.4 * t + 1.2)
                    values.append(self.baseline_v[i] + noise + delta)

            if self.safety_alarm_demo_enabled and t > self.alarm_demo_start_s:
                alarm_phase = (t - self.alarm_demo_start_s) % self.alarm_demo_period_s
                target_force = None

                if 0.35 <= alarm_phase < 2.0:
                    # Traction overload and shear puncture: sustained high Fz.
                    target_force = (13.0, 0.0, 0.0)
                elif 2.0 <= alarm_phase < 2.18:
                    # Traction tear/slip: a short sharp fall after the high-force plateau.
                    target_force = (-70.0, 0.0, 0.0)
                elif 2.18 <= alarm_phase < 3.30:
                    # Let filtered Fz recover before the shear tear demo starts.
                    target_force = (0.0, 0.0, 0.0)
                elif 3.30 <= alarm_phase < 3.90:
                    # Shear tissue-tear setup: positive Fz with obvious lateral oscillation.
                    lateral_flip = int((alarm_phase - 3.30) / 0.08) % 2
                    if lateral_flip == 0:
                        target_force = (8.0, 38.0, -28.0)
                    else:
                        target_force = (8.0, -38.0, 28.0)
                elif 3.90 <= alarm_phase < 4.22:
                    # Shear tissue tear: Fz drops while lateral force continues to swing.
                    lateral_flip = int((alarm_phase - 3.90) / 0.08) % 2
                    if lateral_flip == 0:
                        target_force = (-10.0, -38.0, 28.0)
                    else:
                        target_force = (-10.0, 38.0, -28.0)
                elif 5.55 <= alarm_phase < 6.65:
                    # A second high-Fz plateau keeps puncture/thread overload visible in shear mode.
                    target_force = (12.0, 0.0, 0.0)

                if target_force is not None:
                    values = self._values_for_target_force(target_force, random)
        else:
            # Generate 4 mock float values (sine wave with different phases)
            ch1 = math.sin(2 * math.pi * 1.0 * t) * 1.0 + 0.5  # 1 Hz
            ch2 = math.cos(2 * math.pi * 2.0 * t) * 1.0 + 1.0 # 2 Hz
            ch3 = math.sin(2 * math.pi * 0.5 * t) * 0.5 + 0.2   # 0.5 Hz
            ch4 = math.cos(2 * math.pi * 0.2 * t) * 0.8 - 0.3   # 0.2 Hz
            values = [ch1, ch2, ch3, ch4]

        # Construct the raw frame
        # 1. Start with zeroes
        frame = bytearray(self.frame_length)

        # 2. Write header and metadata (e.g., 81 02 1D)
        for i, b in enumerate(self.header_bytes):
            if i < len(frame):
                frame[i] = b

        if len(frame) >= 5:
            frame[2] = 0x81
            frame[3] = 0x02
            frame[4] = 0x1D

        # 3. Write float values
        offset = self.data_offset
        fmt = self.float_endian + 'f'
        for val in values:
            b_val = struct.pack(fmt, val)
            for i, b in enumerate(b_val):
                if offset + i < len(frame):
                    frame[offset + i] = b
            offset += 4

        # 4. Write mock trailer
        trailer_mock = b'\xAA' * (self.frame_length - self.trailer_offset)
        for i, b in enumerate(trailer_mock):
            if self.trailer_offset + i < len(frame):
                frame[self.trailer_offset + i] = b

        return bytes(frame)

    def is_connected(self) -> bool:
        return self._connected

    def _values_for_target_force(self, target_force, random_module):
        if self.force_to_voltage is not None:
            delta = self.force_to_voltage @ np.asarray(target_force, dtype=float)
        else:
            delta = np.asarray([target_force[0] / 125.0] * 4, dtype=float)

        return [
            self.baseline_v[i] + float(delta[i]) + (random_module.random() - 0.5) * self.baseline_noise_v
            for i in range(4)
        ]

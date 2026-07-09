import json
import socket
import threading
import time
from typing import Any


class CppBridgeClient:
    def __init__(self, host: str, port: int, heartbeat_interval_ms: int = 300, timeout_ms: int = 1000):
        self.host = host
        self.port = port
        self.heartbeat_interval = heartbeat_interval_ms / 1000.0
        self.timeout = timeout_ms / 1000.0
        self._sock: socket.socket | None = None
        self._running = False
        self._rx_thread: threading.Thread | None = None
        self._heartbeat_thread: threading.Thread | None = None
        self._send_lock = threading.Lock()
        self._telemetry_lock = threading.Lock()
        self._latest_telemetry: dict[str, Any] = {}
        self._recent_messages: list[dict[str, Any]] = []
        self._last_message_time = 0.0

    def connect(self):
        self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._sock.settimeout(0.2)
        self._running = True
        self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self._rx_thread.start()
        self._heartbeat_thread.start()

    def close(self):
        self._running = False
        if self._sock:
            try:
                self._sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._sock.close()
            self._sock = None

    def latest_telemetry(self) -> dict[str, Any]:
        with self._telemetry_lock:
            return dict(self._latest_telemetry)

    def recent_messages(self) -> list[dict[str, Any]]:
        with self._telemetry_lock:
            return [dict(msg) for msg in self._recent_messages]

    def send_feedback(self, gripper_feedback: float):
        self._send_json({
            "type": "set_omega_feedback",
            "gripper_feedback": float(gripper_feedback),
        })

    def send_command(self, command: str):
        self._send_json({
            "type": "command",
            "command": command,
        })

    def _heartbeat_loop(self):
        while self._running:
            try:
                self._send_json({"type": "heartbeat"})
            except OSError:
                self._running = False
                return
            time.sleep(self.heartbeat_interval)

    def _rx_loop(self):
        buffer = ""
        while self._running:
            try:
                assert self._sock is not None
                chunk = self._sock.recv(4096)
                if not chunk:
                    self._running = False
                    return
                buffer += chunk.decode("utf-8", errors="replace")
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    self._handle_line(line.strip())
            except socket.timeout:
                continue
            except OSError:
                self._running = False
                return

    def _handle_line(self, line: str):
        if not line:
            return
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            return
        self._last_message_time = time.monotonic()
        with self._telemetry_lock:
            self._recent_messages.append(msg)
            self._recent_messages = self._recent_messages[-100:]
            if msg.get("type") == "telemetry":
                self._latest_telemetry = msg

    def _send_json(self, obj: dict[str, Any]):
        data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
        with self._send_lock:
            if not self._sock:
                raise ConnectionError("C++ bridge is not connected")
            self._sock.sendall(data)

import json
import platform
import sys
import time
import traceback
from pathlib import Path


class DiagnosticLogger:
    def __init__(self, config_path: str, config: dict, save_dir: str = "diagnostics"):
        self.save_dir = Path(save_dir).resolve()
        self.save_dir.mkdir(parents=True, exist_ok=True)
        self.path = self.save_dir / time.strftime("diagnostics_%Y%m%d_%H%M%S.txt")
        self._file = self.path.open("w", encoding="utf-8")
        self.write_section("session", {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "config_path": config_path,
            "python": sys.version,
            "executable": sys.executable,
            "platform": platform.platform(),
            "cwd": str(Path.cwd()),
        })
        self.write_section("config", config)

    def log(self, text: str):
        line = f"{time.strftime('%H:%M:%S')} {text}"
        self._file.write(line + "\n")
        self._file.flush()

    def exception(self, title: str, exc: BaseException):
        self.log(f"{title}: {exc.__class__.__name__}: {exc}")
        trace = traceback.format_exc()
        self._file.write(trace)
        if not trace.endswith("\n"):
            self._file.write("\n")
        self._file.flush()

    def write_section(self, title: str, data):
        self._file.write(f"\n===== {title} =====\n")
        if isinstance(data, str):
            self._file.write(data + "\n")
        else:
            self._file.write(json.dumps(data, ensure_ascii=False, indent=2, default=str) + "\n")
        self._file.flush()

    def snapshot(self, title: str, data: dict):
        self.write_section(title, data)

    def close(self):
        if self._file:
            self._file.flush()
            self._file.close()
            self._file = None

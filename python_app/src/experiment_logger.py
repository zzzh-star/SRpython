import csv
import time
from pathlib import Path


class ExperimentLogger:
    FIELDNAMES = [
        "timestamp",
        "sample_index",
        "fx",
        "fy",
        "fz",
        "fn",
        "ft",
        "grasp_state",
        "gripper_feedback_target",
        "gripper_feedback_applied",
        "sma_command",
        "sma_temperature",
        "omega_px",
        "omega_py",
        "omega_pz",
        "cpp_warning",
    ]

    def __init__(self, config: dict):
        self.config = config
        self.save_dir = Path(config.get("save_dir", "data"))
        self.auto_save = bool(config.get("auto_save", True))
        self._file = None
        self._writer = None
        self.current_path = None

    def start(self):
        if not self.auto_save:
            return None
        self.save_dir.mkdir(parents=True, exist_ok=True)
        path = self.save_dir / time.strftime("experiment_%Y%m%d_%H%M%S.csv")
        self._file = path.open("w", newline="", encoding="utf-8")
        self._writer = csv.DictWriter(self._file, fieldnames=self.FIELDNAMES)
        self._writer.writeheader()
        self.current_path = path
        print(f"Logging to {path}")
        return path

    def write_row(self, row: dict):
        if not self._writer:
            return
        self._writer.writerow({name: row.get(name, "") for name in self.FIELDNAMES})

    def stop(self):
        if self._file:
            self._file.flush()
            self._file.close()
            self._file = None
            self._writer = None

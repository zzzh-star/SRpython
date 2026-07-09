import time
from collections import deque
import numpy as np

class SafetyMonitor:
    def __init__(self, config):
        self.config = config.get("safety", {})
        self.enabled = self.config.get("enabled", True)
        self.mode = "牵拉模式" # Default

        self.trac_cfg = self.config.get("traction", {
            "safe_fz_threshold_n": 5.0,
            "stable_pull_min_fz_n": 0.5,
            "stable_pull_duration_ms": 500,
            "stable_pull_fluctuation_ratio": 0.30,
            "slip_drop_ratio": 0.50,
            "slip_window_ms": 100,
            "tear_drop_ratio": 0.40,
            "tear_drop_window_ms": 50,
            "overload_duration_ms": 300
        })

        self.shear_cfg = self.config.get("suturing", {
            "puncture_safe_fz_threshold_n": 8.0,
            "thread_safe_fz_threshold_n": 4.0,
            "slope_threshold_n_per_s": 0.5,
            "puncture_drop_ratio": 0.60,
            "puncture_drop_window_ms": 100,
            "needle_slip_drop_ratio": 0.90,
            "tear_drop_ratio": 0.40,
            "lateral_fluctuation_ratio": 0.50,
            "thread_overload_duration_ms": 1000,
            "low_plateau_min_n": 0.2,
            "low_plateau_max_n": 1.0,
            "low_plateau_fluctuation_n": 0.3
        })

        # Max window needed is 1000ms. If sample rate is 20ms, that's 50 samples.
        self.history_len = 100
        self.hist_t = deque(maxlen=self.history_len)
        self.hist_fx = deque(maxlen=self.history_len)
        self.hist_fy = deque(maxlen=self.history_len)
        self.hist_fz = deque(maxlen=self.history_len)

        self.alarms = []
        self.recent_alarm = None
        self.reset_alarms()

    def set_task_mode(self, mode):
        self.mode = mode
        self.hist_t.clear()
        self.hist_fx.clear()
        self.hist_fy.clear()
        self.hist_fz.clear()
        self.reset_alarms()

    def reset_alarms(self):
        if self.mode == "牵拉模式":
            self.alarms = [
                {"event": "组织滑脱", "level": "未触发", "reason": "依据：Fz 在 100 ms 内突降 > 50%", "val": "", "ts": 0},
                {"event": "组织撕裂", "level": "未触发", "reason": "依据：Fz 超阈值后快速下降", "val": "", "ts": 0},
                {"event": "牵拉过载", "level": "未触发", "reason": "依据：Fz > 5 N 且持续 > 300 ms", "val": "", "ts": 0}
            ]
        else:
            self.alarms = [
                {"event": "缝合针脱出", "level": "未触发", "reason": "依据：Fx/Fy/Fz 同步降低 > 90%", "val": "", "ts": 0},
                {"event": "穿刺力过大", "level": "未触发", "reason": "依据：Fz > 8 N 且持续 > 100 ms", "val": "", "ts": 0},
                {"event": "组织撕裂", "level": "未触发", "reason": "依据：Fz 骤降且侧向力大幅波动", "val": "", "ts": 0},
                {"event": "拉线过紧", "level": "未触发", "reason": "依据：Fz > 4 N 且持续 > 1 s", "val": "", "ts": 0}
            ]

    def update(self, fx, fy, fz, timestamp_ms, decoder_status):
        if not self.enabled or decoder_status not in ["已启用", "死区内"]:
            return self.alarms

        self.hist_t.append(timestamp_ms)
        self.hist_fx.append(fx)
        self.hist_fy.append(fy)
        self.hist_fz.append(fz)

        if len(self.hist_t) < 5:
            return self.alarms

        t_arr = np.array(self.hist_t)
        fz_arr = np.array(self.hist_fz)

        # Reset current frame levels so they drop back if condition clears
        for a in self.alarms:
             if timestamp_ms - a["ts"] > 2000: # cooldown for UI display persistence
                  a["level"] = "未触发"

        if self.mode == "牵拉模式":
            self._eval_traction(t_arr, fz_arr, fz, timestamp_ms)
        else:
            self._eval_shear(t_arr, fz_arr, fx, fy, fz, timestamp_ms)

        return self.alarms

    def _get_window(self, t_arr, data_arr, current_t, window_ms):
        mask = t_arr >= (current_t - window_ms)
        return t_arr[mask], data_arr[mask]

    def _trigger_alarm(self, event_name, level, val_str, ts):
        for a in self.alarms:
            if a["event"] == event_name:
                a["level"] = level
                a["val"] = val_str
                a["ts"] = ts

        if level in ["预警", "已触发"]:
             self.recent_alarm = {"event": event_name, "level": level, "ts": ts}

    def _eval_traction(self, t_arr, fz_arr, fz, ts):
        # 1. Slip: Fz drops > 50% in 100ms
        t100, fz100 = self._get_window(t_arr, fz_arr, ts, self.trac_cfg["slip_window_ms"])
        if len(fz100) > 1:
            peak = np.max(fz100)
            if peak > 1.0 and fz < peak * (1.0 - self.trac_cfg["slip_drop_ratio"]):
                self._trigger_alarm("组织滑脱", "已触发", f"峰值:{peak:.1f}N->{fz:.1f}N", ts)

        # 2. Tear: Fz drops > 40% in 50ms from a high force state
        t50, fz50 = self._get_window(t_arr, fz_arr, ts, self.trac_cfg["tear_drop_window_ms"])
        if len(fz50) > 1:
            peak = np.max(fz50)
            # Assuming threshold was crossed
            if peak > self.trac_cfg["safe_fz_threshold_n"] and fz < peak * (1.0 - self.trac_cfg["tear_drop_ratio"]):
                self._trigger_alarm("组织撕裂", "已触发", f"突降:{peak:.1f}N->{fz:.1f}N", ts)

        # 3. Overload: > 5N for 300ms
        t300, fz300 = self._get_window(t_arr, fz_arr, ts, self.trac_cfg["overload_duration_ms"])
        if len(fz300) > 5 and np.all(fz300 > self.trac_cfg["safe_fz_threshold_n"]):
            self._trigger_alarm("牵拉过载", "已触发", f"持续>{self.trac_cfg['safe_fz_threshold_n']}N", ts)
        elif fz > self.trac_cfg["safe_fz_threshold_n"]:
            self._trigger_alarm("牵拉过载", "预警", f"接近阈值", ts)

    def _eval_shear(self, t_arr, fz_arr, fx, fy, fz, ts):
        # 1. Puncture overload: > 8N for 100ms
        t100, fz100 = self._get_window(t_arr, fz_arr, ts, 100)
        if len(fz100) > 1 and np.all(fz100 > self.shear_cfg["puncture_safe_fz_threshold_n"]):
             self._trigger_alarm("穿刺力过大", "已触发", f">{self.shear_cfg['puncture_safe_fz_threshold_n']}N", ts)

        # 2. Thread Overload: > 4N for 1000ms
        t1000, fz1000 = self._get_window(t_arr, fz_arr, ts, 1000)
        if len(fz1000) > 5 and np.all(fz1000 > self.shear_cfg["thread_safe_fz_threshold_n"]):
             self._trigger_alarm("拉线过紧", "已触发", f"持续>{self.shear_cfg['thread_safe_fz_threshold_n']}N", ts)
        elif fz > self.shear_cfg["thread_safe_fz_threshold_n"]:
             self._trigger_alarm("拉线过紧", "预警", f"拉力: {fz:.1f}N", ts)

        # 3. Needle Slip: drops > 90% (simplified check across last 200ms)
        t200, fz200 = self._get_window(t_arr, fz_arr, ts, 200)
        if len(fz200) > 2:
             peak = np.max(fz200)
             if peak > 2.0 and fz < peak * 0.1:
                  self._trigger_alarm("缝合针脱出", "已触发", f"峰值:{peak:.1f}N->{fz:.1f}N", ts)

        # 4. Tissue tear during traverse (simplified logic detecting lateral drop)
        fx_arr = np.array(self.hist_fx)
        fy_arr = np.array(self.hist_fy)
        _, fx100 = self._get_window(t_arr, fx_arr, ts, 100)
        _, fy100 = self._get_window(t_arr, fy_arr, ts, 100)
        if len(fz100) > 1 and np.max(fz100) > 1.0:
             if fz < np.max(fz100) * 0.6 and (np.std(fx100) > 0.5 or np.std(fy100) > 0.5):
                  self._trigger_alarm("组织撕裂", "已触发", f"侧向高频波动", ts)

    def get_current_alarms(self):
        return self.alarms

    def get_recent_alarm(self):
        return self.recent_alarm

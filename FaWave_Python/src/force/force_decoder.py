import math

class ForceDecoder:
    def __init__(self, config=None):
        self.config = config.get("force_decoder", {}) if config else {}
        self.enabled = self.config.get("enabled", False)

        # Load params
        self.startup_warmup_ms = self.config.get("startup_warmup_ms", 5000)

        matrix_cfg = self.config.get("matrix", [
            [39.2375, 29.8622, 24.3702, 32.1236],
            [17.6342, -0.1770, -20.2660, 0.3688],
            [-7.9958, 19.9397, -6.9973, -17.1562]
        ])
        self.decouple_matrix = matrix_cfg

        filter_cfg = self.config.get("filter", {})
        self.input_ema_alpha = filter_cfg.get("input_ema_alpha", 0.03)
        self.force_ema_alpha_fz = filter_cfg.get("force_ema_alpha_fz", 0.10)
        self.force_ema_alpha_fx = filter_cfg.get("force_ema_alpha_fx", 0.15)
        self.force_ema_alpha_fy = filter_cfg.get("force_ema_alpha_fy", 0.15)
        self.use_median3_filter = filter_cfg.get("use_median3_filter", True)
        self.median_buf_len = 3

        baseline_cfg = self.config.get("baseline", {})
        self.baseline_smooth_alpha = baseline_cfg.get("baseline_smooth_alpha", 0.03)
        self.baseline_converge_th = baseline_cfg.get("baseline_converge_th", 0.003)
        self.baseline_win = baseline_cfg.get("baseline_win", 30)
        self.baseline_range_th = baseline_cfg.get("baseline_range_th", 0.03)
        self.baseline_hold_ms = baseline_cfg.get("baseline_hold_ms", 1000)
        self.startup_range_th = baseline_cfg.get("startup_range_th", 0.03)

        deadzone_cfg = self.config.get("deadzone", {})
        self.force_deadzone_fz = deadzone_cfg.get("fz", 1.0)
        self.force_deadzone_fx = deadzone_cfg.get("fx", 0.5)
        self.force_deadzone_fy = deadzone_cfg.get("fy", 0.5)

        small_th_cfg = self.config.get("small_force_threshold", {})
        self.small_force_th_fz = small_th_cfg.get("fz", 4.0)
        self.small_force_th_fx = small_th_cfg.get("fx", 3.5)
        self.small_force_th_fy = small_th_cfg.get("fy", 3.5)

        self.reset()

    def reset(self):
        self.initialized = False
        self.baseline_v = [0.0]*4
        self.baseline_target_v = [0.0]*4
        self.filt_v = [0.0]*4
        self.raw_hist = [[0.0]*self.median_buf_len for _ in range(4)]
        self.raw_idx = 0
        self.force_filt = [0.0]*3 # Fz, Fx, Fy

        self.stable_buf = [[0.0]*self.baseline_win for _ in range(4)]
        self.stable_buf_idx = 0
        self.stable_sample_count = 0
        self.stable_buf_full = False

        self.stable_start_ms = 0
        self.stable_timing = False
        self.baseline_updating = False

        self.startup_begin_ms = 0
        self.startup_warmup_started = False
        self.startup_baseline_done = False
        self.auto_update_enabled = self.config.get("baseline", {}).get("auto_update_enabled", True)

    def _clampf(self, v, lo, hi):
        if v < lo: return lo
        if v > hi: return hi
        return v

    def _ema_alpha(self, new_val, old_val, alpha):
        alpha = self._clampf(alpha, 0.0, 1.0)
        return alpha * new_val + (1.0 - alpha) * old_val

    def _median3(self, a, b, c):
        arr = [a, b, c]
        arr.sort()
        return arr[1]

    def _window_mean(self, buf):
        if len(buf) == 0: return 0.0
        return sum(buf) / len(buf)

    def _window_range(self, buf):
        if len(buf) == 0: return 0.0
        return max(buf) - min(buf)

    def _apply_deadzone(self, val, threshold):
        return 0.0 if abs(val) < threshold else val

    def initialize(self, init_v):
        if not init_v or len(init_v) != 4:
            return -1

        self.reset()

        for ch in range(4):
            self.baseline_v[ch] = init_v[ch]
            self.baseline_target_v[ch] = init_v[ch]
            self.filt_v[ch] = init_v[ch]

            for k in range(self.median_buf_len):
                self.raw_hist[ch][k] = init_v[ch]

            for i in range(self.baseline_win):
                self.stable_buf[ch][i] = init_v[ch]

        self.initialized = True
        return 0

    def update(self, v_in, timestamp_ms):
        if not self.enabled:
            return {
                "fx": 0.0, "fy": 0.0, "fz": 0.0,
                "fx_raw": 0.0, "fy_raw": 0.0, "fz_raw": 0.0,
                "fx_filtered": 0.0, "fy_filtered": 0.0, "fz_filtered": 0.0,
                "d": [0.0]*4, "baseline": [0.0]*4,
                "valid": False, "status": "未启用", "in_deadzone": False
            }

        if not self.initialized:
            return {
                "fx": 0.0, "fy": 0.0, "fz": 0.0,
                "fx_raw": 0.0, "fy_raw": 0.0, "fz_raw": 0.0,
                "fx_filtered": 0.0, "fy_filtered": 0.0, "fz_filtered": 0.0,
                "d": [0.0]*4, "baseline": [0.0]*4,
                "valid": False, "status": "未初始化", "in_deadzone": False
            }

        if not self.startup_warmup_started:
            self.startup_begin_ms = timestamp_ms
            self.startup_warmup_started = True

        # 1. Input filter (Median3 + EMA)
        for ch in range(4):
            self.raw_hist[ch][self.raw_idx] = v_in[ch]
            if self.use_median3_filter:
                v_med = self._median3(
                    self.raw_hist[ch][0],
                    self.raw_hist[ch][1],
                    self.raw_hist[ch][2]
                )
            else:
                v_med = v_in[ch]

            self.filt_v[ch] = self._ema_alpha(v_med, self.filt_v[ch], self.input_ema_alpha)

        self.raw_idx = (self.raw_idx + 1) % self.median_buf_len

        # 2. Stable window update
        for ch in range(4):
            self.stable_buf[ch][self.stable_buf_idx] = self.filt_v[ch]

        self.stable_buf_idx = (self.stable_buf_idx + 1) % self.baseline_win

        if self.stable_sample_count < self.baseline_win:
            self.stable_sample_count += 1
            if self.stable_sample_count >= self.baseline_win:
                self.stable_buf_full = True

        # 3. Startup auto baseline
        if not self.startup_baseline_done:
            warmup_elapsed = timestamp_ms - self.startup_begin_ms

            if warmup_elapsed >= self.startup_warmup_ms and self.stable_buf_full:
                stable_voltage = True
                for ch in range(4):
                    if self._window_range(self.stable_buf[ch]) >= self.startup_range_th:
                        stable_voltage = False
                        break

                if stable_voltage:
                    for ch in range(4):
                        v_mean = self._window_mean(self.stable_buf[ch])
                        self.baseline_v[ch] = v_mean
                        self.baseline_target_v[ch] = v_mean

                    self.baseline_updating = False
                    self.stable_timing = False
                    self.startup_baseline_done = True

                    self.force_filt[0] = 0.0
                    self.force_filt[1] = 0.0
                    self.force_filt[2] = 0.0

            return {
                "fx": 0.0, "fy": 0.0, "fz": 0.0,
                "fx_raw": 0.0, "fy_raw": 0.0, "fz_raw": 0.0,
                "fx_filtered": 0.0, "fy_filtered": 0.0, "fz_filtered": 0.0,
                "d": [0.0]*4, "baseline": list(self.baseline_v),
                "valid": False, "status": "基线建立中", "in_deadzone": False
            }

        # 4. Smooth baseline update
        if self.baseline_updating:
            converged = True
            for ch in range(4):
                self.baseline_v[ch] += self.baseline_smooth_alpha * (self.baseline_target_v[ch] - self.baseline_v[ch])
                if abs(self.baseline_v[ch] - self.baseline_target_v[ch]) > self.baseline_converge_th:
                    converged = False
            if converged:
                self.baseline_updating = False

        # 5. Differential voltage
        d = [0.0]*4
        for ch in range(4):
            d[ch] = self.filt_v[ch] - self.baseline_v[ch]

        # 6. Matrix decoupling
        m = self.decouple_matrix
        fz_raw = m[0][0]*d[0] + m[0][1]*d[1] + m[0][2]*d[2] + m[0][3]*d[3]
        fx_raw = m[1][0]*d[0] + m[1][1]*d[1] + m[1][2]*d[2] + m[1][3]*d[3]
        fy_raw = m[2][0]*d[0] + m[2][1]*d[1] + m[2][2]*d[2] + m[2][3]*d[3]

        # 7. Output EMA filter
        self.force_filt[0] = self._ema_alpha(fz_raw, self.force_filt[0], self.force_ema_alpha_fz)
        self.force_filt[1] = self._ema_alpha(fx_raw, self.force_filt[1], self.force_ema_alpha_fx)
        self.force_filt[2] = self._ema_alpha(fy_raw, self.force_filt[2], self.force_ema_alpha_fy)

        fz_filtered = self.force_filt[0]
        fx_filtered = self.force_filt[1]
        fy_filtered = self.force_filt[2]

        # 8. Normal auto baseline update logic
        if self.stable_buf_full and self.auto_update_enabled:
            small_force = (abs(fz_filtered) < self.small_force_th_fz and
                           abs(fx_filtered) < self.small_force_th_fx and
                           abs(fy_filtered) < self.small_force_th_fy)

            stable_voltage = True
            for ch in range(4):
                if self._window_range(self.stable_buf[ch]) >= self.baseline_range_th:
                    stable_voltage = False
                    break

            if small_force and stable_voltage:
                if not self.stable_timing:
                    self.stable_timing = True
                    self.stable_start_ms = timestamp_ms
                elif (timestamp_ms - self.stable_start_ms) >= self.baseline_hold_ms:
                    for ch in range(4):
                        self.baseline_target_v[ch] = self._window_mean(self.stable_buf[ch])
                    self.baseline_updating = True
                    self.stable_timing = False
            else:
                self.stable_timing = False

        # 9. Deadzone
        fz = self._apply_deadzone(fz_filtered, self.force_deadzone_fz)
        fx = self._apply_deadzone(fx_filtered, self.force_deadzone_fx)
        fy = self._apply_deadzone(fy_filtered, self.force_deadzone_fy)

        in_deadzone = (fz == 0.0 and fx == 0.0 and fy == 0.0)
        status = "死区内" if in_deadzone else "已启用"

        # 10. Return result
        return {
            "fx": fx,
            "fy": fy,
            "fz": fz,
            "fx_raw": fx_raw,
            "fy_raw": fy_raw,
            "fz_raw": fz_raw,
            "fx_filtered": fx_filtered,
            "fy_filtered": fy_filtered,
            "fz_filtered": fz_filtered,
            "d": d,
            "baseline": list(self.baseline_v),
            "valid": True,
            "status": status,
            "in_deadzone": in_deadzone
        }

    def set_baseline(self, baseline_v=None):
        if not self.initialized:
            return -2

        if baseline_v:
            for ch in range(4):
                self.baseline_v[ch] = baseline_v[ch]
                self.baseline_target_v[ch] = baseline_v[ch]
        else:
            for ch in range(4):
                self.baseline_v[ch] = self.filt_v[ch]
                self.baseline_target_v[ch] = self.filt_v[ch]

        for ch in range(4):
            for i in range(self.baseline_win):
                self.stable_buf[ch][i] = self.baseline_v[ch]

        self.stable_buf_idx = 0
        self.stable_sample_count = 0
        self.stable_buf_full = False
        self.stable_timing = False
        self.baseline_updating = False

        self.force_filt[0] = 0.0
        self.force_filt[1] = 0.0
        self.force_filt[2] = 0.0

        self.startup_baseline_done = True
        self.startup_warmup_started = True

        return 0

    def get_baseline(self):
        if not self.initialized:
            return None
        return list(self.baseline_v)

    def set_matrix(self, matrix):
        if not matrix or len(matrix) != 3 or len(matrix[0]) != 4:
            return -1
        self.decouple_matrix = [[v for v in row] for row in matrix]
        return 0
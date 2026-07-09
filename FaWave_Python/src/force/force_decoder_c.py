import ctypes
import os
from ..utils.resource import resource_path

class ForceResult(ctypes.Structure):
    _fields_ = [
        ("Fz", ctypes.c_float),
        ("Fx", ctypes.c_float),
        ("Fy", ctypes.c_float),
        ("d", ctypes.c_float * 4)
    ]

class CForceDecoder:
    def __init__(self, config=None):
        self.config = config.get("force_decoder", {}) if config else {}
        self.enabled = self.config.get("enabled", False)

        self.dll = None
        self.dll_valid = False

        c_dll_path_config = self.config.get("c_dll_path", "src/force/c_backend/force_decoder.dll")
        dll_path = resource_path(c_dll_path_config)
        so_path = dll_path.replace('.dll', '.so')

        if os.path.exists(dll_path):
            self.dll = ctypes.CDLL(dll_path)
        elif os.path.exists(so_path):
            self.dll = ctypes.CDLL(so_path)

        if self.dll:
            try:
                self.dll.init_sensor.argtypes = [ctypes.POINTER(ctypes.c_float)]
                self.dll.update_sensor.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_uint32, ctypes.POINTER(ForceResult)]
                self.dll.set_baseline.argtypes = [ctypes.POINTER(ctypes.c_float)]
                self.dll_valid = True
            except AttributeError:
                self.dll_valid = False

        self.initialized = False
        self.startup_baseline_done = False
        self.result = ForceResult()

    def reset(self):
        self.initialized = False
        self.startup_baseline_done = False

    def initialize(self, init_v):
        if not self.dll: return -1
        v_arr = (ctypes.c_float * 4)(*init_v)
        res = self.dll.init_sensor(v_arr)
        if res == 0:
            self.initialized = True
        return res

    def update(self, v_in, timestamp_ms):
        ret_err = {
            "fx": 0.0, "fy": 0.0, "fz": 0.0,
            "fx_raw": 0.0, "fy_raw": 0.0, "fz_raw": 0.0,
            "fx_filtered": 0.0, "fy_filtered": 0.0, "fz_filtered": 0.0,
            "d": [0.0]*4, "baseline": [0.0]*4,
            "valid": False, "status": "", "in_deadzone": False
        }

        if not self.enabled:
            ret_err["status"] = "未启用"
            return ret_err
        if not getattr(self, "dll_valid", False):
            ret_err["status"] = "错误"
            return ret_err
        if not self.initialized:
            ret_err["status"] = "未初始化"
            return ret_err

        v_arr = (ctypes.c_float * 4)(*v_in)
        self.dll.update_sensor(v_arr, timestamp_ms, ctypes.byref(self.result))

        fx = self.result.Fx
        fy = self.result.Fy
        fz = self.result.Fz
        d = [self.result.d[0], self.result.d[1], self.result.d[2], self.result.d[3]]

        # Attempt to determine state conservatively. In a real scenario, we'd query C state.
        status = "未定"
        in_deadzone = False

        if timestamp_ms < self.config.get("startup_warmup_ms", 5000):
            status = "基线建立中"
        elif fz == 0.0 and fx == 0.0 and fy == 0.0 and d[0] == 0.0 and d[1] == 0.0 and d[2] == 0.0 and d[3] == 0.0:
            status = "基线建立中" # or strictly inside deadzone from frame 1
        elif fz == 0.0 and fx == 0.0 and fy == 0.0:
            status = "死区内"
            in_deadzone = True
        else:
            self.startup_baseline_done = True
            status = "已启用"

        return {
            "fx": fx,
            "fy": fy,
            "fz": fz,
            "fx_raw": fx, # C backend doesnt currently expose raw
            "fy_raw": fy,
            "fz_raw": fz,
            "fx_filtered": fx, # C backend doesnt currently expose filtered
            "fy_filtered": fy,
            "fz_filtered": fz,
            "d": d,
            "baseline": [0.0]*4, # C backend doesnt currently expose baseline
            "valid": True,
            "status": status,
            "in_deadzone": in_deadzone
        }

    def set_baseline(self, baseline_v=None):
        if not self.dll: return -1
        if baseline_v:
            b_arr = (ctypes.c_float * 4)(*baseline_v)
            return self.dll.set_baseline(b_arr)
        else:
            return self.dll.set_baseline(None)

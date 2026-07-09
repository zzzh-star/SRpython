import math


class ForceProcessor:
    def __init__(self, config: dict):
        self.alpha = float(config.get("filter_alpha", 0.2))
        self._fx = 0.0
        self._fy = 0.0
        self._fz = 0.0
        self._initialized = False

    def update(self, sample: dict) -> dict:
        fx = float(sample.get("fx", 0.0))
        fy = float(sample.get("fy", 0.0))
        fz = float(sample.get("fz", 0.0))

        if not self._initialized:
            self._fx, self._fy, self._fz = fx, fy, fz
            self._initialized = True
        else:
            a = self.alpha
            self._fx += a * (fx - self._fx)
            self._fy += a * (fy - self._fy)
            self._fz += a * (fz - self._fz)

        fn = max(0.0, self._fz)
        ft = math.sqrt(self._fx * self._fx + self._fy * self._fy)

        return {
            "fx": self._fx,
            "fy": self._fy,
            "fz": self._fz,
            "fn": fn,
            "ft": ft,
        }

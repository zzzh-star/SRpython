import sys
import os
import csv
import json
import numpy as np

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../../')))

from src.force.force_decoder import ForceDecoder

# Apply np.float32 injection into force_decoder to closely match C
# We do this by subclassing and overriding the clampf, ema_alpha, etc. to explicitly use np.float32

class NumpyForceDecoder(ForceDecoder):
    def reset(self):
        super().reset()
        self.baseline_v = np.zeros(4, dtype=np.float32)
        self.baseline_target_v = np.zeros(4, dtype=np.float32)
        self.filt_v = np.zeros(4, dtype=np.float32)
        self.raw_hist = np.zeros((4, self.median_buf_len), dtype=np.float32)
        self.force_filt = np.zeros(3, dtype=np.float32)
        self.stable_buf = np.zeros((4, self.baseline_win), dtype=np.float32)

    def _clampf(self, v, lo, hi):
        return np.clip(np.float32(v), np.float32(lo), np.float32(hi))

    def _ema_alpha(self, new_val, old_val, alpha):
        alpha = self._clampf(alpha, 0.0, 1.0)
        return np.float32(alpha * np.float32(new_val) + (1.0 - alpha) * np.float32(old_val))

def run_test(filename):
    with open('../../config/default_config.json') as f:
        config = json.load(f)

    fd = NumpyForceDecoder(config)

    output_filename = filename.replace('.csv', '_out.csv')
    with open(filename, 'r') as fin, open(output_filename, 'w') as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)
        next(reader) # skip header
        writer.writerow(['timestamp_ms', 'fx', 'fy', 'fz', 'd1', 'd2', 'd3', 'd4'])

        first = True
        for row in reader:
            t = int(row[0])
            v_in = [np.float32(float(x)) for x in row[1:]]
            if first:
                fd.initialize(v_in)
                first = False

            res = fd.update(v_in, t)
            writer.writerow([t, res['fx'], res['fy'], res['fz'], res['d'][0], res['d'][1], res['d'][2], res['d'][3]])

if __name__ == '__main__':
    run_test('startup_static.csv')
    run_test('step_force.csv')
    run_test('noise_static.csv')
    print("Python decoder ran.")

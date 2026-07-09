import csv
import sys
import numpy as np

def compare_results(py_csv, c_csv):
    if not os.path.exists(py_csv) or not os.path.exists(c_csv):
        print(f"Missing files: {py_csv} or {c_csv}")
        return

    py_data = []
    with open(py_csv, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for r in reader:
            py_data.append([float(x) for x in r])

    c_data = []
    with open(c_csv, 'r') as f:
        reader = csv.reader(f)
        next(reader)
        for r in reader:
            c_data.append([float(x) for x in r])

    py_arr = np.array(py_data)
    c_arr = np.array(c_data)

    if len(py_arr) != len(c_arr):
        print("Mismatched row counts!")
        return

    diff = np.abs(py_arr - c_arr)

    # columns: time, fx, fy, fz, d1, d2, d3, d4
    force_diff = diff[:, 1:4]
    volt_diff = diff[:, 4:8]

    max_force_err = np.max(force_diff)
    max_volt_err = np.max(volt_diff)

    print(f"--- Comparison Report for {py_csv} vs {c_csv} ---")
    print(f"Max Force Error (Fx/Fy/Fz): {max_force_err:.6e} N")
    print(f"Max Voltage Error (d1-d4): {max_volt_err:.6e} V")

    passed = True
    if max_force_err > 1e-3:
        print("FAILED: Force error exceeds 1e-3 threshold.")
        passed = False
    if max_volt_err > 1e-5:
        print("FAILED: Voltage error exceeds 1e-5 threshold.")
        passed = False

    if passed:
        print("PASSED: All values within acceptable thresholds.")
    print("----------------------------------------------------\n")

if __name__ == '__main__':
    import os
    compare_results('startup_static_out.csv', 'startup_static_c_out.csv')
    compare_results('step_force_out.csv', 'step_force_c_out.csv')
    compare_results('noise_static_out.csv', 'noise_static_c_out.csv')

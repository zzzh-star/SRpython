import sys
import os
import csv
import ctypes

class ForceResult(ctypes.Structure):
    _fields_ = [
        ("Fz", ctypes.c_float),
        ("Fx", ctypes.c_float),
        ("Fy", ctypes.c_float),
        ("d", ctypes.c_float * 4)
    ]

def run_c_test(filename):
    dll_path = os.path.join(os.path.dirname(__file__), '../../src/force/c_ref/force_decoder.dll')
    if not os.path.exists(dll_path):
        # We can't strictly compile a DLL natively under Linux unless cross compiling with mingw but the requirement asks for dll logic
        # We will attempt loading an SO if present, otherwise just print a message for the user to compile it on Windows.
        so_path = os.path.join(os.path.dirname(__file__), '../../src/force/c_ref/force_decoder.so')
        if os.path.exists(so_path):
            dll = ctypes.CDLL(so_path)
        else:
            print("C Reference Library not compiled. Please run build_force_dll.bat on Windows.")
            return
    else:
        dll = ctypes.CDLL(dll_path)

    dll.init_sensor.argtypes = [ctypes.POINTER(ctypes.c_float)]
    dll.update_sensor.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_uint32, ctypes.POINTER(ForceResult)]

    output_filename = filename.replace('.csv', '_c_out.csv')
    with open(filename, 'r') as fin, open(output_filename, 'w') as fout:
        reader = csv.reader(fin)
        writer = csv.writer(fout)
        next(reader) # skip header
        writer.writerow(['timestamp_ms', 'fx', 'fy', 'fz', 'd1', 'd2', 'd3', 'd4'])

        first = True
        result = ForceResult()

        for row in reader:
            t = int(row[0])
            v_in_list = [float(x) for x in row[1:]]
            v_in_arr = (ctypes.c_float * 4)(*v_in_list)

            if first:
                dll.init_sensor(v_in_arr)
                first = False

            dll.update_sensor(v_in_arr, t, ctypes.byref(result))

            writer.writerow([t, result.Fx, result.Fy, result.Fz, result.d[0], result.d[1], result.d[2], result.d[3]])

if __name__ == '__main__':
    run_c_test('startup_static.csv')
    run_c_test('step_force.csv')
    run_c_test('noise_static.csv')
    print("C Reference tests ran.")

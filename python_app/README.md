# Python Main Control Prototype

This folder is the Python-side main-control prototype for the SR/Omega bridge.

Current scope:

- Connect to the C++ `PythonBridgeServer` on `127.0.0.1:8765`
- Receive C++ telemetry
- Generate simulated force data
- Compute `Fn` and `Ft`
- Classify grasp state with threshold rules
- Compute Omega gripper feedback in Python
- Send `set_omega_feedback` to C++
- Log CSV rows
- Simulate SMA commands without opening a real serial port

Run after starting the C++ `SR.exe` bridge:

```powershell
python python_app/main.py --config python_app/config/feedback_config.json
```

## Hardware test

Use the `minist` Anaconda environment:

```powershell
python_app\run_hardware_ui_minist.bat
```

Before hardware testing, edit:

```text
python_app\config\hardware_config.json
```

Set the real force sensor COM port in `force_sensor.port`, and the ESP32/SMA COM port in `sma.port`.

Run this first to list ports and catch obvious environment problems:

```powershell
python_app\hardware_precheck_minist.bat
```

If the configured port shows `BTHENUM`, `Bluetooth`, or `蓝牙链接`, verify it carefully. Those are often virtual Bluetooth COM ports, not the USB serial port of the force sensor or ESP32.

Recommended startup order:

1. Connect Omega, Maxon, force sensor, and ESP32/SMA.
2. Start `x64\Debug\SR.exe`.
3. In the C++ window, initialize Omega/Maxon as before.
4. Run `python_app\run_hardware_ui_minist.bat`.
5. Check the two COM dropdowns. They refresh automatically about every 1.5 seconds, so after plugging in USB, wait briefly and select the new port.
6. Click `List COM Ports` if you want the full port list written into the diagnostics file.
7. Click `Test Force Sensor` and check whether a force sample or raw frame appears in the log.
8. Click `Test SMA` and check whether temperature/response is returned.
9. Click `Reconnect Local` after changing either dropdown selection.
10. Click `Connect C++`.
11. Click `Start Experiment`.
12. Click `Start Recording` when you want to save CSV data.

Useful hardware-test buttons:

- COM dropdowns: automatically refresh when USB serial devices are inserted or removed.
- `Reconnect Local`: closes and reopens the force sensor and SMA serial ports using the current dropdown selections.
- `List COM Ports`: writes all Windows COM ports into the UI log and diagnostics file.
- `Test Force Sensor`: reads one sample and records the last raw sensor frame/parse result.
- `Test SMA`: sends a temperature query and stop command, then records the SMA response.
- `Diagnostic Snapshot`: writes current state to diagnostics and copies the diagnostics file path.

## Error collection

Every UI session creates a diagnostics text file:

```text
diagnostics\diagnostics_YYYYMMDD_HHMMSS.txt
```

The UI also shows the diagnostics file name in the right-side status cards.

When hardware testing fails:

1. Click `Diagnostic Snapshot`.
2. The diagnostics file path is copied to the clipboard.
3. Send that `.txt` file together with a short note about what you clicked before the error.

The diagnostics file includes:

- Python executable, version, platform, working directory;
- full config snapshot;
- force sensor/SMA/C++ bridge startup errors;
- exception tracebacks;
- last SMA command and response;
- recent SMA serial responses;
- force sensor last raw frame and parse status;
- Windows COM port list at startup and at snapshot time;
- recent C++ bridge `ack/error/telemetry` messages;
- latest recording file path if recording is active.

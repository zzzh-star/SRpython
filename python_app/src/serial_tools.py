def list_serial_ports() -> list[dict]:
    try:
        from serial.tools import list_ports
    except Exception as exc:
        return [{"error": f"serial.tools.list_ports unavailable: {exc}"}]

    ports = []
    for port in list_ports.comports():
        ports.append({
            "device": port.device,
            "description": port.description,
            "hwid": port.hwid,
            "manufacturer": port.manufacturer,
            "serial_number": port.serial_number,
        })
    return ports

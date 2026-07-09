import sys
import os

def get_exe_dir():
    """Get the directory of the executable or the script depending on context"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))

def resource_path(relative_path):
    """Get absolute path to resource, works for dev and for PyInstaller _MEIPASS"""
    try:
        base_path = sys._MEIPASS
    except Exception:
        base_path = get_exe_dir()

    return os.path.join(base_path, relative_path)

def config_path(filename="default_config.json"):
    """
    Search for config inside the executable's direct path first (to allow overriding),
    then fallback to the internal package if missing.
    """
    external_config = os.path.join(get_exe_dir(), "config", filename)
    if os.path.exists(external_config):
        return external_config
    return resource_path(f"config/{filename}")

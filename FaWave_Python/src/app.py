import sys
import json
import os
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QIcon
from .ui.main_window import MainWindow
from .utils.logger import setup_logger

from .utils.resource import config_path as get_config_path, resource_path

def load_config():
    target_config = get_config_path()
    try:
        with open(target_config, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        print(f"Failed to load config from {target_config}: {e}")
        # Return a minimal default config if not found
        return {
            "device_ip": "192.168.1.82",
            "device_port": 16008,
            "communication_mode": "Mock",
            "force_decoder": {"input_unit": "V", "input_scale_to_v": 1.0}
        }

def run_app():
    logger = setup_logger()
    logger.info("程序启动 - FaWave 四通道力传感采集系统")

    if os.name == "nt":
        try:
            import ctypes
            app_id = "FaWave.MultidimensionalForceSensing.Platform.1.0"
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(app_id)
            logger.info(f"AppUserModelID 已设置：{app_id}")
        except Exception as e:
            logger.warning(f"设置 AppUserModelID 失败: {e}")

    config = load_config()

    app = QApplication(sys.argv)
    app.setStyle("Fusion") # Base style to build upon

    icon_path = resource_path("assets/app_icon.ico")
    if os.path.exists(icon_path):
        app_icon = QIcon(icon_path)
        app.setWindowIcon(app_icon)
        logger.info(f"应用图标已加载：{icon_path}")
    else:
        logger.warning(f"应用图标不存在: {icon_path}")

    window = MainWindow(config, logger)
    window.show()

    sys.exit(app.exec())

import logging
import sys
import os
from datetime import datetime

def setup_logger(name="FaWaveLogger", log_level=logging.INFO):
    """
    Setup a logger for the FaWave application.
    """
    logger = logging.getLogger(name)
    logger.setLevel(log_level)

    # Avoid duplicate logs
    if not logger.handlers:
        formatter = logging.Formatter(
            '%(asctime)s [%(levelname)s] %(name)s: %(message)s',
            datefmt='%Y-%m-%d %H:%M:%S'
        )

        # Console handler
        console_handler = logging.StreamHandler(sys.stdout)
        console_handler.setFormatter(formatter)
        logger.addHandler(console_handler)

        from .resource import get_exe_dir

        # File handler
        logs_dir = os.path.join(get_exe_dir(), 'logs')
        os.makedirs(logs_dir, exist_ok=True)
        timestamp = datetime.now().strftime("%Y%m%d")
        log_file = os.path.join(logs_dir, f"FaWave_{timestamp}.log")

        file_handler = logging.FileHandler(log_file, encoding='utf-8')
        file_handler.setFormatter(formatter)
        logger.addHandler(file_handler)

    return logger

import json
from pathlib import Path


def load_config(path: str) -> dict:
    config_path = Path(path)
    with config_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def save_config(path: str, config: dict):
    config_path = Path(path)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    with config_path.open("w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)
        f.write("\n")

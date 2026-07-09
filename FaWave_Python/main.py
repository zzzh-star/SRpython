import sys
import os

# Add the parent directory to sys.path if needed
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))

from src.app import run_app

if __name__ == "__main__":
    run_app()
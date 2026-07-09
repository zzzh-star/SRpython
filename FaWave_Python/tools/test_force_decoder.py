import sys
import os

# Add parent dir to path so we can import src
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from src.force.force_decoder import ForceDecoder
import json

def test_force_decoder():
    config = {
        "force_decoder": {
            "enabled": True,
            "startup_warmup_ms": 100,
            "baseline": {
                "baseline_win": 10
            }
        }
    }

    fd = ForceDecoder(config)
    print("1. Initialized properly:", fd is not None)

    res = fd.update([1.0, 1.0, 1.0, 1.0], 0)
    print("2. Update without init returns status:", res["status"])
    assert res["status"] == "未初始化"

    fd.initialize([1.0, 1.0, 1.0, 1.0])
    print("3. Initialization complete. initialized flag:", fd.initialized)
    assert fd.initialized == True

    # Send some fake data
    for i in range(100): # Warmup time is 100ms
        res = fd.update([1.01, 0.99, 1.05, 0.95], i*20)
        if res["valid"]:
            break

    print("4. Startup baseline state:", fd.startup_baseline_done)

    # Fast forward time
    res = fd.update([2.0, 2.0, 2.0, 2.0], 500)
    print("5. After warmup, result valid:", res["valid"], "Fx:", res["fx"])

    print("6. Set baseline manually")
    fd.set_baseline([2.0, 2.0, 2.0, 2.0])

    res = fd.update([3.0, 3.0, 3.0, 3.0], 1000)
    print("7. After baseline set, valid:", res["valid"])

    # Test setting a new matrix
    new_matrix = [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0]
    ]
    fd.set_matrix(new_matrix)
    print("8. Set new matrix.")

if __name__ == "__main__":
    test_force_decoder()
    print("All tests passed.")

import random
import csv

def generate_static():
    with open('startup_static.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp_ms', 'v1', 'v2', 'v3', 'v4'])
        for i in range(500):
            writer.writerow([i*20, 1.0, 1.0, 1.0, 1.0])

def generate_step_force():
    with open('step_force.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp_ms', 'v1', 'v2', 'v3', 'v4'])
        for i in range(250): # warmup
            writer.writerow([i*20, 1.0, 1.0, 1.0, 1.0])
        for i in range(250, 500):
            writer.writerow([i*20, 1.5, 1.0, 1.0, 1.0])

def generate_noise_static():
    with open('noise_static.csv', 'w') as f:
        writer = csv.writer(f)
        writer.writerow(['timestamp_ms', 'v1', 'v2', 'v3', 'v4'])
        for i in range(500):
            v1 = 1.0 + (random.random() - 0.5) * 0.01
            v2 = 1.0 + (random.random() - 0.5) * 0.01
            v3 = 1.0 + (random.random() - 0.5) * 0.01
            v4 = 1.0 + (random.random() - 0.5) * 0.01
            writer.writerow([i*20, v1, v2, v3, v4])

if __name__ == '__main__':
    generate_static()
    generate_step_force()
    generate_noise_static()
    print("Test vectors generated.")

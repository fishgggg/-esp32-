"""读取 ESP32 串口输出并打印

用法: python monitor.py [COM口]，默认 COM6
"""
import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM6'
ser = serial.Serial(port, 115200, timeout=1)
time.sleep(0.5)
print(f"=== ESP32 Serial Monitor ({port}, 10s) ===")
start = time.time()
while time.time() - start < 12:
    line = ser.readline()
    if line:
        try:
            print(line.decode('utf-8', errors='replace'), end='', flush=True)
        except:
            print(str(line), flush=True)
print("\n=== Done ===")
ser.close()

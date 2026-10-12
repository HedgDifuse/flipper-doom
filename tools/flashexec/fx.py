"""Run one flashexec step: python fx.py write|run|erase  (Flipper on COM3)."""
import subprocess, sys, time
import serial

PORT = "COM3"
cmd = sys.argv[1]


def cli(s, line, wait=1.0):
    s.reset_input_buffer()
    s.write(line.encode() + b"\r")
    time.sleep(wait)
    return s.read(s.in_waiting or 1).decode(errors="replace")


s = serial.Serial(PORT, 230400, timeout=1)
time.sleep(0.5)
s.read(s.in_waiting or 1)
cli(s, "storage remove /ext/apps_data/flashexec.txt")
cli(s, "storage remove /ext/apps_data/flashexec.cmd")  # write appends
s.write(b"storage write /ext/apps_data/flashexec.cmd\r")
time.sleep(0.5)
s.write(cmd.encode())
time.sleep(0.3)
s.write(b"\x03")
time.sleep(0.5)
print(cli(s, "storage read /ext/apps_data/flashexec.cmd").strip().splitlines()[-2:])
s.close()

subprocess.run([sys.executable, "-m", "ufbt", "launch"], check=True, capture_output=True)
time.sleep(3)
s = serial.Serial(PORT, 230400, timeout=1)
time.sleep(0.5)
s.read(s.in_waiting or 1)
print(cli(s, "storage read /ext/apps_data/flashexec.txt", 2))
s.close()

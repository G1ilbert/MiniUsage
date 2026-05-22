import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 74880
duration = int(sys.argv[3]) if len(sys.argv) > 3 else 15

s = serial.Serial(port, baud, timeout=1)
print(f"=== Serial Monitor {port} @ {baud} ===")
print(f"Listening for {duration} seconds... (reset ESP to see boot log)")
print("")

end = time.time() + duration
while time.time() < end:
    data = s.read(512)
    if data:
        text = data.decode('latin-1', errors='replace')
        sys.stdout.buffer.write(text.encode('utf-8', errors='replace'))
        sys.stdout.flush()

s.close()
print("\n=== Done ===")

"""Research: hold a float in the game's memory for a while (writes!).
  python hold.py <addr hex> <value> <seconds>"""
import ctypes, struct, sys, time
import memscan as ms

h = ms.open_game(write=True)
k32 = ctypes.WinDLL('kernel32')
addr, value, secs = int(sys.argv[1], 16), float(sys.argv[2]), float(sys.argv[3])
n = ctypes.c_size_t()
end = time.time() + secs
before = struct.unpack('<f', ms.read(h, addr, 4))[0]
while time.time() < end:
    k32.WriteProcessMemory(h, ctypes.c_void_p(addr), struct.pack('<f', value), 4, ctypes.byref(n))
    time.sleep(0.02)
print(f'held {addr:X} at {value} for {secs} s (was {before})')

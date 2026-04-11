import struct

with open(r'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\audio_buf.bin', 'rb') as f:
    data = f.read()
samples = struct.unpack_from('<' + 'h' * (len(data)//2), data)

# Check if samples[0:16] == samples[32:48] (exact repeat)
half0 = samples[0:16]
half1 = samples[16:32]
half0b = samples[32:48]
half1b = samples[48:64]

print("half[0] == half[2]?", half0 == half0b)
print("half[1] == half[3]?", half1 == half1b)
print()
print("half[0]:", list(half0))
print("half[1]:", list(half1))
print()

# Check at later part of recording (away from transients)
h0 = samples[5000:5016]
h1 = samples[5016:5032]
h2 = samples[5032:5048]
h3 = samples[5048:5064]
print(f"At sample 5000:")
print(f"half[0] == half[2]: {h0 == h2}")
print(f"half[1] == half[3]: {h1 == h3}")
print(f"h0: {list(h0)}")
print(f"h1: {list(h1)}")
print(f"h2: {list(h2)}")

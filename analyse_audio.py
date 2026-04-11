import struct, math, sys

with open(r'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\audio_buf.bin', 'rb') as f:
    data = f.read()

samples = struct.unpack_from('<' + 'h' * (len(data)//2), data)
n = len(samples)
print(f"Samples: {n}  ({n/16000:.2f}s)")

# First 32 samples
print("\nFirst 32 samples:")
print([s for s in samples[:32]])

# Stats for windows
def stats(s):
    mn, mx = min(s), max(s)
    rms = math.sqrt(sum(x*x for x in s)/len(s)) if s else 0
    dc = sum(s)/len(s) if s else 0
    clip = sum(1 for x in s if abs(x) >= 32767)
    return f"min={mn:6d} max={mx:6d} rms={rms:7.1f} dc={dc:7.1f} clip={clip}"

print(f"\nFull buffer:     {stats(samples)}")
print(f"samples[0:200]:  {stats(samples[0:200])}")     # first 12ms - still transient?
print(f"samples[200:800]:{stats(samples[200:800])}")   # next 37ms
print(f"noise[0:8000]:   {stats(samples[0:8000])}")    # first 0.5s
print(f"speech[8k:40k]:  {stats(samples[8000:40000])}")# main 2s

# Histogram of absolute values
buckets = [0]*10
for s in samples:
    b = min(abs(s) * 10 // 32768, 9)
    buckets[b] += 1
print("\nAmplitude histogram (0=near-zero, 9=near-clip):")
for i,c in enumerate(buckets):
    bar = '#' * (c * 60 // max(buckets+[1]))
    print(f"  {i*10:3d}-{(i+1)*10-1:3d}%: {bar} ({c})")

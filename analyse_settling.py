import struct, math

with open(r'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\audio_buf.bin', 'rb') as f:
    data = f.read()
samples = struct.unpack_from('<' + 'h' * (len(data)//2), data)
n = len(samples)
print(f"Samples: {n}  ({n/16000:.2f}s)")

# Find where saturation ends — check each 16-sample block for clipping
print("\nFirst 300 blocks of 16 samples (checking for clips):")
clip_end = 0
for i in range(300):
    blk = samples[i*16:(i+1)*16]
    clips = sum(1 for x in blk if abs(x) >= 32767)
    rms = math.sqrt(sum(x*x for x in blk)/len(blk)) if blk else 0
    marker = " <-- CLIP" if clips > 0 else ""
    if clips > 0 or i < 10:
        print(f"  block[{i:3d}] samp[{i*16:5d}..{(i+1)*16-1:5d}] rms={rms:7.1f} clips={clips}{marker}")
    if clips > 0:
        clip_end = (i+1)*16

print(f"\nLast clipping block ends at sample {clip_end}")
print(f"Clean audio starts at sample ~{clip_end} ({clip_end/16000*1000:.1f} ms into recording)")

# RMS of clean region
if clip_end < n:
    clean = samples[clip_end:clip_end+4000]
    rms_c = math.sqrt(sum(x*x for x in clean)/len(clean))
    print(f"RMS of first 250ms clean audio: {rms_c:.1f}")

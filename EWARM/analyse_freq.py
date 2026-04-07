import struct, math

with open(r'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\audio_buf.bin', 'rb') as f:
    data = f.read()
samples = struct.unpack_from('<' + 'h' * (len(data)//2), data)

# Show first 64 samples raw
print("First 64 samples:")
for i in range(0, 64, 16):
    print(f"  [{i:3d}] " + " ".join(f"{samples[i+j]:7d}" for j in range(16)))

# RMS per 16-sample block, first 40 blocks
print("\nRMS per DMA half (16 samples), first 40 blocks:")
for i in range(40):
    blk = samples[i*16:(i+1)*16]
    rms = math.sqrt(sum(x*x for x in blk)/len(blk))
    dc  = sum(blk)/len(blk)
    print(f"  half[{i:2d}] rms={rms:7.1f}  dc={dc:8.1f}")

# Check if even/odd halves have different DC offsets
even_dc = sum(sum(samples[i*16:(i+1)*16]) for i in range(0,100,2)) / (50*16)
odd_dc  = sum(sum(samples[i*16:(i+1)*16]) for i in range(1,100,2)) / (50*16)
print(f"\nEven-half DC avg: {even_dc:.1f}")
print(f"Odd-half  DC avg: {odd_dc:.1f}")
print(f"DC difference: {even_dc - odd_dc:.1f}")

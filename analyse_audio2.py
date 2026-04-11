import struct, math

with open(r'C:\TouchGFXProjects\MyApplication\EWARM\SystemView\audio_buf.bin', 'rb') as f:
    data = f.read()
samples = struct.unpack_from('<' + 'h' * (len(data)//2), data)

# Show settling: RMS of each 100-sample block in first 2000 samples
print("Settling analysis (RMS per 100-sample block, first 2000 samples):")
for i in range(20):
    blk = samples[i*100:(i+1)*100]
    rms = math.sqrt(sum(x*x for x in blk)/len(blk))
    bar = '#' * int(rms/500)
    print(f"  [{i*100:4d}-{(i+1)*100-1:4d}] rms={rms:7.1f} {bar}")

print("\nSteady-state RMS per 1000-sample block (samples 1000-48000):")
for i in range(1, 48):
    blk = samples[i*1000:(i+1)*1000]
    rms = math.sqrt(sum(x*x for x in blk)/len(blk))
    bar = '#' * int(rms/100)
    t_ms = i*1000*1000//16000
    print(f"  t={t_ms:5d}ms rms={rms:6.1f} {bar}")

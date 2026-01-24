import re, struct, sys

# 匹配 sendto(..., "\x.."*16, 16, ...)
pat = re.compile(r'sendto\([^,]+,\s*"((?:\\\\x[0-9a-fA-F]{2}){16})"')

seqs = []
for line in open(sys.argv[1], 'r', errors='ignore'):
    m = pat.search(line)
    if not m:
        continue
    hx = m.group(1).replace("\\x", "")
    b = bytes.fromhex(hx)
    if len(b) != 16:
        continue
    seq = struct.unpack("<I", b[12:16])[0]  # little-endian uint32
    seqs.append(seq)

if not seqs:
    print("No seq extracted. (maybe need strace -xx and -s large enough)")
    sys.exit(0)

non_increasing = sum(1 for i in range(1, len(seqs)) if seqs[i] <= seqs[i-1])
dups = sum(1 for i in range(1, len(seqs)) if seqs[i] == seqs[i-1])

print("total_sendto_msgs:", len(seqs))
print("non_increasing(seq[i]<=seq[i-1]):", non_increasing)
print("adjacent_duplicates(seq[i]==seq[i-1]):", dups)

# 打印前 40 个 seq 供你肉眼确认
print("first_40_seqs:", seqs[:40])

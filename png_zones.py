import struct, zlib

with open(r'C:\MirageWork\MirageVulkan\screenshots\wake.png','rb') as f: d=f.read()
w=struct.unpack('>I',d[16:20])[0]; h=struct.unpack('>I',d[20:24])[0]
i=8; idat=b''
while i<len(d):
    l=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+l]; i+=12+l
    if t==b'IDAT': idat+=c
raw=zlib.decompress(idat)
channels=4; stride=w*channels+1

# Check top/middle/bottom bands for brightness distribution
for name, r1, r2 in [('top 20%', 0, h//5), ('middle', 2*h//5, 3*h//5), ('bottom 20%', 4*h//5, h)]:
    bright=0; total=0
    for row in range(r1, r2):
        base=row*stride+1
        for col in range(0, w*channels, channels):
            r=raw[base+col]; g=raw[base+col+1]; b=raw[base+col+2]
            if r>30 or g>30 or b>30: bright+=1
            total+=1
    print(f'{name}: bright={bright}/{total} ({100*bright//total}%)')

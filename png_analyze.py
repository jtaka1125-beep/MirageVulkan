import struct, zlib
def analyze(path, label=''):
    with open(path,'rb') as f: d=f.read()
    w=struct.unpack('>I',d[16:20])[0]; h=struct.unpack('>I',d[20:24])[0]; ct=d[25]
    i=8; idat=b''
    while i<len(d):
        l=struct.unpack('>I',d[i:i+4])[0]; t=d[i+4:i+8]; c=d[i+8:i+8+l]; i+=12+l
        if t==b'IDAT': idat+=c
    raw=zlib.decompress(idat)
    channels=4 if ct==6 else 3; stride=w*channels+1
    bright=0; total=0
    for row in range(0,h,3):
        base=row*stride+1
        for col in range(0,w*channels,channels*3):
            r=raw[base+col]; g=raw[base+col+1]; b=raw[base+col+2]
            if r>30 or g>30 or b>30: bright+=1
            total+=1
    mid=raw[(h//2)*stride+1+(w//2)*channels:]
    print(f'{label}: {w}x{h} bright={100*bright//total}% center=({mid[0]},{mid[1]},{mid[2]})')
analyze(r'C:\MirageWork\MirageVulkan\screenshots\bright_test.png', 'A9#2 after brightness=255')

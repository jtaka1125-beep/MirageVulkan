import subprocess, struct, zlib, os

devices = [
    ('192.168.0.6:5555', '6810568f', 'RebotAi A9 #1'),
    ('192.168.0.8:5555', '98875caa', 'RebotAi A9 #2 [MAIN]'),
    ('192.168.0.3:5555', 'f1925da3', 'N-one Npad X1'),
]

def analyze(path, label):
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
    print(f'  {label}: {w}x{h} bright={100*bright//total}% center=({mid[0]},{mid[1]},{mid[2]})')

for serial, tag, name in devices:
    print(f'{name}')
    remote = f'/sdcard/ss_{tag}.png'
    local = f'C:\\MirageWork\\MirageVulkan\\screenshots\\ss_{tag}.png'
    subprocess.run(f'adb -s {serial} shell screencap -p {remote}', shell=True, capture_output=True)
    r = subprocess.run(f'adb -s {serial} pull {remote} {local}', shell=True, capture_output=True)
    if os.path.exists(local):
        analyze(local, 'screen')
    else:
        print(f'  PULL FAILED: {r.stderr.decode()}')

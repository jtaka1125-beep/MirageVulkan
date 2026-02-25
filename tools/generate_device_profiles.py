import json, os, subprocess, re, time

OUT_DIR = r"C:\MirageWork\ops_snapshots\device_profiles"
os.makedirs(OUT_DIR, exist_ok=True)

# GUI video ports (fixed)
# AI ports are separate to avoid collision with GUI/H.264.
DEVICES = [
  {"name":"X1","adb":"192.168.0.3:5555","tcp_port":50100,"ai_port":51100},
  {"name":"A9_956","adb":"192.168.0.6:5555","tcp_port":50102,"ai_port":51102},
  {"name":"A9_479","adb":"192.168.0.8:5555","tcp_port":50104,"ai_port":51104},
]

def adb(adb_id, cmd):
    full = ['adb','-s',adb_id] + cmd
    return subprocess.check_output(full, stderr=subprocess.STDOUT, text=True, timeout=15)

def hwid(adb_id):
    aid = adb(adb_id, ['shell','settings','get','secure','android_id']).strip()
    if not aid or 'null' in aid or 'Error' in aid:
        return ''
    if len(aid) > 8:
        h=0
        for c in aid:
            h = (h*31 + ord(c)) & 0xffffffff
        return aid[:8] + '_' + str(h % 100000000)
    return aid

for d in DEVICES:
    adb_id=d['adb']
    wm_size = adb(adb_id,['shell','wm','size']).strip()
    wm_den = adb(adb_id,['shell','wm','density']).strip()

    j={
      'name': d['name'],
      'wifi_adb': adb_id,
      'tcp_port': d['tcp_port'],
      'ai_port': d['ai_port'],
      'hardware_id': hwid(adb_id),
      'model': adb(adb_id,['shell','getprop','ro.product.model']).strip(),
      'manufacturer': adb(adb_id,['shell','getprop','ro.product.manufacturer']).strip(),
      'brand': adb(adb_id,['shell','getprop','ro.product.brand']).strip(),
      'android_release': adb(adb_id,['shell','getprop','ro.build.version.release']).strip(),
      'sdk': adb(adb_id,['shell','getprop','ro.build.version.sdk']).strip(),
      'wm_size': wm_size,
      'wm_density': wm_den,
      'captured_at': time.strftime('%Y-%m-%dT%H:%M:%S')
    }

    fn=os.path.join(OUT_DIR, f"{d['name']}_{time.strftime('%Y%m%d_%H%M%S')}.json")
    with open(fn,'w',encoding='utf-8') as f:
        json.dump(j,f,ensure_ascii=False,indent=2)
    print('wrote',fn)

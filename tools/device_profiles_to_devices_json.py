import json, glob, os, re

SRC_DIR = r"C:\MirageWork\ops_snapshots\device_profiles"
OUT_PATH = r"C:\MirageWork\MirageVulkan\devices.json"

profiles = sorted(glob.glob(os.path.join(SRC_DIR, "*.json")), key=os.path.getmtime)
if not profiles:
    raise SystemExit(f"no profiles in {SRC_DIR}")

latest = {}
for p in profiles:
    try:
        j = json.load(open(p, 'r', encoding='utf-8'))
    except Exception:
        continue
    key = j.get('name') or j.get('usb_serial') or os.path.basename(p)
    latest[key] = j

out = {"devices": []}

for key, j in sorted(latest.items()):
    hw = (j.get('hardware_id') or '').strip()
    if not hw:
        continue

    # parse wm_size "Physical size: 1200x2000"
    w = h = 0
    m = re.search(r"(\d+)x(\d+)", j.get('wm_size', ''))
    if m:
        w = int(m.group(1)); h = int(m.group(2))

    den = 0
    m = re.search(r"(\d+)", j.get('wm_density', ''))
    if m:
        den = int(m.group(1))

    tcp_port = int(j.get('tcp_port') or 0)
    ai_port = int(j.get('ai_port') or 0)

    out['devices'].append({
        "hardware_id": hw,
        "screen_width": w,
        "screen_height": h,
        "screen_density": den,
        "tcp_port": tcp_port,
        "ai_port": ai_port,
        # AI native desired size (same as screen physical by default)
        "ai_width": int(j.get('ai_width') or w or 0),
        "ai_height": int(j.get('ai_height') or h or 0),
        "name": j.get('name', '')
    })

json.dump(out, open(OUT_PATH, 'w', encoding='utf-8'), ensure_ascii=False, indent=2)
print(f"wrote {OUT_PATH} ({len(out['devices'])} devices)")

import sys
sys.stdout.reconfigure(encoding='utf-8')

with open('backend/api.py', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Find "def screen_record" line inside AdbDevice
insert_idx = None
for i, line in enumerate(lines):
    if 'def screen_record(self, duration=10):' in line and i > 200:
        insert_idx = i
        break

if insert_idx is None:
    print("ERROR: screen_record not found")
    sys.exit(1)

# Build the tap_element method with matching indentation
patch = [
    "            def tap_element(self, resource_id):\n",
    "                nonlocal step_count; step_count += 1\n",
    "                result = api_self.dump_ui(serial)\n",
    "                if 'elements' in result:\n",
    "                    for e in result['elements']:\n",
    "                        if resource_id in (e.get('resource_id', '') or ''):\n",
    "                            cx, cy = e['center_x'], e['center_y']\n",
    "                            log_lines.append(f\"[ADB] tap_element('{resource_id}') -> tap({cx},{cy})\")\n",
    "                            subprocess.run(['adb', '-s', serial, 'shell', 'input', 'tap', str(cx), str(cy)],\n",
    "                                           capture_output=True, timeout=10)\n",
    "                            return True\n",
    "                log_lines.append(f\"[ADB] tap_element('{resource_id}') -> not found\")\n",
    "                return False\n",
]

lines[insert_idx:insert_idx] = patch

with open('backend/api.py', 'w', encoding='utf-8') as f:
    f.writelines(lines)

print(f"PATCHED: tap_element inserted at line {insert_idx+1}")

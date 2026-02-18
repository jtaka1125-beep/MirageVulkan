"""Macro Editor comprehensive validation test"""
import ast
import sys
import os
import re
import json

sys.path.insert(0, os.path.dirname(__file__))

errors = []
warnings = []
passes = 0

def ok(msg):
    global passes
    passes += 1
    print(f"  [PASS] {msg}")

def fail(msg):
    errors.append(msg)
    print(f"  [FAIL] {msg}")

def warn(msg):
    warnings.append(msg)
    print(f"  [WARN] {msg}")


# ================================================================
print("=" * 60)
print("MACRO EDITOR VALIDATION TEST")
print("=" * 60)

# 1. Python Syntax
print("\n--- 1. Python Syntax ---")
for f in ['app.py', 'backend/api.py', 'backend/mirage_client.py']:
    try:
        with open(f, encoding='utf-8') as fh:
            ast.parse(fh.read())
        ok(f)
    except SyntaxError as e:
        fail(f"{f}: {e}")

# 2. Import chain
print("\n--- 2. Import Chain ---")
try:
    from backend.mirage_client import (
        MirageClient, MacroRunner,
        dump_ui_hierarchy, find_text_on_screen, find_element_by_id,
        _DeviceProxy, _LogProxy, _MacroCancelled, _parse_bounds
    )
    ok("mirage_client: 9 symbols")
except Exception as e:
    fail(f"mirage_client import: {e}")

try:
    from backend.api import MacroEditorAPI, _run_adb
    from backend.api import _parse_bounds as api_parse_bounds
    ok("api: MacroEditorAPI + helpers")
except Exception as e:
    fail(f"api import: {e}")

# 3. API method completeness
print("\n--- 3. MacroEditorAPI Methods ---")
api = MacroEditorAPI()
expected_api = [
    'get_devices', 'capture_screen', 'run_macro', 'cancel_macro',
    'save_macro', 'load_macro', 'list_macros', 'ping',
    'dump_ui', 'find_text', 'get_clickables', 'capture_screen_with_elements'
]
pub = [m for m in dir(api) if not m.startswith('_')]
for m in expected_api:
    if m in pub:
        ok(f"api.{m}()")
    else:
        fail(f"api.{m}() MISSING")

# 4. DeviceProxy completeness
print("\n--- 4. _DeviceProxy Methods ---")
expected_proxy = [
    'tap', 'swipe', 'long_press', 'key', 'text',
    'launch_app', 'force_stop', 'screenshot',
    'screen_contains_text', 'find_and_tap_text',
    'wait_for_text', 'tap_element', 'screen_record'
]
prx = [m for m in dir(_DeviceProxy) if not m.startswith('_')]
for m in expected_proxy:
    if m in prx:
        ok(f"device.{m}()")
    else:
        fail(f"device.{m}() MISSING")

# 5. MirageClient RPC methods
print("\n--- 5. MirageClient RPC ---")
expected_rpc = [
    'connect', 'disconnect', 'ping', 'list_devices', 'device_info',
    'tap', 'swipe', 'long_press', 'key', 'text',
    'click_id', 'click_text', 'launch_app', 'force_stop', 'screenshot'
]
cli = dir(MirageClient)
for m in expected_rpc:
    if m in cli:
        ok(f"client.{m}()")
    else:
        fail(f"client.{m}() MISSING")

# 6. ADB fallback methods
print("\n--- 6. ADB Fallback (_run_macro_adb) ---")
with open('backend/api.py', encoding='utf-8') as f:
    api_src = f.read()
adb_expected = [
    'def tap(', 'def swipe(', 'def long_press(', 'def key(', 'def text(',
    'def launch_app(', 'def force_stop(', 'def screenshot(',
    'def screen_contains_text(', 'def find_and_tap_text(',
    'def wait_for_text(', 'def screen_record('
]
for sig in adb_expected:
    if sig in api_src:
        ok(f"AdbDevice.{sig.split('(')[0].replace('def ','')}")
    else:
        fail(f"AdbDevice.{sig} MISSING in api.py")

# 7. JavaScript file checks
print("\n--- 7. JavaScript Syntax & Content ---")
js_files = {
    'frontend/js/blocks/adb_touch.js': None,
    'frontend/js/generators/python_adb.js': None,
    'frontend/js/toolbox.js': None,
    'frontend/js/workspace.js': None,
    'frontend/index.html': None,
}
for f in js_files:
    if os.path.exists(f):
        with open(f, encoding='utf-8') as fh:
            js_files[f] = fh.read()
        ok(f"File exists: {f}")
    else:
        fail(f"File missing: {f}")

# 8. Block definitions vs generators
print("\n--- 8. Block-Generator Consistency ---")
blocks_src = js_files.get('frontend/js/blocks/adb_touch.js', '')
gen_src = js_files.get('frontend/js/generators/python_adb.js', '')
toolbox_src = js_files.get('frontend/js/toolbox.js', '')

# Extract block types from definitions
block_types = re.findall(r'"type":\s*"(adb_\w+)"', blocks_src)
block_types = list(dict.fromkeys(block_types))  # dedupe preserving order

# Extract generator registrations
gen_types = re.findall(r"python\.forBlock\['(adb_\w+)'\]", gen_src)

# Extract toolbox entries
toolbox_types = re.findall(r'"type":\s*"(adb_\w+)"', toolbox_src)

print(f"  Blocks defined: {len(block_types)}")
print(f"  Generators: {len(gen_types)}")
print(f"  Toolbox entries: {len(toolbox_types)}")

for bt in block_types:
    if bt in gen_types:
        ok(f"Block {bt} has generator")
    else:
        fail(f"Block {bt} MISSING generator")

for bt in block_types:
    if bt in toolbox_types:
        ok(f"Block {bt} in toolbox")
    else:
        fail(f"Block {bt} MISSING from toolbox")

# Check for generators without block definitions
for gt in gen_types:
    if gt not in block_types:
        warn(f"Generator {gt} has no block definition")

# 9. workspace.js function checks
print("\n--- 9. Workspace Functions ---")
ws_src = js_files.get('frontend/js/workspace.js', '')
expected_funcs = [
    'refreshDevices', 'runMacro', 'stopMacro', 'saveMacro', 'loadMacro',
    'exportCode', 'registerContextMenu', 'executeBlockLive',
    'executeFromBlockLive', 'generateCodeForBlock', 'flashBlock',
    'startDeviceMonitor', 'updateConnectionStatus', 'showRunLog',
    'startRecording', 'stopRecording', 'showScreenPicker',
    'openPickerModal', 'handlePickerClick', 'createContainerFromRecording'
]
for fn in expected_funcs:
    if f'function {fn}' in ws_src or f'async function {fn}' in ws_src:
        ok(f"workspace.{fn}()")
    else:
        fail(f"workspace.{fn}() MISSING")

# 10. HTML integrity
print("\n--- 10. HTML Integrity ---")
html_src = js_files.get('frontend/index.html', '')
required_elements = [
    'id="blocklyDiv"', 'id="device-select"', 'id="code-output"',
    'id="toolbar-buttons"', 'id="btn-run"',
    'adb_touch.js', 'python_adb.js', 'toolbox.js', 'workspace.js',
    'blockly.min.js'
]
for elem in required_elements:
    if elem in html_src:
        ok(f"HTML: {elem}")
    else:
        fail(f"HTML: {elem} MISSING")

# 11. Cross-reference: Blockly block types used in workspace.js
print("\n--- 11. Cross-References ---")
if 'adb_container' in ws_src:
    ok("workspace references adb_container for recording")
else:
    fail("workspace missing adb_container reference")

if 'adb_tap' in ws_src:
    ok("workspace references adb_tap for recording")
else:
    fail("workspace missing adb_tap reference")

if 'adb_swipe' in ws_src:
    ok("workspace references adb_swipe for recording")
else:
    fail("workspace missing adb_swipe reference")

if 'adb_long_press' in ws_src:
    ok("workspace references adb_long_press for recording")
else:
    fail("workspace missing adb_long_press reference")

# 12. _parse_bounds validation
print("\n--- 12. Utility Functions ---")
from backend.api import _parse_bounds as pb
assert pb('[0,0][1080,1920]') == (0, 0, 1080, 1920), "parse_bounds basic"
ok("_parse_bounds('[0,0][1080,1920]')")
assert pb('[100,200][300,400]') == (100, 200, 300, 400), "parse_bounds mid"
ok("_parse_bounds('[100,200][300,400]')")
assert pb('invalid') is None, "parse_bounds invalid"
ok("_parse_bounds('invalid') -> None")
assert pb('') is None, "parse_bounds empty"
ok("_parse_bounds('') -> None")
assert pb(None) is None, "parse_bounds None"
ok("_parse_bounds(None) -> None")

# 13. C++ server file check
print("\n--- 13. C++ MacroApiServer ---")
cpp_hpp = '../src/macro_api_server.hpp'
cpp_cpp = '../src/macro_api_server.cpp'
for f in [cpp_hpp, cpp_cpp]:
    if os.path.exists(f):
        with open(f, encoding='utf-8') as fh:
            content = fh.read()
        ok(f"C++ file: {os.path.basename(f)} ({len(content)} bytes)")
    else:
        warn(f"C++ file not found: {f} (may be in different path)")

# Check C++ server has required handlers
if os.path.exists(cpp_cpp):
    with open(cpp_cpp, encoding='utf-8') as fh:
        cpp_src = fh.read()
    cpp_handlers = ['handle_tap', 'handle_swipe', 'handle_long_press', 'handle_key',
                    'handle_text', 'handle_screenshot', 'handle_launch_app',
                    'handle_force_stop', 'handle_click_id', 'handle_click_text']
    for h in cpp_handlers:
        if h in cpp_src:
            ok(f"C++ handler: {h}")
        else:
            fail(f"C++ handler: {h} MISSING")


# ================================================================
print("\n" + "=" * 60)
print(f"RESULTS: {passes} passed, {len(errors)} errors, {len(warnings)} warnings")
print("=" * 60)
if errors:
    print("\nERRORS:")
    for e in errors:
        print(f"  * {e}")
if warnings:
    print("\nWARNINGS:")
    for w in warnings:
        print(f"  * {w}")

#!/usr/bin/env python3
"""Patch index.html to include OCR JS files."""
with open('frontend/index.html', 'r', encoding='utf-8') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    new_lines.append(line)
    if 'adb_touch.js' in line:
        new_lines.append('    <script src="js/blocks/ocr_blocks.js"></script>\n')
    if 'python_adb.js' in line:
        new_lines.append('    <script src="js/generators/python_ocr.js"></script>\n')

with open('frontend/index.html', 'w', encoding='utf-8') as f:
    f.writelines(new_lines)
print("OK")

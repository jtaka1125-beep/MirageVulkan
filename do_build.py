import subprocess, sys, os
result = subprocess.run(
    ['cmake', '--build', '.', '--', '-j2'],
    cwd=r'C:\MirageWork\MirageVulkanuild',
    capture_output=True, text=True, timeout=300
)
combined = result.stdout + result.stderr
lines = combined.splitlines()
output_lines = []
output_lines.append('TOTAL_LINES=' + str(len(lines)))
output_lines.append('RETURN_CODE=' + str(result.returncode))
output_lines.append('---LAST 20 LINES---')
for l in lines[-20:]:
    output_lines.append(l)
error_lines = [l for l in lines if 'error:' in l.lower() or 'Error' in l]
if error_lines:
    output_lines.append('---ERRORS---')
    for l in error_lines[:10]:
        output_lines.append(l)
with open(r'C:\MirageWork\MirageVulkanuild_result.txt', 'w') as f:
    f.write(chr(10).join(output_lines) + chr(10))

import os

targets = ['ScreenAnalyzer','OcrEngine','UiDetector','TiledEncoder','TileRepeater','MtilFraming']
for root, dirs, fs in os.walk(r'C:\MirageWork\MirageVulkan\android\capture\src'):
    for f in fs:
        if any(f.startswith(x) for x in targets):
            p = os.path.join(root, f)
            print(f'=== {f} ({os.path.getsize(p)}B) ===')
            with open(p,'rb') as fh:
                lines = fh.read().decode('utf-8','replace').splitlines()
            for l in lines[:55]: print(l)
            if len(lines)>55: print(f'  ... ({len(lines)} lines total)')
            print()

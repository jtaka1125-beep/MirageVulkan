from pathlib import Path
p = Path(r'C:\MirageWork\MirageVulkan\docs\macro_editor_design.md')
text = p.read_text(encoding='utf-8', errors='replace')
section = r'''

---

## 8. Coordinate Basis Policy (2026-03 update)

### 8.1 Current implementation policy

Macro Editor now follows the same live frame path as MirageGUI as much as possible.

- Screenshot / preview image:
  - Prefer GUI-side `FrameReadyEvent` / JPEG cache generated from the live mirror path
  - Avoid separate ADB screencap when preview cache is already available
- Touch mapping (`tap`, `swipe`, `long_press`, `multi_touch`, `pinch`):
  - Prefer preview/cache dimensions first
  - Fallback to native screen dimensions only when preview dimensions are unavailable
- API responses now expose both preview and native dimensions so the editor can decide the correct basis explicitly

### 8.2 Recommended editor rule

Use the following rule consistently:

1. **Editing / point picking**
   - Use `preview_w` / `preview_h` when available
   - This matches what the user is actually seeing in the editor preview

2. **Persistent storage**
   - Prefer storing coordinates in **native-normalized form**
   - Recommended fields:
     - `x_norm = x / basis_w`
     - `y_norm = y / basis_h`
     - `basis = "native_normalized"`
   - This makes macros more portable across preview scaling changes

3. **Execution**
   - If preview dimensions are available, convert editor coordinates using preview basis
   - If loading a normalized macro, reconstruct runtime coordinates from current basis dimensions
   - Final command path should continue to use the same MirageGUI touch route policy

### 8.3 Suggested JSON shape for saved macros

```json
{
  "coord_basis": "native_normalized",
  "points": {
    "tap_login": {
      "x_norm": 0.5000,
      "y_norm": 0.1500
    }
  },
  "meta": {
    "recorded_native_w": 1200,
    "recorded_native_h": 2000,
    "recorded_preview_w": 1080,
    "recorded_preview_h": 1800
  }
}
```

### 8.4 API contract expected by Macro Editor

`device_info` should be interpreted as:

- `native_w`, `native_h`: device native screen size
- `preview_w`, `preview_h`: current live preview/cache size if available
- `coord_space`: current recommended live editing space (`preview` or `native`)
- `coord_basis`: hint string for the editor

`screenshot` should be interpreted as:

- returned `width`, `height`: the image basis the editor should use for direct point picking
- `native_w`, `native_h`: reference dimensions for normalization / persistence

### 8.5 Practical conclusion

- **Live editing:** preview basis
- **Saved macro:** native-normalized basis
- **Runtime execution:** recalculate from current dimensions, then send via MirageGUI command path

This keeps the editor intuitive while making stored macros resilient to preview scaling changes.
'''
if '## 8. Coordinate Basis Policy (2026-03 update)' not in text:
    text += section
p.write_text(text, encoding='utf-8')
print('appended')

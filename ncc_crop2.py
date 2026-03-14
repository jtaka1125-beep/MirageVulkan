from PIL import Image
import numpy as np

# 1200x2000 スクリーンショットを 1080x1800 にリサイズしてからクロップ
img_path = r'C:\MirageWork\MirageVulkan\screenshots\ss_192_168_0_3_5555_20260313_171830.png'
img = Image.open(img_path)
print('Original size:', img.size)

# 1080x1800 にリサイズ（GUIデコードフレームと同じサイズ）
img_resized = img.resize((1080, 1800), Image.LANCZOS)
print('Resized size:', img_resized.size)

# Xボタン: 15%, 18% -> 中心
w, h = img_resized.size
cx = int(w * 0.15)
cy = int(h * 0.18)
print('X button center (1080x1800):', cx, cy)

# 80x80 グレースケールクロップ
margin = 40
box = (max(0, cx-margin), max(0, cy-margin), min(w, cx+margin), min(h, cy+margin))
crop = img_resized.crop(box).convert('L')
print('Crop size:', crop.size)

# 保存
out_path = r'C:\MirageWork\MirageVulkan\templates\auto_popup\x_button_crop.png'
crop.save(out_path)
print('Saved:', out_path)

# NCC自己検証
img_gray = np.array(img_resized.convert('L'), dtype=np.float32)
tmpl = np.array(crop, dtype=np.float32)
x0, y0 = max(0, cx-margin), max(0, cy-margin)
roi = img_gray[y0:y0+tmpl.shape[0], x0:x0+tmpl.shape[1]]
t_norm = tmpl - tmpl.mean()
r_norm = roi - roi.mean()
denom = float(np.sqrt((t_norm**2).sum() * (r_norm**2).sum()))
if denom > 0:
    ncc = float((t_norm * r_norm).sum()) / denom
    print('NCC (self-check):', ncc)
else:
    print('flat region')

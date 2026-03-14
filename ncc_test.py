from PIL import Image
import numpy as np

img = np.array(Image.open(r'C:\MirageWork\MirageVulkan\screenshots\ss_192_168_0_3_5555_20260313_170606.png').convert('L'), dtype=np.float32)
tmpl = np.array(Image.open(r'C:\MirageWork\MirageVulkan\templates\auto_popup\x_button_crop.png').convert('L'), dtype=np.float32)

print('Screen:', img.shape, 'Template:', tmpl.shape)
tw = tmpl.shape[1]
th = tmpl.shape[0]
cx = 180
cy = 380
x0 = cx - 40
y0 = cy - 40
roi = img[y0:y0+th, x0:x0+tw]
t_norm = tmpl - tmpl.mean()
r_norm = roi - roi.mean()
denom = float(np.sqrt((t_norm**2).sum() * (r_norm**2).sum()))
if denom > 0:
    ncc = float((t_norm * r_norm).sum()) / denom
    print('NCC:', ncc)
else:
    print('flat region')

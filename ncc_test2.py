from PIL import Image
import numpy as np

img_path = r'C:\Users\jun\AppData\Local\Temp\popup_detect_192.168.0.3_5555.png'
tmpl_path = r'C:\MirageWork\MirageVulkan\templates\auto_popup\x_button_crop.png'
orig_path = r'C:\MirageWork\MirageVulkan\screenshots\ss_192_168_0_3_5555_20260313_170606.png'

img = np.array(Image.open(img_path).convert('L'), dtype=np.float32)
tmpl = np.array(Image.open(tmpl_path).convert('L'), dtype=np.float32)

h_img, w_img = img.shape
print('popup_detect image:', w_img, 'x', h_img)
print('template:', tmpl.shape[1], 'x', tmpl.shape[0])

cx = int(w_img * 0.15)
cy = int(h_img * 0.18)
x0 = cx - 40
y0 = cy - 40
roi = img[y0:y0+80, x0:x0+80]
t_mean = tmpl.mean()
r_mean = roi.mean()
t_norm = tmpl - t_mean
r_norm = roi - r_mean
denom = float(np.sqrt((t_norm**2).sum() * (r_norm**2).sum()))
if denom > 0:
    ncc = float((t_norm * r_norm).sum()) / denom
    print('NCC at', x0, y0, ':', ncc)
else:
    print('flat region at', x0, y0)

orig = np.array(Image.open(orig_path).convert('L'), dtype=np.float32)
print('Original screenshot:', orig.shape[1], 'x', orig.shape[0])

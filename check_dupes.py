
# 重複定義チェック
import re

p_hpp = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.hpp'
with open(p_hpp,'rb') as f: hpp = f.read().decode('utf-8')

p_cpp = r'C:\MirageWork\MirageVulkan\src\usb_command_api.cpp'
with open(p_cpp,'rb') as f: cpp = f.read().decode('utf-8')

p_proto = r'C:\MirageWork\MirageVulkan\src\mirage_protocol.hpp'
with open(p_proto,'rb') as f: proto = f.read().decode('utf-8')

print("=== mirage_protocol.hpp: PINCH/LONGPRESS ===")
for l in proto.splitlines():
    if 'PINCH' in l or 'LONGPRESS' in l: print(f"  {l}")

print("\n=== multi_usb_command_sender.hpp: send_pinch/send_longpress ===")
for l in hpp.splitlines():
    if 'send_pinch' in l or 'send_longpress' in l: print(f"  {l}")

print("\n=== usb_command_api.cpp: send_pinch/send_click_id/send_click_text ===")
for l in cpp.splitlines():
    if 'send_pinch' in l or 'send_longpress' in l or 'send_click_id' in l or 'send_click_text' in l:
        print(f"  {l}")

# Count occurrences of CMD_PINCH in protocol.hpp
cnt = proto.count('CMD_PINCH')
print(f"\nCMD_PINCH occurrences in mirage_protocol.hpp: {cnt}")
cnt2 = hpp.count('send_pinch')
print(f"send_pinch occurrences in hpp: {cnt2}")

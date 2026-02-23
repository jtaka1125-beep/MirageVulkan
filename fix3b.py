h264_path = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt'
with open(h264_path,'rb') as f:
    h264 = f.read().decode('utf-8')

old = 'for (pkt in packets) { sender.send(pkt) }\r\n                if (sender is UsbVideoSender) { sender.flushBatch() }\r\n                for (sec in secondarySenders) {\r\n                    try { for (pkt in packets) { sec.send(pkt) } }\r\n                    catch (e: Exception) {'
new = 'for (pkt in packets) { sender.send(pkt) }\r\n                sender.flush() // FIX-3: interface経由、instanceof不要\r\n                for (sec in secondarySenders) {\r\n                    try { for (pkt in packets) { sec.send(pkt) }; sec.flush() }\r\n                    catch (e: Exception) {'

if old in h264:
    h264 = h264.replace(old, new)
    with open(h264_path,'wb') as f:
        f.write(h264.encode('utf-8'))
    print("H264Encoder instanceof removal: OK")
else:
    print("ERROR: pattern not found")
    # LFで試す
    old_lf = old.replace('\r\n','\n')
    if old_lf in h264:
        h264 = h264.replace(old_lf, new.replace('\r\n','\n'))
        with open(h264_path,'wb') as f:
            f.write(h264.encode('utf-8'))
        print("H264Encoder instanceof removal: OK (LF)")
    else:
        print("Still not found. Check context.")


import sys
p = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.hpp'
with open(p,'rb') as f: raw=f.read()
h = raw.decode('utf-8')
h_lf = h.replace('\r\n','\n')

# show bytes around recv_thread
idx = h_lf.find('recv_thread')
sys.stderr.write(f'idx={idx}\n')
sys.stderr.write(repr(h_lf[max(0,idx-5):idx+120])+'\n')

old_dh = (
    '        std::thread recv_thread;  // Per-device receive thread\n'
    '        std::atomic<bool> recv_running{false};\n'
)
sys.stderr.write(f'old_dh in h_lf: {old_dh in h_lf}\n')
sys.stderr.write('old_dh repr: '+repr(old_dh)+'\n')

idx2 = h_lf.find('std::thread recv_thread')
sys.stderr.write(repr(h_lf[idx2:idx2+100])+'\n')

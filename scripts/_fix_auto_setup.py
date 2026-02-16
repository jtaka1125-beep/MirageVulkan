path = r"C:\MirageWork\MirageVulkan\src\auto_setup.hpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

c = c.replace('#include "logger.hpp"', '#include "mirage_log.hpp"')
c = c.replace('LOG_INFO(', 'MLOG_INFO(')
c = c.replace('LOG_ERROR(', 'MLOG_ERROR(')
c = c.replace('LOG_WARN(', 'MLOG_WARN(')

# Fix fmt {} to printf %
replacements = [
    ('"scrcpy: scid={} tcp={} udp={}"', '"scrcpy: scid=%s tcp=%d udp=%d"'),
    ('"scrcpy server exited: {}"', '"scrcpy server exited: %s"'),
    ('"Bridge thread starting: TCP:{} -> UDP:{}"', '"Bridge thread starting: TCP:%d -> UDP:%d"'),
    ('"Bridge: TCP connected to scrcpy on port {}"', '"Bridge: TCP connected to scrcpy on port %d"'),
    ('"Bridge: TCP recv returned {}"', '"Bridge: TCP recv returned %d"'),
    ('"Bridge: {:.1f}s {:.2f} Mbps"', '"Bridge: %.1fs %.2f Mbps"'),
    ('"Bridge thread ended (total {} bytes)"', '"Bridge thread ended (total %lld bytes)"'),
]
for old, new in replacements:
    c = c.replace(old, new)

# Fix out to out.c_str() for server exit log
c = c.replace('scrcpy server exited: %s", out)', 'scrcpy server exited: %s", out.c_str())')

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)
print("FIXED")

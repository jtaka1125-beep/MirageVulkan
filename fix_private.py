#!/usr/bin/env python3
"""Move process_raw_h264 from private to public in mirror_receiver.hpp"""

filepath = r'C:\MirageWork\MirageVulkan\src\mirror_receiver.hpp'

with open(filepath, 'r', encoding='utf-8-sig') as f:
    content = f.read()

# Add process_raw_h264 declaration to public section (after feed_rtp_packet)
old_public = '  // Feed RTP packet from external source (e.g., USB AOA)\n  void feed_rtp_packet(const uint8_t* data, size_t len);'
new_public = '  // Feed RTP packet from external source (e.g., USB AOA)\n  void feed_rtp_packet(const uint8_t* data, size_t len);\n\n  // Feed raw H.264 Annex B data from external source (e.g., scrcpy TCP)\n  void process_raw_h264(const uint8_t* data, size_t len);'

assert old_public in content, "public section not found!"
content = content.replace(old_public, new_public)

# Remove from private section
old_private = '  void process_raw_h264(const uint8_t* data, size_t len);\n'
# There are now 2 occurrences; remove the one in private (second occurrence)
first_idx = content.find(old_private)
second_idx = content.find(old_private, first_idx + 1)
if second_idx >= 0:
    content = content[:second_idx] + content[second_idx + len(old_private):]
    print("Removed process_raw_h264 from private section")

with open(filepath, 'w', encoding='utf-8-sig') as f:
    f.write(content)

print("Done: process_raw_h264 moved to public")

"""
Fix MirrorReceiver to handle raw H.264 Annex B packets from scrcpy bridge.

The bridge sends TCP chunks as-is via UDP (1400-byte chunks).
These are NOT RTP packets - they're raw H.264 Annex B with start codes.
We need to accumulate chunks and extract NAL units at start code boundaries.
"""
path = r"C:\MirageWork\MirageVulkan\src\mirror_receiver.cpp"
with open(path, 'r', encoding='utf-8') as f:
    c = f.read()

# Replace the process_rtp_packet function to handle both RTP and raw H.264
old_validate = '''  // Validate RTP version (must be 2)
  uint8_t version = (data[0] >> 6) & 0x03;
  if (version != 2) {
    // Not a valid RTP packet, silently ignore
    return;
  }'''

new_validate = '''  // Check if this is raw H.264 Annex B (from scrcpy bridge) or RTP
  // Raw H.264 starts with 00 00 00 01 or 00 00 01, RTP has version=2 in bits 7-6
  uint8_t version = (data[0] >> 6) & 0x03;
  if (version != 2) {
    // Not RTP - treat as raw H.264 Annex B stream chunk
    process_raw_h264(data, len);
    return;
  }'''

c = c.replace(old_validate, new_validate)

# Add the raw H.264 processor function before process_rtp_packet
# Find the position to insert
insert_marker = "void MirrorReceiver::process_rtp_packet(const uint8_t* data, size_t len) {"

raw_h264_func = '''// ==============================================================================
// Raw H.264 Annex B stream processing (for scrcpy raw_stream=true)
// Accumulates UDP chunks and extracts NAL units at start code boundaries
// ==============================================================================
void MirrorReceiver::process_raw_h264(const uint8_t* data, size_t len) {
  // Append to accumulation buffer
  raw_h264_buf_.insert(raw_h264_buf_.end(), data, data + len);
  bytes_received_.fetch_add(len);

  // Extract complete NAL units (delimited by 00 00 00 01)
  while (raw_h264_buf_.size() >= 4) {
    // Find first start code
    size_t first_sc = find_start_code(raw_h264_buf_.data(), raw_h264_buf_.size(), 0);
    if (first_sc == (size_t)-1) {
      // No start code found - discard accumulated junk
      raw_h264_buf_.clear();
      return;
    }
    if (first_sc > 0) {
      // Discard bytes before first start code
      raw_h264_buf_.erase(raw_h264_buf_.begin(), raw_h264_buf_.begin() + first_sc);
    }

    // Determine start code length (3 or 4 bytes)
    size_t sc_len = (raw_h264_buf_.size() >= 4 &&
                     raw_h264_buf_[0] == 0 && raw_h264_buf_[1] == 0 &&
                     raw_h264_buf_[2] == 0 && raw_h264_buf_[3] == 1) ? 4 : 3;

    // Find next start code (end of current NAL)
    size_t next_sc = find_start_code(raw_h264_buf_.data(), raw_h264_buf_.size(), sc_len);
    if (next_sc == (size_t)-1) {
      // No second start code - NAL is incomplete, wait for more data
      // Safety: if buffer is huge (>1MB), something is wrong - flush it
      if (raw_h264_buf_.size() > 1024 * 1024) {
        MLOG_WARN("mirror", "Raw H.264 buffer overflow (%zu bytes), flushing", raw_h264_buf_.size());
        raw_h264_buf_.clear();
      }
      return;
    }

    // Extract NAL unit (without start code prefix)
    const uint8_t* nal_data = raw_h264_buf_.data() + sc_len;
    size_t nal_len = next_sc - sc_len;
    if (nal_len > 0) {
      packets_received_.fetch_add(1);
      enqueue_nal(nal_data, nal_len);
    }

    // Remove processed NAL from buffer
    raw_h264_buf_.erase(raw_h264_buf_.begin(), raw_h264_buf_.begin() + next_sc);
  }
}

// Find H.264 Annex B start code (00 00 00 01 or 00 00 01) starting from offset
size_t MirrorReceiver::find_start_code(const uint8_t* data, size_t len, size_t offset) {
  for (size_t i = offset; i + 3 <= len; i++) {
    if (data[i] == 0 && data[i+1] == 0) {
      if (i + 3 < len && data[i+2] == 0 && data[i+3] == 1) return i;  // 00 00 00 01
      if (data[i+2] == 1) return i;  // 00 00 01
    }
  }
  return (size_t)-1;
}

'''

c = c.replace(insert_marker, raw_h264_func + insert_marker)

with open(path, 'w', encoding='utf-8') as f:
    f.write(c)

# Add declarations to header
hpp_path = r"C:\MirageWork\MirageVulkan\src\mirror_receiver.hpp"
with open(hpp_path, 'r', encoding='utf-8') as f:
    h = f.read()

# Add raw_h264_buf_ member and method declarations
if 'raw_h264_buf_' not in h:
    # Add method declarations near process_rtp_packet
    h = h.replace(
        '  void process_rtp_packet(const uint8_t* data, size_t len);',
        '  void process_rtp_packet(const uint8_t* data, size_t len);\n'
        '  void process_raw_h264(const uint8_t* data, size_t len);\n'
        '  size_t find_start_code(const uint8_t* data, size_t len, size_t offset);'
    )
    # Add buffer member
    h = h.replace(
        '  // Reusable annexb buffer for decode_nal (avoids per-NAL heap allocation)',
        '  // Raw H.264 Annex B accumulation buffer (for scrcpy raw_stream=true)\n'
        '  std::vector<uint8_t> raw_h264_buf_;\n\n'
        '  // Reusable annexb buffer for decode_nal (avoids per-NAL heap allocation)'
    )

with open(hpp_path, 'w', encoding='utf-8') as f:
    f.write(h)

print("DONE: Added raw H.264 Annex B support to MirrorReceiver")

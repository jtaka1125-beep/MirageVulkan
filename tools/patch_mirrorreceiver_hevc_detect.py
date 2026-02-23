from pathlib import Path
import re

hpp = Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.hpp")
t = hpp.read_text(encoding='utf-8', errors='ignore')
if 'bool stream_is_hevc_' not in t:
    t = t.replace('  // SPS/PPS validation gate', '  // HEVC detection\n  bool stream_is_hevc_ = false;\n\n  // SPS/PPS validation gate', 1)
hpp.write_text(t, encoding='utf-8')

cpp = Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.cpp")
t = cpp.read_text(encoding='utf-8', errors='ignore')

# helper detect in decode_nal: after start code skip
if 'HEVC VPS/SPS detected' not in t:
    # find start of decode_nal
    m = re.search(r"void MirrorReceiver::decode_nal\(const uint8_t\* data, size_t len\) \{", t)
    if not m:
        raise SystemExit('decode_nal not found')
    start = m.end()
    # insert after initial checks in decode_nal (we locate first occurrence of 'if (!data || len == 0) return;')
    ins_pos = t.find('if (!data || len == 0)', start)
    ins_pos = t.find('\n', ins_pos)+1
    detect_block = r'''
  // --- Auto-detect HEVC by NAL unit type (VPS/SPS/PPS are 32/33/34 in HEVC) ---
  // H.264 SPS is nal_type=7 (byte0 & 0x1f). HEVC nal_type = (byte0 >> 1) & 0x3f.
  // When HEVC is detected, re-init decoder backend for HEVC (FFmpeg).
  {
    size_t off = 0;
    // skip start code 00 00 01 or 00 00 00 01
    while (off + 3 < len && data[off] == 0x00) off++;
    if (off < len && data[off] == 0x01) off++;
    if (off < len) {
      uint8_t b0 = data[off];
      int hevc_type = (b0 >> 1) & 0x3f;
      if (!stream_is_hevc_ && (hevc_type == 32 || hevc_type == 33 || hevc_type == 34)) {
        stream_is_hevc_ = true;
        MLOG_INFO("mirror", "HEVC VPS/SPS detected (nal_type=%d) - switching decoder to HEVC", hevc_type);
        // Re-init unified decoder for HEVC
        if (unified_decoder_) {
          unified_decoder_.reset();
        }
        init_decoder();
        // Bypass H.264 SPS gate
        has_valid_sps_ = true;
      }
    }
  }
'''
    t = t[:ins_pos] + detect_block + t[ins_pos:]

# In init_decoder(), set config.codec based on stream_is_hevc_
if 'config.codec' not in t:
    # locate UnifiedDecoderConfig config; line
    t = t.replace('mirage::video::UnifiedDecoderConfig config;', 'mirage::video::UnifiedDecoderConfig config;\n  config.codec = stream_is_hevc_ ? mirage::video::VideoCodec::HEVC : mirage::video::VideoCodec::H264;')

cpp.write_text(t, encoding='utf-8')
print('patched MirrorReceiver HEVC detect + config')

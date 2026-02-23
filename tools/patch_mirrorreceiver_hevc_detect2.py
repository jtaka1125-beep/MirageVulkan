from pathlib import Path
import re

# patch header
hpp=Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.hpp")
h=hpp.read_text(encoding='utf-8', errors='ignore')
if 'bool stream_is_hevc_' not in h:
    h=h.replace('  // SPS/PPS cache for stream recovery', '  // HEVC detection\n  bool stream_is_hevc_ = false;\n\n  // SPS/PPS cache for stream recovery', 1)
hpp.write_text(h, encoding='utf-8')

cpp=Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.cpp")
t=cpp.read_text(encoding='utf-8', errors='ignore')

# ensure init_decoder sets codec
if 'config.codec' not in t:
    t=t.replace('mirage::video::UnifiedDecoderConfig config;', 'mirage::video::UnifiedDecoderConfig config;\n  config.codec = stream_is_hevc_ ? mirage::video::VideoCodec::HEVC : mirage::video::VideoCodec::H264;')

# insert detection into decode_nal right after function begins and initial null checks
m=re.search(r"void MirrorReceiver::decode_nal\(const uint8_t\* data, size_t len\) \{", t)
if not m:
    raise SystemExit('decode_nal not found')
fn_start=m.end()
# find after first guard return block within function
# we find the first occurrence of 'if (!data || len == 0) return;' after fn_start
pos=t.find('if (!data || len == 0)', fn_start)
if pos==-1:
    raise SystemExit('guard not found')
line_end=t.find('\n', pos)
insert_at=line_end+1

if 'Auto-detect HEVC' not in t[fn_start:fn_start+800]:
    detect=r'''

  // --- Auto-detect HEVC by NAL unit type (VPS/SPS/PPS are 32/33/34 in HEVC) ---
  // H.264 SPS is nal_type=7 (byte0 & 0x1f). HEVC nal_type = (byte0 >> 1) & 0x3f.
  // When HEVC is detected, switch decoder backend to HEVC (FFmpeg).
  if (!stream_is_hevc_ && data && len > 6) {
    size_t off = 0;
    // skip start code 00 00 01 or 00 00 00 01
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00) {
      if (data[2] == 0x01) off = 3;
      else if (data[2] == 0x00 && data[3] == 0x01) off = 4;
    }
    if (off < len) {
      uint8_t b0 = data[off];
      int hevc_type = (b0 >> 1) & 0x3f;
      if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34) {
        stream_is_hevc_ = true;
        MLOG_INFO("mirror", "HEVC VPS/SPS detected (nal_type=%d) - switching decoder to HEVC", hevc_type);
        unified_decoder_.reset();
        init_decoder();
        has_valid_sps_ = true; // bypass H.264 SPS gate
      }
    }
  }
'''
    t=t[:insert_at]+detect+t[insert_at:]

cpp.write_text(t, encoding='utf-8')
print('patched MirrorReceiver HEVC detect v2')

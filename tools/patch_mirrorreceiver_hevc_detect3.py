from pathlib import Path
import re

# header add flag
hpp=Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.hpp")
h=hpp.read_text(encoding='utf-8', errors='ignore')
if 'bool stream_is_hevc_' not in h:
    h=h.replace('  // SPS/PPS cache for stream recovery', '  // HEVC detection\n  bool stream_is_hevc_ = false;\n\n  // SPS/PPS cache for stream recovery', 1)
hpp.write_text(h, encoding='utf-8')

cpp=Path(r"C:/MirageWork/MirageVulkan/src/mirror_receiver.cpp")
t=cpp.read_text(encoding='utf-8', errors='ignore')

# set config.codec
if 'config.codec' not in t:
    t=t.replace('mirage::video::UnifiedDecoderConfig config;', 'mirage::video::UnifiedDecoderConfig config;\n    config.codec = stream_is_hevc_ ? mirage::video::VideoCodec::HEVC : mirage::video::VideoCodec::H264;')

# insert detect after len<1 check in decode_nal
m=re.search(r"void MirrorReceiver::decode_nal\(const uint8_t\* data, size_t len\) \{\n\s*if \(len < 1\) return;", t)
if not m:
    raise SystemExit('decode_nal len check not found')
insert_at=m.end()
if 'HEVC VPS/SPS detected' not in t[insert_at:insert_at+600]:
    detect=r'''

  // Auto-detect HEVC by NAL unit type (VPS/SPS/PPS are 32/33/34 in HEVC)
  // HEVC nal_type = (byte0 >> 1) & 0x3f
  if (!stream_is_hevc_ && len >= 2) {
    int hevc_type = (data[0] >> 1) & 0x3f;
    if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34) {
      stream_is_hevc_ = true;
      MLOG_INFO("mirror", "HEVC VPS/SPS detected (nal_type=%d) - switching decoder to HEVC", hevc_type);
      unified_decoder_.reset();
      init_decoder();
      has_valid_sps_ = true; // bypass H.264 SPS gate
    }
  }
'''
    t=t[:insert_at]+detect+t[insert_at:]

cpp.write_text(t, encoding='utf-8')
print('patched mirror_receiver HEVC detect v3')

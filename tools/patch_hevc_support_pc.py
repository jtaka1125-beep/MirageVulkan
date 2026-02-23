from pathlib import Path
import re

# 1) unified_decoder.hpp: add codec enum and field
uh = Path(r"C:/MirageWork/MirageVulkan/src/video/unified_decoder.hpp")
t = uh.read_text(encoding='utf-8', errors='ignore')
if 'enum class VideoCodec' not in t:
    ins = '\n// Video codec type\nenum class VideoCodec {\n    H264,\n    HEVC,\n};\n'
    # insert after DecoderBackend enum
    pos = t.find('enum class DecoderBackend')
    pos = t.find('};', pos)
    pos = t.find('\n', pos)+1
    t = t[:pos] + ins + t[pos:]

# add field to UnifiedDecoderConfig
if 'VideoCodec codec' not in t:
    t = t.replace('uint32_t dpb_slot_count = 8;\n', 'uint32_t dpb_slot_count = 8;\n\n    VideoCodec codec = VideoCodec::H264;\n')

uh.write_text(t, encoding='utf-8')

# 2) unified_decoder.cpp: make FFmpegDecoder::init accept codec
uc = Path(r"C:/MirageWork/MirageVulkan/src/video/unified_decoder.cpp")
t = uc.read_text(encoding='utf-8', errors='ignore')
# change signature
if 'bool init()' in t and 'bool init(VideoCodec codec)' not in t:
    t = t.replace('    bool init() {', '    bool init(VideoCodec codec) {', 1)
    # change call to decoder->init()
    t = t.replace('        return decoder->init();', '        return decoder->init(codec == VideoCodec::HEVC);', 1)

# in initializeFFmpeg call
if 'ffmpeg_decoder_->init()' in t:
    t = t.replace('ffmpeg_decoder_->init()', 'ffmpeg_decoder_->init(config_.codec)')

# in initialize(), skip VulkanVideo when HEVC
if 'if (config_.prefer_vulkan_video' in t and 'config_.codec == VideoCodec::HEVC' not in t:
    t = t.replace('    // Try Vulkan Video first if configured and available',
                  '    // Try Vulkan Video first if configured and available\n    // NOTE: HEVC currently uses FFmpeg backend (Vulkan Video path is H.264-only here).')
    t = t.replace('    if (config_.prefer_vulkan_video &&',
                  '    if (config_.codec != VideoCodec::HEVC && config_.prefer_vulkan_video &&', 1)

uc.write_text(t, encoding='utf-8')
print('patched PC unified decoder for HEVC')

from pathlib import Path
import re
p = Path(r"C:/MirageWork/MirageVulkan/src/video/unified_decoder.cpp")
t = p.read_text(encoding='utf-8', errors='ignore')

# Patch FFmpegDecoder::setCallback to COPY the frame data, because H264Decoder guarantees pointer validity only during callback.
old_pat = re.compile(r"void setCallback\(std::function<void\(const uint8_t\*, int, int, int64_t\)> cb\) \{[\s\S]*?decoder->set_frame_callback\(\[cb\]\(const uint8_t\* data, int w, int h, uint64_t pts\) \{[\s\S]*?cb\(data, w, h, static_cast<int64_t>\(pts\)\);[\s\S]*?\}\);[\s\S]*?\}", re.MULTILINE)

if not old_pat.search(t):
    raise SystemExit('pattern not found')

new_block = """void setCallback(std::function<void(const uint8_t*, int, int, int64_t)> cb) {
        // IMPORTANT: H264Decoder's callback pointer is only valid during the callback.
        // Copy into a persistent buffer before forwarding.
        decoder->set_frame_callback([this, cb](const uint8_t* data, int w, int h, uint64_t pts) {
            if (!data || w <= 0 || h <= 0) return;
            const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
            if (frame_buffer.size() != bytes) frame_buffer.resize(bytes);
            std::memcpy(frame_buffer.data(), data, bytes);
            cb(frame_buffer.data(), w, h, static_cast<int64_t>(pts));
        });
    }"""

t = old_pat.sub(new_block, t, count=1)

p.write_text(t, encoding='utf-8')
print('patched unified_decoder.cpp to copy FFmpeg frames')

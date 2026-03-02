// =============================================================================
// MirageSystem - Frame Analyzer (Tesseract OCR) 実装
// =============================================================================
#ifdef MIRAGE_OCR_ENABLED

#include "frame_analyzer.hpp"
#include "mirage_log.hpp"

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace mirage {

static constexpr const char* TAG = "ocr";

std::vector<OcrWord> OcrResult::findText(const std::string& query) const {
    std::vector<OcrWord> matches;
    if (query.empty()) return matches;
    std::string lower_query;
    lower_query.reserve(query.size());
    for (char c : query) lower_query += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& w : words) {
        std::string lower_text;
        lower_text.reserve(w.text.size());
        for (char c : w.text) lower_text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower_text.find(lower_query) != std::string::npos) {
            matches.push_back(w);
        }
    }
    return matches;
}

std::string OcrResult::fullText() const {
    std::string result;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) result += ' ';
        result += words[i].text;
    }
    return result;
}

struct FrameAnalyzer::TessImpl {
    tesseract::TessBaseAPI api;
    ~TessImpl() { api.End(); }
};

FrameAnalyzer::FrameAnalyzer() = default;
FrameAnalyzer::~FrameAnalyzer() { stopCapture(); }

bool FrameAnalyzer::init(const std::string& langs) {
    if (initialized_.load()) {
        MLOG_WARN(TAG, "Already initialized");
        return true;
    }
    auto impl = std::make_unique<TessImpl>();
    std::string tessdata_path;
    const char* env = std::getenv("TESSDATA_PREFIX");
    if (env && env[0] != '\0') {
        tessdata_path = env;
    } else {
        tessdata_path = "C:/msys64/mingw64/share/tessdata";
    }
    MLOG_INFO(TAG, "Tesseract init: langs=%s tessdata=%s", langs.c_str(), tessdata_path.c_str());
    int rc = impl->api.Init(tessdata_path.c_str(), langs.c_str(), tesseract::OEM_LSTM_ONLY);
    if (rc != 0) {
        MLOG_ERROR(TAG, "Tesseract Init failed: rc=%d", rc);
        return false;
    }
    impl->api.SetPageSegMode(tesseract::PSM_AUTO);
    tess_ = std::move(impl);
    initialized_.store(true);
    MLOG_INFO(TAG, "Tesseract initialized (langs=%s)", langs.c_str());
    return true;
}

void FrameAnalyzer::startCapture() {
    frame_sub_ = bus().subscribe<FrameReadyEvent>([this](const FrameReadyEvent& evt) { onFrame(evt); });
    MLOG_INFO(TAG, "Frame capture started");
}

void FrameAnalyzer::stopCapture() {
    frame_sub_ = SubscriptionHandle{};
    MLOG_INFO(TAG, "Frame capture stopped");
}

void FrameAnalyzer::onFrame(const FrameReadyEvent& evt) {
    if (evt.frame) {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        frames_[evt.device_id] = evt.frame;
        return;
    }
    if (!evt.rgba_data || evt.width <= 0 || evt.height <= 0) return;
    const size_t data_size = static_cast<size_t>(evt.width) * evt.height * 4;
    auto frame = std::make_shared<SharedFrame>();
    frame->device_id = evt.device_id;
    frame->width = evt.width;
    frame->height = evt.height;
    frame->frame_id = evt.frame_id;
    auto buffer = std::shared_ptr<uint8_t[]>(new uint8_t[data_size]);
    std::memcpy(buffer.get(), evt.rgba_data, data_size);
    frame->rgba = std::move(buffer);
    std::lock_guard<std::mutex> lock(frames_mutex_);
    frames_[evt.device_id] = frame;
}

OcrResult FrameAnalyzer::analyzeText(const std::string& device_id) {
    OcrResult result;
    result.device_id = device_id;
    if (!initialized_.load()) {
        MLOG_WARN(TAG, "Tesseract not initialized");
        return result;
    }
    std::shared_ptr<SharedFrame> frame;
    {
        std::lock_guard<std::mutex> lock(frames_mutex_);
        auto it = frames_.find(device_id);
        if (it == frames_.end() || !it->second) {
            MLOG_WARN(TAG, "No frame for device %s", device_id.c_str());
            return result;
        }
        frame = it->second;
    }
    return runOcr(frame->data(), frame->width, frame->height);
}

std::vector<OcrWord> FrameAnalyzer::findText(const std::string& device_id, const std::string& query) {
    return analyzeText(device_id).findText(query);
}

bool FrameAnalyzer::hasText(const std::string& device_id, const std::string& query) {
    return !findText(device_id, query).empty();
}

bool FrameAnalyzer::getTextCenter(const std::string& device_id, const std::string& query, int& cx, int& cy) {
    auto matches = findText(device_id, query);
    if (matches.empty()) return false;
    const auto& w = matches[0];
    cx = (w.x1 + w.x2) / 2;
    cy = (w.y1 + w.y2) / 2;
    return true;
}

OcrResult FrameAnalyzer::runOcr(const uint8_t* rgba, int w, int h) {
    OcrResult result;
    result.image_width = w;
    result.image_height = h;
    auto t_start = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(ocr_mutex_);
    PIX* pix = pixCreate(w, h, 32);
    if (!pix) {
        MLOG_ERROR(TAG, "pixCreate failed: %dx%d", w, h);
        return result;
    }
    l_uint32* pix_data = pixGetData(pix);
    int wpl = pixGetWpl(pix);
    for (int y = 0; y < h; ++y) {
        l_uint32* line = pix_data + y * wpl;
        const uint8_t* src = rgba + y * w * 4;
        for (int x = 0; x < w; ++x) {
            composeRGBPixel(src[x*4], src[x*4+1], src[x*4+2], &line[x]);
        }
    }
    PIX* pix_gray = pixConvertRGBToGray(pix, 0.0f, 0.0f, 0.0f);
    pixDestroy(&pix);
    if (!pix_gray) {
        MLOG_ERROR(TAG, "pixConvertRGBToGray failed");
        return result;
    }
    tess_->api.SetImage(pix_gray);
    tess_->api.Recognize(nullptr);
    tesseract::ResultIterator* ri = tess_->api.GetIterator();
    if (ri) {
        do {
            const char* word = ri->GetUTF8Text(tesseract::RIL_WORD);
            if (!word) continue;
            float conf = ri->Confidence(tesseract::RIL_WORD);
            int x1, y1, x2, y2;
            ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
            OcrWord ow;
            ow.text = word;
            ow.x1 = x1; ow.y1 = y1; ow.x2 = x2; ow.y2 = y2;
            ow.confidence = conf;
            result.words.push_back(std::move(ow));
            delete[] word;
        } while (ri->Next(tesseract::RIL_WORD));
    }
    pixDestroy(&pix_gray);
    auto t_end = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    MLOG_INFO(TAG, "OCR done: %d words %.1fms (%dx%d)", (int)result.words.size(), result.elapsed_ms, w, h);
    return result;
}

FrameAnalyzer& analyzer() {
    static FrameAnalyzer instance;
    return instance;
}

} // namespace mirage

#endif // MIRAGE_OCR_ENABLED

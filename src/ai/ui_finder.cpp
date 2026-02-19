// =============================================================================
// UiFinder - マルチ戦略UI要素検索 (Result型統合)
// src/ui_finder.cpp から移行: optional+last_error_ → Result<T>
// =============================================================================

#include "ai/ui_finder.hpp"
#include "mirage_log.hpp"

#ifdef MIRAGE_OCR_ENABLED
#include "frame_analyzer.hpp"
#endif

#include <sstream>
#include <fstream>
#include <regex>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static constexpr const char* TAG = "UiFinder";

namespace mirage::ai {

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================

UiFinder::UiFinder() {
    // デフォルトADB実行関数（サブプロセス呼び出し）
    adb_executor_ = [](const std::string& cmd) -> std::string {
        std::string full_cmd = "adb " + cmd;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        HANDLE hReadPipe = NULL, hWritePipe = NULL;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWritePipe; si.hStdError = hWritePipe; si.hStdInput = NULL;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::string cmdline = "cmd.exe /c " + full_cmd;
        BOOL ok = CreateProcessA(NULL, cmdline.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        CloseHandle(hWritePipe);
        std::string result;
        if (ok) {
            WaitForSingleObject(pi.hProcess, 30000);
            char buf[4096];
            while (true) {
                DWORD avail = 0;
                if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
                DWORD bytesRead = 0;
                if (!ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) || bytesRead == 0) break;
                buf[bytesRead] = '\0'; result += buf;
                if (result.size() > 65536) break;
            }
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        CloseHandle(hReadPipe);
        return result;
#else
        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) return "";
        std::string result;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }
        pclose(pipe);
        return result;
#endif
    };
}

UiFinder::~UiFinder() = default;

// =============================================================================
// 設定
// =============================================================================

void UiFinder::set_adb_executor(AdbExecutor executor) {
    std::lock_guard<std::mutex> lock(mutex_);
    adb_executor_ = std::move(executor);
}

void UiFinder::set_device_model(const std::string& model) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_model_ = model;
}

void UiFinder::set_device_id(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_id_ = id;
}

// =============================================================================
// EventBus連携
// =============================================================================

void UiFinder::subscribe_events() {
    auto sub = mirage::bus().subscribe<UiFindRequestEvent>(
        [this](const UiFindRequestEvent& e) {
            on_find_request(e);
        });
    subscriptions_.push_back(std::move(sub));
}

void UiFinder::on_find_request(const UiFindRequestEvent& event) {
    // デバイスID一時設定
    {
        std::lock_guard<std::mutex> lock(mutex_);
        device_id_ = event.device_id;
    }

    auto result = find(event.identifier, event.strategy, event.timeout_ms);

    UiFindResultEvent resp;
    resp.request_id = event.request_id;
    if (result.is_ok()) {
        resp.found = true;
        resp.element = result.value();
    } else {
        resp.found = false;
        resp.error = result.error().message;
    }
    mirage::bus().publish(resp);
}

// =============================================================================
// 検索（メインAPI）
// =============================================================================

Result<UiElement> UiFinder::find(
    const std::string& identifier,
    SearchStrategy strategy,
    int timeout_ms)
{
    auto start = std::chrono::steady_clock::now();

    while (true) {
        Result<UiElement> result = mirage::Err<UiElement>("未検索");

        switch (strategy) {
            case SearchStrategy::RESOURCE_ID:
                result = find_by_resource_id(identifier);
                break;
            case SearchStrategy::TEXT:
                result = find_by_text(identifier, true);
                break;
            case SearchStrategy::OCR: {
                std::lock_guard<std::mutex> lock(mutex_);
                result = find_by_ocr(identifier, device_id_);
                break;
            }
            case SearchStrategy::COORDINATE_TABLE:
                result = find_from_table(identifier);
                break;
            case SearchStrategy::AUTO: {
                // 戦略を順に試行
                result = find_by_resource_id(identifier);
                if (result.is_err()) result = find_by_text(identifier, true);
                if (result.is_err()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    result = find_by_ocr(identifier, device_id_);
                }
                if (result.is_err()) result = find_from_table(identifier);
                break;
            }
        }

        if (result.is_ok()) return result;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed >= timeout_ms) {
            MLOG_DEBUG(TAG, "要素が見つからない: %s", identifier.c_str());
            return mirage::Err<UiElement>("タイムアウト: " + identifier);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// =============================================================================
// resource-id検索
// =============================================================================

Result<UiElement> UiFinder::find_by_resource_id(const std::string& resource_id) {
    auto xml_result = dump_ui_hierarchy();
    if (xml_result.is_err()) {
        return mirage::Err<UiElement>(xml_result.error());
    }

    auto elements = parse_ui_dump(xml_result.value());
    for (const auto& elem : elements) {
        if (elem.resource_id == resource_id ||
            elem.resource_id.find(resource_id) != std::string::npos) {
            return elem;
        }
    }

    MLOG_DEBUG(TAG, "resource-id未検出: %s", resource_id.c_str());
    return mirage::Err<UiElement>("resource-id未検出: " + resource_id);
}

// =============================================================================
// テキスト検索
// =============================================================================

Result<UiElement> UiFinder::find_by_text(const std::string& text, bool partial_match) {
    auto xml_result = dump_ui_hierarchy();
    if (xml_result.is_err()) {
        return mirage::Err<UiElement>(xml_result.error());
    }

    auto elements = parse_ui_dump(xml_result.value());
    for (const auto& elem : elements) {
        if (partial_match) {
            if (elem.text.find(text) != std::string::npos) {
                return elem;
            }
        } else {
            if (elem.text == text) {
                return elem;
            }
        }
    }

    MLOG_DEBUG(TAG, "テキスト未検出: %s", text.c_str());
    return mirage::Err<UiElement>("テキスト未検出: " + text);
}

// =============================================================================
// OCR検索
// =============================================================================

Result<UiElement> UiFinder::find_by_ocr(
    const std::string& text,
    const std::string& device_id)
{
#ifdef MIRAGE_OCR_ENABLED
    if (device_id.empty()) {
        MLOG_WARN(TAG, "device_idが未設定のためOCR検索スキップ");
        return mirage::Err<UiElement>("OCR検索にはdevice_idが必要");
    }

    auto& fa = mirage::analyzer();
    if (!fa.isInitialized()) {
        MLOG_WARN(TAG, "FrameAnalyzer未初期化");
        return mirage::Err<UiElement>("FrameAnalyzer未初期化");
    }

    // findText() で部分一致検索
    auto matches = fa.findText(device_id, text);
    if (!matches.empty()) {
        const auto& word = matches[0];
        UiElement elem;
        elem.x = word.x1;
        elem.y = word.y1;
        elem.width = word.x2 - word.x1;
        elem.height = word.y2 - word.y1;
        elem.text = word.text;
        elem.clickable = true;
        return elem;
    }

    // getTextCenter() でもフォールバック試行
    int cx = 0, cy = 0;
    if (fa.getTextCenter(device_id, text, cx, cy)) {
        UiElement elem;
        elem.x = cx;
        elem.y = cy;
        elem.width = 1;
        elem.height = 1;
        elem.text = text;
        elem.clickable = true;
        return elem;
    }
#else
    (void)device_id;
#endif
    MLOG_DEBUG(TAG, "OCR未検出: %s", text.c_str());
    return mirage::Err<UiElement>("OCR未検出: " + text);
}

// =============================================================================
// 座標テーブル検索
// =============================================================================

Result<UiElement> UiFinder::find_from_table(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : coordinate_table_) {
        if (entry.key == key) {
            // デバイスモデルのマッチ確認
            if (!entry.device_model.empty() && !device_model_.empty()) {
                if (entry.device_model != device_model_) {
                    continue;
                }
            }

            UiElement elem;
            elem.x = entry.x;
            elem.y = entry.y;
            elem.width = 1;
            elem.height = 1;
            elem.text = entry.description;
            elem.clickable = true;
            return elem;
        }
    }

    MLOG_DEBUG(TAG, "座標テーブルにキーなし: %s", key.c_str());
    return mirage::Err<UiElement>("座標テーブルにキーなし: " + key);
}

// =============================================================================
// 座標テーブル管理
// =============================================================================

void UiFinder::add_coordinate_entry(const CoordinateEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 同一キー/モデルの既存エントリを削除
    coordinate_table_.erase(
        std::remove_if(coordinate_table_.begin(), coordinate_table_.end(),
            [&entry](const CoordinateEntry& e) {
                return e.key == entry.key && e.device_model == entry.device_model;
            }),
        coordinate_table_.end()
    );
    coordinate_table_.push_back(entry);
}

Result<void> UiFinder::load_coordinate_table(const std::string& json_path) {
    std::ifstream file(json_path);
    if (!file) {
        MLOG_ERROR(TAG, "座標テーブル読み込み失敗: %s", json_path.c_str());
        return mirage::Error("ファイルを開けない: " + json_path);
    }

    // 簡易JSONパース（外部ライブラリ不要）
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    std::string pattern = R"_(\{\s*"key"\s*:\s*"([^"]+)"\s*,\s*"device_model"\s*:\s*"([^"]*)"\s*,\s*"x"\s*:\s*(\d+)\s*,\s*"y"\s*:\s*(\d+)\s*(?:,\s*"description"\s*:\s*"([^"]*)")?\s*\})_";
    std::regex entry_regex(pattern);

    auto begin = std::sregex_iterator(content.begin(), content.end(), entry_regex);
    auto end = std::sregex_iterator();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        coordinate_table_.clear();
        for (auto it = begin; it != end; ++it) {
            CoordinateEntry entry;
            entry.key = (*it)[1];
            entry.device_model = (*it)[2];
            try {
                entry.x = std::stoi((*it)[3]);
                entry.y = std::stoi((*it)[4]);
            } catch (const std::exception&) {
                entry.x = 0;
                entry.y = 0;
            }
            if (it->size() > 5) entry.description = (*it)[5];
            coordinate_table_.push_back(entry);
        }
    }

    return mirage::Ok();
}

Result<void> UiFinder::save_coordinate_table(const std::string& json_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream file(json_path);
    if (!file) {
        MLOG_ERROR(TAG, "座標テーブル保存失敗: %s", json_path.c_str());
        return mirage::Error("ファイル作成失敗: " + json_path);
    }

    file << "[\n";
    for (size_t i = 0; i < coordinate_table_.size(); ++i) {
        const auto& entry = coordinate_table_[i];
        file << "  {\n";
        file << "    \"key\": \"" << entry.key << "\",\n";
        file << "    \"device_model\": \"" << entry.device_model << "\",\n";
        file << "    \"x\": " << entry.x << ",\n";
        file << "    \"y\": " << entry.y << ",\n";
        file << "    \"description\": \"" << entry.description << "\"\n";
        file << "  }";
        if (i < coordinate_table_.size() - 1) file << ",";
        file << "\n";
    }
    file << "]\n";

    return mirage::Ok();
}

// =============================================================================
// UIヒエラルキーダンプ
// =============================================================================

Result<std::string> UiFinder::dump_ui_hierarchy() {
    AdbExecutor executor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        executor = adb_executor_;
    }

    if (!executor) {
        return mirage::Err<std::string>("ADB executor未設定");
    }

    // デバイスにダンプ出力
    executor("shell uiautomator dump /data/local/tmp/mirage_ui.xml");

    // ファイルをpullして読み取り
#ifdef _WIN32
    std::string temp_path = "C:\\Windows\\Temp\\mirage_ui.xml";
#else
    std::string temp_path = "/tmp/mirage_ui.xml";
#endif

    executor("pull /data/local/tmp/mirage_ui.xml " + temp_path);

    std::ifstream file(temp_path);
    if (!file) {
        return mirage::Err<std::string>("UIダンプ読み取り失敗");
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    if (content.empty()) {
        return mirage::Err<std::string>("UIヒエラルキー取得失敗（空）");
    }

    return content;
}

// =============================================================================
// XML解析
// =============================================================================

std::vector<UiElement> UiFinder::parse_ui_dump(const std::string& xml) {
    std::vector<UiElement> elements;

    // nodeタグをパース
    std::regex node_regex(R"(<node\s+([^>]+)/>)");
    auto begin = std::sregex_iterator(xml.begin(), xml.end(), node_regex);
    auto end = std::sregex_iterator();

    std::string attr_pattern = R"_(([a-zA-Z0-9_-]+)="([^"]*)")_";
    std::regex attr_regex(attr_pattern);

    for (auto it = begin; it != end; ++it) {
        std::string attrs = (*it)[1];
        UiElement elem;

        auto attr_begin = std::sregex_iterator(attrs.begin(), attrs.end(), attr_regex);
        auto attr_end = std::sregex_iterator();

        for (auto attr_it = attr_begin; attr_it != attr_end; ++attr_it) {
            std::string name = (*attr_it)[1];
            std::string value = (*attr_it)[2];

            if (name == "resource-id") elem.resource_id = value;
            else if (name == "text") elem.text = value;
            else if (name == "class") elem.class_name = value;
            else if (name == "clickable") elem.clickable = (value == "true");
            else if (name == "enabled") elem.enabled = (value == "true");
            else if (name == "bounds") {
                parse_bounds(value, elem.x, elem.y, elem.width, elem.height);
            }
        }

        elements.push_back(elem);
    }

    return elements;
}

bool UiFinder::parse_bounds(const std::string& bounds, int& x, int& y, int& w, int& h) {
    // 形式: [x1,y1][x2,y2]
    std::regex bounds_regex(R"(\[(\d+),(\d+)\]\[(\d+),(\d+)\])");
    std::smatch match;

    if (std::regex_match(bounds, match, bounds_regex)) {
        try {
            int x1 = std::stoi(match[1]);
            int y1 = std::stoi(match[2]);
            int x2 = std::stoi(match[3]);
            int y2 = std::stoi(match[4]);

            x = x1;
            y = y1;
            w = x2 - x1;
            h = y2 - y1;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    return false;
}

} // namespace mirage::ai

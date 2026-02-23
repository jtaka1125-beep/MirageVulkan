// =============================================================================
// テンプレートマニフェスト - JSON読み書き・ID管理
// =============================================================================
#include "ai/template_manifest.hpp"
#include "mirage_log.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr const char* TAG = "TplManifest";

namespace mirage::ai {

static std::string readAll(const std::string& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

static bool writeAll(const std::string& p, const std::string& s) {
    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) return false;
    ofs << s;
    return true;
}

// JSON文字列エスケープ（書き出し用）
// " → \", \ → \\, 改行 → \n, タブ → \t, 制御文字 → \uXXXX
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (c < 0x20) {
                // 制御文字 → \uXXXX
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out += (char)c;
            }
            break;
        }
    }
    return out;
}

// JSON文字列アンエスケープ（読み込み用）
// \n → 改行, \t → タブ, \" → ", \\ → \, \uXXXX → UTF-8
static std::string jsonUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'u':
                // \uXXXX → UTF-8エンコード（サロゲートペア対応）
                if (i + 4 < s.size()) {
                    char hex[5] = {};
                    hex[0] = s[i+1]; hex[1] = s[i+2];
                    hex[2] = s[i+3]; hex[3] = s[i+4];
                    unsigned int cp = (unsigned int)std::strtoul(hex, nullptr, 16);
                    i += 4;
                    // サロゲートペア: \uD800-\uDBFF + \uDC00-\uDFFF → U+10000以上
                    if (cp >= 0xD800 && cp <= 0xDBFF
                        && i + 6 < s.size() && s[i+1] == '\\' && s[i+2] == 'u') {
                        char hex2[5] = {};
                        hex2[0] = s[i+3]; hex2[1] = s[i+4];
                        hex2[2] = s[i+5]; hex2[3] = s[i+6];
                        unsigned int lo = (unsigned int)std::strtoul(hex2, nullptr, 16);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        // 4バイト UTF-8 (U+10000以上)
                        out += (char)(0xF0 | (cp >> 18));
                        out += (char)(0x80 | ((cp >> 12) & 0x3F));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            default:
                out += s[i];
                break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// 最小限JSONパーサー（マニフェスト専用フォーマット）
// エスケープシーケンス（\" \\ \n \t \uXXXX）を正しく処理
static bool findString(const std::string& j, const std::string& key, std::string& out) {
    auto k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return false;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return false;
    pos = j.find('"', pos);
    if (pos == std::string::npos) return false;
    // 文字列終端を探す（\" はエスケープなので終端ではない）
    size_t start = pos + 1;
    size_t i = start;
    while (i < j.size()) {
        if (j[i] == '\\') {
            i += 2; // エスケープシーケンスをスキップ
        } else if (j[i] == '"') {
            break;  // 終端クォート
        } else {
            ++i;
        }
    }
    if (i >= j.size()) return false;
    out = jsonUnescape(j.substr(start, i - start));
    return true;
}

static bool findInt(const std::string& j, const std::string& key, int& out) {
    auto k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return false;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\n' || j[pos] == '\r' || j[pos] == '\t')) pos++;
    char* e = nullptr;
    out = (int)std::strtol(j.c_str() + pos, &e, 10);
    return e && e != j.c_str() + pos;
}

static bool findU64(const std::string& j, const std::string& key, uint64_t& out) {
    auto k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return false;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\n' || j[pos] == '\r' || j[pos] == '\t')) pos++;
    char* e = nullptr;
    out = (uint64_t)std::strtoull(j.c_str() + pos, &e, 10);
    return e && e != j.c_str() + pos;
}

static bool findU32(const std::string& j, const std::string& key, uint32_t& out) {
    uint64_t t = 0;
    if (!findU64(j, key, t)) return false;
    out = (uint32_t)t;
    return true;
}

static bool findFloat(const std::string& j, const std::string& key, float& out) {
    auto k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return false;
    pos = j.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\n' || j[pos] == '\r' || j[pos] == '\t')) pos++;
    char* e = nullptr;
    out = std::strtof(j.c_str() + pos, &e);
    return e && e != j.c_str() + pos;
}

// JSON内の位置から行番号を計算
static size_t lineNumber(const std::string& s, size_t pos) {
    size_t line = 1;
    for (size_t i = 0; i < pos && i < s.size(); ++i) {
        if (s[i] == '\n') ++line;
    }
    return line;
}

// 文字列を考慮した括弧マッチング（エスケープ済み文字列内の {} [] を無視）
// pos は '{' or '[' の位置。対応する閉じ括弧の位置を返す
static size_t findMatchingBracket(const std::string& j, size_t pos) {
    if (pos >= j.size()) return std::string::npos;
    char open = j[pos];
    char close = (open == '{') ? '}' : (open == '[') ? ']' : '\0';
    if (!close) return std::string::npos;

    int depth = 0;
    for (size_t i = pos; i < j.size(); ++i) {
        if (j[i] == '"') {
            // 文字列をスキップ（エスケープ考慮）
            ++i;
            while (i < j.size() && j[i] != '"') {
                if (j[i] == '\\') ++i;
                ++i;
            }
        } else if (j[i] == open) {
            ++depth;
        } else if (j[i] == close) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

// JSON構造の基本検証（括弧の対応、文字列の閉じ）
// エラー時は行番号・位置情報を含むメッセージを返す
static bool validateJsonStructure(const std::string& j, std::string* err) {
    std::vector<char> stack;
    bool inString = false;
    for (size_t i = 0; i < j.size(); ++i) {
        if (inString) {
            if (j[i] == '\\') {
                ++i; // エスケープされた文字をスキップ
            } else if (j[i] == '"') {
                inString = false;
            }
            continue;
        }
        switch (j[i]) {
        case '"': inString = true; break;
        case '{': stack.push_back('}'); break;
        case '[': stack.push_back(']'); break;
        case '}':
        case ']':
            if (stack.empty() || stack.back() != j[i]) {
                if (err) {
                    *err = "JSONパースエラー: 不正な '" + std::string(1, j[i])
                         + "' (行 " + std::to_string(lineNumber(j, i))
                         + ", 位置 " + std::to_string(i) + ")";
                }
                return false;
            }
            stack.pop_back();
            break;
        default: break;
        }
    }
    if (inString) {
        if (err) *err = "JSONパースエラー: 閉じられていない文字列";
        return false;
    }
    if (!stack.empty()) {
        if (err) {
            *err = "JSONパースエラー: 閉じられていない括弧 ('"
                 + std::string(1, stack.back()) + "' が不足)";
        }
        return false;
    }
    return true;
}

// 配列内のオブジェクト分割（文字列内の {} を考慮）
static std::vector<std::string> splitObjectsInArray(const std::string& j, const std::string& arrayKey) {
    std::vector<std::string> objs;
    auto k = "\"" + arrayKey + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return objs;
    pos = j.find('[', pos);
    if (pos == std::string::npos) return objs;
    // 対応する ']' を文字列考慮で検索
    size_t end = findMatchingBracket(j, pos);
    if (end == std::string::npos) return objs;

    // 配列内のトップレベルオブジェクトを抽出（文字列内の {} を無視）
    for (size_t i = pos + 1; i < end; ++i) {
        if (j[i] == '"') {
            // 文字列をスキップ
            ++i;
            while (i < end && j[i] != '"') {
                if (j[i] == '\\') ++i;
                ++i;
            }
        } else if (j[i] == '{') {
            size_t objEnd = findMatchingBracket(j, i);
            if (objEnd == std::string::npos) break;
            objs.push_back(j.substr(i, objEnd - i + 1));
            i = objEnd;
        }
    }
    return objs;
}

// サブオブジェクト抽出: "key": { ... }（文字列内の {} を考慮）
static std::string findSubObject(const std::string& j, const std::string& key) {
    auto k = "\"" + key + "\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return {};
    pos = j.find('{', pos);
    if (pos == std::string::npos) return {};
    size_t end = findMatchingBracket(j, pos);
    if (end == std::string::npos) return {};
    return j.substr(pos, end - pos + 1);
}


bool loadManifestJson(const std::string& path_utf8, TemplateManifest& out, std::string* err) {
    out = {};
    auto j = readAll(path_utf8);
    if (j.empty()) {
        if (err) *err = "manifest not found or empty: " + path_utf8;
        return false;
    }
    // JSON構造の基本検証（括弧/文字列の対応チェック）
    if (!validateJsonStructure(j, err)) {
        MLOG_WARN(TAG, "マニフェストJSON構造不正: %s", path_utf8.c_str());
        return false;
    }
    findInt(j, "version", out.version);
    findString(j, "root_dir", out.root_dir);

    auto objs = splitObjectsInArray(j, "entries");
    for (auto& o : objs) {
        TemplateEntry e;
        if (!findInt(o, "template_id", e.template_id)) continue;
        findString(o, "name", e.name);
        findString(o, "file", e.file);
        findInt(o, "w", e.w);
        findInt(o, "h", e.h);
        findU64(o, "mtime_utc", e.mtime_utc);
        findU32(o, "crc32", e.crc32);
        findString(o, "tags", e.tags);
        findFloat(o, "threshold", e.threshold);
        {
            auto roi_str = findSubObject(o, "roi");
            if (!roi_str.empty()) {
                findFloat(roi_str, "x", e.roi_x);
                findFloat(roi_str, "y", e.roi_y);
                findFloat(roi_str, "w", e.roi_w);
                findFloat(roi_str, "h", e.roi_h);
            }
        }
        out.entries.push_back(std::move(e));
    }
    if (out.root_dir.empty()) out.root_dir = "templates";

    MLOG_DEBUG(TAG, "マニフェスト読込: %zu エントリ, version=%d",
               out.entries.size(), out.version);
    return true;
}

bool saveManifestJson(const std::string& path_utf8, const TemplateManifest& m, std::string* err) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"version\": " << m.version << ",\n";
    ss << "  \"root_dir\": \"" << jsonEscape(m.root_dir) << "\",\n";
    ss << "  \"entries\": [\n";
    for (size_t i = 0; i < m.entries.size(); ++i) {
        auto& e = m.entries[i];
        ss << "    {\n";
        ss << "      \"template_id\": " << e.template_id << ",\n";
        ss << "      \"name\": \"" << jsonEscape(e.name) << "\",\n";
        ss << "      \"file\": \"" << jsonEscape(e.file) << "\",\n";
        ss << "      \"w\": " << e.w << ",\n";
        ss << "      \"h\": " << e.h << ",\n";
        ss << "      \"mtime_utc\": " << e.mtime_utc << ",\n";
        ss << "      \"crc32\": " << e.crc32 << ",\n";
        ss << "      \"tags\": \"" << jsonEscape(e.tags) << "\"";  
        if (e.threshold > 0.0f) ss << ",\n      \"threshold\": " << e.threshold;
        if (e.roi_w > 0.0f || e.roi_h > 0.0f) {
            ss << ",\n      \"roi\": { \"x\": " << e.roi_x << ", \"y\": " << e.roi_y
               << ", \"w\": " << e.roi_w << ", \"h\": " << e.roi_h << " }";
        }
        ss << "\n";
        ss << "    }" << (i + 1 < m.entries.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";

    if (!writeAll(path_utf8, ss.str())) {
        if (err) *err = "write failed";
        MLOG_ERROR(TAG, "マニフェスト保存失敗: %s", path_utf8.c_str());
        return false;
    }

    MLOG_DEBUG(TAG, "マニフェスト保存: %zu エントリ -> %s",
               m.entries.size(), path_utf8.c_str());
    return true;
}

std::unordered_map<int, size_t> indexById(const TemplateManifest& m) {
    std::unordered_map<int, size_t> idx;
    for (size_t i = 0; i < m.entries.size(); ++i) idx[m.entries[i].template_id] = i;
    return idx;
}

int allocateNextId(const TemplateManifest& m, int start_id) {
    auto idx = indexById(m);
    int id = start_id;
    while (idx.count(id)) id++;
    return id;
}

} // namespace mirage::ai

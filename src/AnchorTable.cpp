#include "soundsys/AnchorTable.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace soundsys {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON reader.
//
// The module deliberately carries no JSON dependency: the only JSON it ever
// reads is the anchor file it also writes (tools/ngmap), so a short
// recursive-descent parser over the subset {object, array, string, number,
// bool, null} is cheaper than a library and never surprises us.
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;

    bool                   boolean = false;
    double                 number = 0.0;
    std::string            string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : object) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool parse(JsonValue& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return true;
    }

    const std::string& error() const { return error_; }

private:
    void skipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool fail(const std::string& msg) {
        if (error_.empty()) {
            error_ = msg + " at offset " + std::to_string(pos_);
        }
        return false;
    }

    bool parseValue(JsonValue& out) {
        skipWs();
        if (pos_ >= s_.size()) return fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return parseObject(out);
            case '[': return parseArray(out);
            case '"': out.type = JsonValue::Type::String; return parseString(out.string);
            case 't':
            case 'f': return parseBool(out);
            case 'n': return parseNull(out);
            default:  return parseNumber(out);
        }
    }

    bool parseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++pos_;  // '{'
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected ':'");
            ++pos_;
            JsonValue value;
            if (!parseValue(value)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++pos_;  // '['
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            JsonValue value;
            if (!parseValue(value)) return false;
            out.array.push_back(std::move(value));
            skipWs();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= s_.size()) break;
            const char esc = s_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {                             // \uXXXX
                    // Anchor labels are ASCII, so keep the escape byte-safe
                    // rather than pulling in a UTF-16 decoder.
                    if (pos_ + 4 > s_.size()) return fail("bad unicode escape");
                    const int code = std::stoi(s_.substr(pos_, 4), nullptr, 16);
                    pos_ += 4;
                    out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                    break;
                }
                default: return fail("bad escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            const bool numeric = std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
                                 c == 'e' || c == 'E' || c == '-' || c == '+';
            if (!numeric) break;
            ++pos_;
        }
        if (pos_ == start) return fail("expected a value");
        out.type = JsonValue::Type::Number;
        out.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    bool parseBool(JsonValue& out) {
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        return fail("expected a boolean");
    }

    bool parseNull(JsonValue& out) {
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out.type = JsonValue::Type::Null;
            return true;
        }
        return fail("expected null");
    }

    const std::string& s_;
    std::size_t        pos_ = 0;
    std::string        error_;
};

// Linear interpolation over a monotonic anchor set, clamped outside the range.
double interpolate(const std::vector<Anchor>& a, double x, bool byNg) {
    const auto xs = [&](const Anchor& an) { return byNg ? an.ng : an.t; };
    const auto ys = [&](const Anchor& an) { return byNg ? an.t : an.ng; };

    if (x <= xs(a.front())) return ys(a.front());
    if (x >= xs(a.back())) return ys(a.back());

    // A handful of anchors: a linear scan beats a binary search and keeps the
    // audio-thread path branch-predictable.
    for (std::size_t i = 1; i < a.size(); ++i) {
        if (x <= xs(a[i])) {
            const double x0 = xs(a[i - 1]), x1 = xs(a[i]);
            const double y0 = ys(a[i - 1]), y1 = ys(a[i]);
            const double span = x1 - x0;
            if (span <= 0.0) return y1;
            return y0 + (y1 - y0) * (x - x0) / span;
        }
    }
    return ys(a.back());
}

}  // namespace

bool AnchorTable::setAnchors(std::vector<Anchor> anchors, std::string* error) {
    if (anchors.size() < 2) {
        if (error) *error = "an anchor table needs at least 2 anchors";
        return false;
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor& a, const Anchor& b) { return a.ng < b.ng; });

    // Both axes must be strictly increasing: NG has to be invertible against
    // recording time. This is exactly what fails if a landmark is marked during
    // a spool-down, or duplicated.
    for (std::size_t i = 1; i < anchors.size(); ++i) {
        if (anchors[i].ng <= anchors[i - 1].ng) {
            if (error) {
                *error = "anchors are not strictly increasing in NG (" +
                         std::to_string(anchors[i - 1].ng) + " -> " +
                         std::to_string(anchors[i].ng) + ")";
            }
            return false;
        }
        if (anchors[i].t <= anchors[i - 1].t) {
            if (error) {
                *error = "anchors are not strictly increasing in time (" +
                         std::to_string(anchors[i - 1].t) + " -> " +
                         std::to_string(anchors[i].t) + ")";
            }
            return false;
        }
    }
    anchors_ = std::move(anchors);
    return true;
}

bool AnchorTable::loadJson(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not open anchor file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!parseJson(ss.str(), error)) {
        if (error) *error = path + ": " + *error;
        return false;
    }
    return true;
}

bool AnchorTable::parseJson(const std::string& text, std::string* error) {
    JsonValue  root;
    JsonParser parser(text);
    if (!parser.parse(root) || root.type != JsonValue::Type::Object) {
        if (error) *error = parser.error().empty() ? "malformed JSON" : parser.error();
        return false;
    }

    if (const JsonValue* v = root.find("engine")) engine_ = v->string;
    if (const JsonValue* v = root.find("asset")) assetPath_ = v->string;
    if (const JsonValue* v = root.find("source")) source_ = v->string;

    const JsonValue* list = root.find("anchors");
    if (!list || list->type != JsonValue::Type::Array) {
        if (error) *error = "missing anchors array";
        return false;
    }

    std::vector<Anchor> parsed;
    parsed.reserve(list->array.size());
    for (const JsonValue& item : list->array) {
        if (item.type != JsonValue::Type::Object) {
            if (error) *error = "anchor entries must be objects";
            return false;
        }
        const JsonValue* ng = item.find("ng");
        const JsonValue* t = item.find("t");
        if (!ng || !t || ng->type != JsonValue::Type::Number ||
            t->type != JsonValue::Type::Number) {
            if (error) *error = "every anchor needs numeric ng and t fields";
            return false;
        }
        Anchor a;
        a.ng = ng->number;
        a.t = t->number;
        if (const JsonValue* label = item.find("label")) a.label = label->string;
        parsed.push_back(std::move(a));
    }

    return setAnchors(std::move(parsed), error);
}

double AnchorTable::timeForNg(double ng) const noexcept {
    if (!valid()) return 0.0;
    return interpolate(anchors_, ng, /*byNg=*/true);
}

double AnchorTable::ngForTime(double t) const noexcept {
    if (!valid()) return 0.0;
    return interpolate(anchors_, t, /*byNg=*/false);
}

double AnchorTable::timePerNg(double ng) const noexcept {
    if (!valid()) return 0.0;

    // Slope of the segment containing `ng`. The end segments extend outwards so
    // an over- or under-shooting NG still yields a sane speed instead of zero.
    std::size_t i = 1;
    while (i + 1 < anchors_.size() && ng > anchors_[i].ng) ++i;

    const double dng = anchors_[i].ng - anchors_[i - 1].ng;
    const double dt = anchors_[i].t - anchors_[i - 1].t;
    if (dng <= 0.0) return 0.0;
    return dt / dng;
}

double AnchorTable::timeOfLabel(const std::string& label) const noexcept {
    for (const Anchor& a : anchors_) {
        if (a.label == label) return a.t;
    }
    return -1.0;
}

}  // namespace soundsys

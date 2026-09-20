// json.cpp — реалізація парсера та генератора JSON.

#include "json.h"

#include <cmath>
#include <cstdlib>

namespace cm::json {

namespace {
const Value kNullValue{};
const Array kEmptyArray{};
const Object kEmptyObject{};
}  // namespace

// ── Value ────────────────────────────────────────────────────────────────────

int64_t Value::asInt(int64_t fallback) const {
    if (!isNumber()) return fallback;
    // Значення поза межами int64 не перетворюємо: краще повернути запасне,
    // ніж мовчки отримати невизначену поведінку приведення.
    if (!std::isfinite(number_)) return fallback;
    if (number_ > 9.2233720368547758e18 || number_ < -9.2233720368547758e18) return fallback;
    return static_cast<int64_t>(number_);
}

const std::string& Value::asString() const {
    static const std::string kEmpty;
    return isString() ? string_ : kEmpty;
}

std::string Value::asStringOr(std::string_view fallback) const {
    return isString() ? string_ : std::string(fallback);
}

const Array& Value::asArray() const {
    return isArray() ? array_ : kEmptyArray;
}

const Object& Value::asObject() const {
    return isObject() ? object_ : kEmptyObject;
}

const Value& Value::operator[](std::string_view key) const {
    if (!isObject()) return kNullValue;
    const auto it = object_.find(key);
    return it == object_.end() ? kNullValue : it->second;
}

const Value& Value::operator[](size_t index) const {
    if (!isArray() || index >= array_.size()) return kNullValue;
    return array_[index];
}

const Value& Value::path(std::string_view dottedPath) const {
    const Value* current = this;
    size_t start = 0;

    while (start <= dottedPath.size()) {
        const size_t dot = dottedPath.find('.', start);
        const std::string_view segment =
            dottedPath.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);

        if (segment.empty()) break;
        current = &(*current)[segment];
        if (current->isNull()) return kNullValue;

        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return *current;
}

size_t Value::size() const {
    if (isArray())  return array_.size();
    if (isObject()) return object_.size();
    if (isString()) return string_.size();
    return 0;
}

bool Value::has(std::string_view key) const {
    return isObject() && object_.find(key) != object_.end();
}

// ── Парсер ───────────────────────────────────────────────────────────────────

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse(Value& out) {
        skipWhitespace();
        if (!parseValue(out, 0)) return false;
        skipWhitespace();
        // Зайві дані після документа означають некоректний вхід.
        return pos_ == text_.size();
    }

private:
    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool expect(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool literal(std::string_view word) {
        if (text_.size() - pos_ < word.size()) return false;
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    bool parseValue(Value& out, int depth) {
        // Обмеження глибини — головний захист парсера. Без нього документ
        // із тисячами вкладених масивів переповнив би стек.
        if (depth >= kMaxDepth) return false;
        if (eof()) return false;

        switch (peek()) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string s;
                if (!parseString(s)) return false;
                out = Value(std::move(s));
                return true;
            }
            case 't': if (!literal("true"))  return false; out = Value(true);  return true;
            case 'f': if (!literal("false")) return false; out = Value(false); return true;
            case 'n': if (!literal("null"))  return false; out = Value();      return true;
            default:  return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        if (!expect('{')) return false;
        Object object;

        skipWhitespace();
        if (expect('}')) { out = Value(std::move(object)); return true; }

        for (;;) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;

            skipWhitespace();
            if (!expect(':')) return false;

            skipWhitespace();
            Value value;
            if (!parseValue(value, depth + 1)) return false;

            // Дублікат ключа: перемагає останній, як у більшості реалізацій.
            object[std::move(key)] = std::move(value);

            skipWhitespace();
            if (expect(',')) continue;
            if (expect('}')) break;
            return false;
        }

        out = Value(std::move(object));
        return true;
    }

    bool parseArray(Value& out, int depth) {
        if (!expect('[')) return false;
        Array array;

        skipWhitespace();
        if (expect(']')) { out = Value(std::move(array)); return true; }

        for (;;) {
            skipWhitespace();
            Value value;
            if (!parseValue(value, depth + 1)) return false;
            array.push_back(std::move(value));

            skipWhitespace();
            if (expect(',')) continue;
            if (expect(']')) break;
            return false;
        }

        out = Value(std::move(array));
        return true;
    }

    bool parseString(std::string& out) {
        if (!expect('"')) return false;
        out.clear();

        while (pos_ < text_.size()) {
            const char c = text_[pos_++];

            if (c == '"') return true;

            if (c != '\\') {
                // Керівні символи в рядку JSON заборонені стандартом.
                if (static_cast<unsigned char>(c) < 0x20) return false;
                out += c;
                continue;
            }

            if (pos_ >= text_.size()) return false;
            const char escape = text_[pos_++];
            switch (escape) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':  if (!parseUnicodeEscape(out)) return false; break;
                default:   return false;
            }
        }
        return false;  // рядок не закрито
    }

    bool readHex4(uint32_t& out) {
        if (text_.size() - pos_ < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseUnicodeEscape(std::string& out) {
        uint32_t code = 0;
        if (!readHex4(code)) return false;

        // Сурогатна пара: старший сурогат має бути в парі з молодшим.
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (text_.size() - pos_ < 6 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                // Непарний сурогат замінюємо на U+FFFD замість відмови:
                // транскрипт із таким символом усе одно варто прочитати.
                appendUtf8(out, 0xFFFD);
                return true;
            }
            pos_ += 2;
            uint32_t low = 0;
            if (!readHex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) { appendUtf8(out, 0xFFFD); return true; }
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            appendUtf8(out, 0xFFFD);
            return true;
        }

        appendUtf8(out, code);
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool parseNumber(Value& out) {
        const size_t start = pos_;

        if (peek() == '-') ++pos_;

        if (eof()) return false;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            return false;
        }

        if (!eof() && peek() == '.') {
            ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return false;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || peek() < '0' || peek() > '9') return false;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }

        // strtod потребує рядка з нульовим завершенням; копіюємо лише
        // числовий фрагмент, який завжди короткий.
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()) return false;

        out = Value(parsed);
        return true;
    }

    std::string_view text_;
    size_t pos_ = 0;
};

}  // namespace

bool Parse(std::string_view text, Value& out) {
    Parser parser(text);
    return parser.parse(out);
}

// ── Writer ───────────────────────────────────────────────────────────────────

void EscapeString(std::string_view text, std::string& out) {
    out += '"';
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"':  out += "\\\"";  break;
            case '\\': out += "\\\\";  break;
            case '\b': out += "\\b";   break;
            case '\f': out += "\\f";   break;
            case '\n': out += "\\n";   break;
            case '\r': out += "\\r";   break;
            case '\t': out += "\\t";   break;
            default:
                if (c < 0x20) {
                    static constexpr char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0x0F];
                    out += kHex[c & 0x0F];
                } else {
                    // Байти UTF-8 передаються як є: JSON дозволяє UTF-8,
                    // а екранування роздуло б кадр удвічі.
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void Writer::separator() {
    if (afterKey_) { afterKey_ = false; return; }
    if (needComma_) out_ += ',';
}

void Writer::beginObject() { separator(); out_ += '{'; needComma_ = false; }
void Writer::endObject()   { out_ += '}'; needComma_ = true; }
void Writer::beginArray()  { separator(); out_ += '['; needComma_ = false; }
void Writer::endArray()    { out_ += ']'; needComma_ = true; }

void Writer::key(std::string_view name) {
    if (needComma_) out_ += ',';
    EscapeString(name, out_);
    out_ += ':';
    needComma_ = false;
    afterKey_ = true;
}

void Writer::valueNull()             { separator(); out_ += "null";  needComma_ = true; }
void Writer::value(bool v)           { separator(); out_ += v ? "true" : "false"; needComma_ = true; }
void Writer::value(std::string_view v) { separator(); EscapeString(v, out_); needComma_ = true; }
void Writer::raw(std::string_view f) { separator(); out_ += f; needComma_ = true; }

void Writer::value(int64_t v) {
    separator();
    char buffer[24];
    int length = 0;
    if (v == 0) {
        buffer[length++] = '0';
    } else {
        const bool negative = v < 0;
        // Беремо модуль в unsigned, щоб коректно обробити INT64_MIN,
        // модуль якого не вміщується в int64.
        uint64_t magnitude = negative ? (~static_cast<uint64_t>(v) + 1) : static_cast<uint64_t>(v);
        char temp[24];
        int t = 0;
        while (magnitude > 0) { temp[t++] = static_cast<char>('0' + magnitude % 10); magnitude /= 10; }
        if (negative) buffer[length++] = '-';
        while (t > 0) buffer[length++] = temp[--t];
    }
    out_.append(buffer, static_cast<size_t>(length));
    needComma_ = true;
}

void Writer::value(uint64_t v) {
    separator();
    char buffer[24];
    int length = 0;
    if (v == 0) {
        buffer[length++] = '0';
    } else {
        char temp[24];
        int t = 0;
        while (v > 0) { temp[t++] = static_cast<char>('0' + v % 10); v /= 10; }
        while (t > 0) buffer[length++] = temp[--t];
    }
    out_.append(buffer, static_cast<size_t>(length));
    needComma_ = true;
}

void Writer::value(double v) {
    separator();
    if (!std::isfinite(v)) {
        // JSON не має представлення для NaN та нескінченності.
        out_ += "null";
    } else {
        char buffer[32];
        const int length = std::snprintf(buffer, sizeof(buffer), "%.6g", v);
        if (length > 0) out_.append(buffer, static_cast<size_t>(length));
        else out_ += '0';
    }
    needComma_ = true;
}

}  // namespace cm::json

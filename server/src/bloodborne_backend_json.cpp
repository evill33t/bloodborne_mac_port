// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bloodborne_backend_json.h"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>

#include <QList>
#include <QStringDecoder>
#include <QStringList>

namespace Bloodborne::Backend {

Json::Json() = default;

Json Json::Bool(bool value) {
    Json json;
    json.m_kind = Kind::Bool;
    json.m_bool = value;
    return json;
}

Json Json::Int(qint64 value) {
    Json json;
    json.m_kind = Kind::Int;
    json.m_int = value;
    return json;
}

Json Json::Float(double value) {
    Json json;
    json.m_kind = Kind::Float;
    json.m_float = value;
    return json;
}

Json Json::String(const QString& value) {
    Json json;
    json.m_kind = Kind::String;
    json.m_string = value;
    return json;
}

Json Json::Array() {
    Json json;
    json.m_kind = Kind::Array;
    return json;
}

Json Json::Array(std::vector<Json> items) {
    Json json;
    json.m_kind = Kind::Array;
    json.m_items = std::move(items);
    return json;
}

Json Json::Object() {
    Json json;
    json.m_kind = Kind::Object;
    return json;
}

const Json* Json::Find(const QString& key) const {
    if (m_kind != Kind::Object) {
        return nullptr;
    }
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) {
            return &m_items[i];
        }
    }
    return nullptr;
}

Json Json::Get(const QString& key, const Json& fallback) const {
    const Json* found = Find(key);
    return found ? *found : fallback;
}

Json& Json::Set(const QString& key, Json value) {
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) {
            m_items[i] = std::move(value);
            return *this;
        }
    }
    m_keys.push_back(key);
    m_items.push_back(std::move(value));
    return *this;
}

Json& Json::Append(Json value) {
    m_items.push_back(std::move(value));
    return *this;
}

bool Json::operator==(const Json& other) const {
    if (m_kind != other.m_kind) {
        return false;
    }
    switch (m_kind) {
    case Kind::Null:
        return true;
    case Kind::Bool:
        return m_bool == other.m_bool;
    case Kind::Int:
        return m_int == other.m_int;
    case Kind::Float:
        return m_float == other.m_float;
    case Kind::String:
        return m_string == other.m_string;
    case Kind::Array:
    case Kind::Object:
        return m_keys == other.m_keys && m_items == other.m_items;
    }
    return false;
}

namespace {

class Parser {
public:
    explicit Parser(const QString& text) : m_text(text) {}

    std::optional<Json> Document() {
        SkipSpace();
        auto value = Value(0);
        if (!value) {
            return std::nullopt;
        }
        SkipSpace();
        if (m_at != m_text.size()) {
            return std::nullopt; // "Extra data"
        }
        return value;
    }

private:
    // Deep enough for any request; json.loads stops at Python's recursion limit too.
    static constexpr int MaxDepth = 512;

    bool AtEnd() const {
        return m_at >= m_text.size();
    }
    QChar Peek() const {
        return AtEnd() ? QChar() : m_text[m_at];
    }
    void SkipSpace() {
        while (!AtEnd()) {
            const char16_t c = m_text[m_at].unicode();
            if (c != u' ' && c != u'\t' && c != u'\n' && c != u'\r') {
                break;
            }
            ++m_at;
        }
    }
    bool Literal(QStringView word) {
        if (QStringView(m_text).mid(m_at).startsWith(word)) {
            m_at += word.size();
            return true;
        }
        return false;
    }

    std::optional<Json> Value(int depth) {
        if (depth > MaxDepth || AtEnd()) {
            return std::nullopt;
        }
        const char16_t c = Peek().unicode();
        if (c == u'{') {
            return ObjectValue(depth);
        }
        if (c == u'[') {
            return ArrayValue(depth);
        }
        if (c == u'"') {
            auto text = StringValue();
            if (!text) {
                return std::nullopt;
            }
            return Json::String(*text);
        }
        if (Literal(u"null")) {
            return Json();
        }
        if (Literal(u"true")) {
            return Json::Bool(true);
        }
        if (Literal(u"false")) {
            return Json::Bool(false);
        }
        if (Literal(u"NaN")) {
            return Json::Float(std::numeric_limits<double>::quiet_NaN());
        }
        if (Literal(u"Infinity")) {
            return Json::Float(std::numeric_limits<double>::infinity());
        }
        if (Literal(u"-Infinity")) {
            return Json::Float(-std::numeric_limits<double>::infinity());
        }
        return Number();
    }

    std::optional<Json> ObjectValue(int depth) {
        ++m_at; // {
        Json object = Json::Object();
        SkipSpace();
        if (Peek() == u'}') {
            ++m_at;
            return object;
        }
        while (true) {
            SkipSpace();
            if (Peek() != u'"') {
                return std::nullopt;
            }
            auto key = StringValue();
            if (!key) {
                return std::nullopt;
            }
            SkipSpace();
            if (Peek() != u':') {
                return std::nullopt;
            }
            ++m_at;
            SkipSpace();
            auto value = Value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            object.Set(*key, std::move(*value));
            SkipSpace();
            if (Peek() == u',') {
                ++m_at;
                continue;
            }
            if (Peek() == u'}') {
                ++m_at;
                return object;
            }
            return std::nullopt;
        }
    }

    std::optional<Json> ArrayValue(int depth) {
        ++m_at; // [
        Json array = Json::Array();
        SkipSpace();
        if (Peek() == u']') {
            ++m_at;
            return array;
        }
        while (true) {
            SkipSpace();
            auto value = Value(depth + 1);
            if (!value) {
                return std::nullopt;
            }
            array.Append(std::move(*value));
            SkipSpace();
            if (Peek() == u',') {
                ++m_at;
                continue;
            }
            if (Peek() == u']') {
                ++m_at;
                return array;
            }
            return std::nullopt;
        }
    }

    std::optional<char16_t> Hex4() {
        if (m_at + 4 > m_text.size()) {
            return std::nullopt;
        }
        char16_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char16_t c = m_text[m_at + i].unicode();
            int nibble;
            if (c >= u'0' && c <= u'9') {
                nibble = c - u'0';
            } else if (c >= u'a' && c <= u'f') {
                nibble = c - u'a' + 10;
            } else if (c >= u'A' && c <= u'F') {
                nibble = c - u'A' + 10;
            } else {
                return std::nullopt;
            }
            value = static_cast<char16_t>(value * 16 + nibble);
        }
        m_at += 4;
        return value;
    }

    std::optional<QString> StringValue() {
        ++m_at; // "
        QString out;
        while (true) {
            if (AtEnd()) {
                return std::nullopt;
            }
            const char16_t c = m_text[m_at].unicode();
            ++m_at;
            if (c == u'"') {
                return out;
            }
            if (c < 0x20) {
                return std::nullopt; // json.loads is strict about control characters
            }
            if (c != u'\\') {
                out.append(QChar(c));
                continue;
            }
            if (AtEnd()) {
                return std::nullopt;
            }
            const char16_t escape = m_text[m_at].unicode();
            ++m_at;
            switch (escape) {
            case u'"':
                out.append(u'"');
                break;
            case u'\\':
                out.append(u'\\');
                break;
            case u'/':
                out.append(u'/');
                break;
            case u'b':
                out.append(u'\b');
                break;
            case u'f':
                out.append(u'\f');
                break;
            case u'n':
                out.append(u'\n');
                break;
            case u'r':
                out.append(u'\r');
                break;
            case u't':
                out.append(u'\t');
                break;
            case u'u': {
                auto unit = Hex4();
                if (!unit) {
                    return std::nullopt;
                }
                out.append(QChar(*unit));
                break;
            }
            default:
                return std::nullopt;
            }
        }
    }

    std::optional<Json> Number() {
        const qsizetype start = m_at;
        if (Peek() == u'-') {
            ++m_at;
        }
        auto digit = [this] {
            const char16_t c = Peek().unicode();
            return !AtEnd() && c >= u'0' && c <= u'9';
        };
        if (!digit()) {
            return std::nullopt;
        }
        if (Peek() == u'0') {
            ++m_at;
        } else {
            while (digit()) {
                ++m_at;
            }
        }
        bool integer = true;
        if (Peek() == u'.' && m_at + 1 < m_text.size() && m_text[m_at + 1] >= u'0' &&
            m_text[m_at + 1] <= u'9') {
            integer = false;
            ++m_at;
            while (digit()) {
                ++m_at;
            }
        }
        if (Peek() == u'e' || Peek() == u'E') {
            qsizetype at = m_at + 1;
            if (at < m_text.size() && (m_text[at] == u'+' || m_text[at] == u'-')) {
                ++at;
            }
            if (at < m_text.size() && m_text[at] >= u'0' && m_text[at] <= u'9') {
                integer = false;
                m_at = at;
                while (digit()) {
                    ++m_at;
                }
            }
        }
        const QByteArray text = QStringView(m_text).mid(start, m_at - start).toLatin1();
        if (integer) {
            qint64 value = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec == std::errc()) {
                return Json::Int(value);
            }
        }
        return Json::Float(std::strtod(text.constData(), nullptr));
    }

    const QString& m_text;
    qsizetype m_at = 0;
};

void AppendString(QByteArray& out, const QString& text) {
    static constexpr char Hex[] = "0123456789abcdef";
    out.append('"');
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        switch (c) {
        case u'"':
            out.append("\\\"");
            break;
        case u'\\':
            out.append("\\\\");
            break;
        case u'\n':
            out.append("\\n");
            break;
        case u'\r':
            out.append("\\r");
            break;
        case u'\t':
            out.append("\\t");
            break;
        case u'\b':
            out.append("\\b");
            break;
        case u'\f':
            out.append("\\f");
            break;
        default:
            if (c >= 0x20 && c <= 0x7e) {
                out.append(static_cast<char>(c));
            } else {
                out.append("\\u");
                out.append(Hex[(c >> 12) & 0xF]);
                out.append(Hex[(c >> 8) & 0xF]);
                out.append(Hex[(c >> 4) & 0xF]);
                out.append(Hex[c & 0xF]);
            }
        }
    }
    out.append('"');
}

void Dump(QByteArray& out, const Json& value, const char* itemSeparator, const char* keySeparator) {
    switch (value.kind()) {
    case Json::Kind::Null:
        out.append("null");
        return;
    case Json::Kind::Bool:
        out.append(value.BoolValue() ? "true" : "false");
        return;
    case Json::Kind::Int:
        out.append(QByteArray::number(value.IntValue()));
        return;
    case Json::Kind::Float: {
        const double f = value.FloatValue();
        if (std::isnan(f)) {
            out.append("NaN");
        } else if (std::isinf(f)) {
            out.append(f > 0 ? "Infinity" : "-Infinity");
        } else {
            out.append(PythonFloat(f));
        }
        return;
    }
    case Json::Kind::String:
        AppendString(out, value.StringValue());
        return;
    case Json::Kind::Array:
        out.append('[');
        for (size_t i = 0; i < value.Items().size(); ++i) {
            if (i) {
                out.append(itemSeparator);
            }
            Dump(out, value.Items()[i], itemSeparator, keySeparator);
        }
        out.append(']');
        return;
    case Json::Kind::Object:
        out.append('{');
        for (size_t i = 0; i < value.Keys().size(); ++i) {
            if (i) {
                out.append(itemSeparator);
            }
            AppendString(out, value.Keys()[i]);
            out.append(keySeparator);
            Dump(out, value.Items()[i], itemSeparator, keySeparator);
        }
        out.append('}');
        return;
    }
}

QString PyRepr(const Json& value);

QString PyStringRepr(const QString& text) {
    const QChar quote = text.contains(u'\'') && !text.contains(u'"') ? u'"' : u'\'';
    QString out = quote;
    for (const QChar c : text) {
        if (c == u'\\' || c == quote) {
            out += u'\\';
            out += c;
        } else if (c == u'\n') {
            out += QStringLiteral("\\n");
        } else if (c == u'\r') {
            out += QStringLiteral("\\r");
        } else if (c == u'\t') {
            out += QStringLiteral("\\t");
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

QString PyRepr(const Json& value) {
    switch (value.kind()) {
    case Json::Kind::String:
        return PyStringRepr(value.StringValue());
    case Json::Kind::Array: {
        QStringList parts;
        for (const Json& item : value.Items()) {
            parts.append(PyRepr(item));
        }
        return u'[' + parts.join(QStringLiteral(", ")) + u']';
    }
    case Json::Kind::Object: {
        QStringList parts;
        for (size_t i = 0; i < value.Keys().size(); ++i) {
            parts.append(PyStringRepr(value.Keys()[i]) + QStringLiteral(": ") +
                         PyRepr(value.Items()[i]));
        }
        return u'{' + parts.join(QStringLiteral(", ")) + u'}';
    }
    default:
        return PyStr(value);
    }
}

std::optional<qint64> ParsePythonInt(const QString& raw) {
    const QString text = raw.trimmed();
    qsizetype at = 0;
    bool negative = false;
    if (at < text.size() && (text[at] == u'+' || text[at] == u'-')) {
        negative = text[at] == u'-';
        ++at;
    }
    if (at >= text.size()) {
        return std::nullopt;
    }
    // Magnitudes up to 2^63 (so the most negative 64-bit value fits too).
    constexpr quint64 Limit = quint64{1} << 63;
    quint64 magnitude = 0;
    bool lastWasDigit = false;
    for (; at < text.size(); ++at) {
        const QChar c = text[at];
        if (c == u'_') {
            // Underscores only between digits.
            if (!lastWasDigit || at + 1 >= text.size() || text[at + 1].digitValue() < 0) {
                return std::nullopt;
            }
            lastWasDigit = false;
            continue;
        }
        const int digit = c.digitValue();
        if (digit < 0) {
            return std::nullopt;
        }
        if (magnitude > (Limit - digit) / 10) {
            return std::nullopt; // beyond 64 bits: Python keeps a big int, this answers the default
        }
        magnitude = magnitude * 10 + digit;
        lastWasDigit = true;
    }
    if (negative) {
        return magnitude == Limit ? std::numeric_limits<qint64>::min()
                                  : -static_cast<qint64>(magnitude);
    }
    if (magnitude == Limit) {
        return std::nullopt;
    }
    return static_cast<qint64>(magnitude);
}

} // namespace

std::optional<Json> ParseJson(const QByteArray& text) {
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::ConvertInitialBom);
    const QString decoded = decoder(text);
    if (decoder.hasError()) {
        return std::nullopt;
    }
    if (decoded.startsWith(QChar(0xFEFF))) {
        return std::nullopt; // "Unexpected UTF-8 BOM"
    }
    return Parser(decoded).Document();
}

QByteArray DumpJson(const Json& value) {
    QByteArray out;
    Dump(out, value, ",", ":");
    return out;
}

QByteArray DumpJsonSpaced(const Json& value) {
    QByteArray out;
    Dump(out, value, ", ", ": ");
    return out;
}

QByteArray PythonFloat(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }
    // The shortest digits that read back as `value`, as Python's repr picks them.
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::scientific);
    const QByteArray scientific(buffer.data(), result.ptr - buffer.data());
    const qsizetype e = scientific.indexOf('e');
    QByteArray mantissa = scientific.left(e);
    const int exponent = scientific.mid(e + 1).toInt();
    QByteArray sign;
    if (mantissa.startsWith('-')) {
        sign = "-";
        mantissa.remove(0, 1);
    }
    const QByteArray digits = QByteArray(mantissa).replace('.', "");
    if (exponent >= -4 && exponent < 16) {
        QByteArray fixed;
        if (exponent >= 0) {
            QByteArray whole = digits.left(exponent + 1);
            while (whole.size() < exponent + 1) {
                whole.append('0');
            }
            QByteArray fraction = digits.mid(exponent + 1);
            fixed = whole + '.' + (fraction.isEmpty() ? QByteArray("0") : fraction);
        } else {
            fixed = "0." + QByteArray(-exponent - 1, '0') + digits;
        }
        return sign + fixed;
    }
    QByteArray out = sign + digits.left(1);
    if (digits.size() > 1) {
        out += '.' + digits.mid(1);
    }
    out += 'e';
    out += exponent < 0 ? '-' : '+';
    QByteArray power = QByteArray::number(std::abs(exponent));
    if (power.size() < 2) {
        power.prepend('0');
    }
    return out + power;
}

qint64 AsInt(const Json& value, qint64 fallback) {
    switch (value.kind()) {
    case Json::Kind::Bool:
        return value.BoolValue() ? 1 : 0;
    case Json::Kind::Int:
        return value.IntValue();
    case Json::Kind::Float: {
        const double f = value.FloatValue();
        if (!std::isfinite(f)) {
            return fallback;
        }
        const double truncated = std::trunc(f);
        if (truncated < -9223372036854775808.0 || truncated >= 9223372036854775808.0) {
            return fallback;
        }
        return static_cast<qint64>(truncated);
    }
    case Json::Kind::String:
        return ParsePythonInt(value.StringValue()).value_or(fallback);
    default:
        return fallback;
    }
}

QString PyStr(const Json& value) {
    switch (value.kind()) {
    case Json::Kind::Null:
        return QStringLiteral("None");
    case Json::Kind::Bool:
        return value.BoolValue() ? QStringLiteral("True") : QStringLiteral("False");
    case Json::Kind::Int:
        return QString::number(value.IntValue());
    case Json::Kind::Float:
        return QString::fromLatin1(PythonFloat(value.FloatValue()));
    case Json::Kind::String:
        return value.StringValue();
    default:
        return PyRepr(value);
    }
}

bool Truthy(const Json& value) {
    switch (value.kind()) {
    case Json::Kind::Null:
        return false;
    case Json::Kind::Bool:
        return value.BoolValue();
    case Json::Kind::Int:
        return value.IntValue() != 0;
    case Json::Kind::Float:
        return value.FloatValue() != 0;
    case Json::Kind::String:
        return !value.StringValue().isEmpty();
    case Json::Kind::Array:
    case Json::Kind::Object:
        return !value.Items().empty();
    }
    return false;
}

std::vector<Json> Iterate(const Json& value) {
    switch (value.kind()) {
    case Json::Kind::Array:
        return value.Items();
    case Json::Kind::Object: {
        std::vector<Json> keys;
        for (const QString& key : value.Keys()) {
            keys.push_back(Json::String(key));
        }
        return keys;
    }
    case Json::Kind::String: {
        std::vector<Json> characters;
        for (const char32_t c : value.StringValue().toUcs4()) {
            characters.push_back(Json::String(QString::fromUcs4(&c, 1)));
        }
        return characters;
    }
    default:
        throw PythonError("object is not iterable");
    }
}

quint32 Crc32(const QByteArray& data) {
    static const auto Table = [] {
        std::array<quint32, 256> table{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
        return table;
    }();
    quint32 crc = 0xFFFFFFFFu;
    for (const char byte : data) {
        crc = Table[(crc ^ static_cast<quint8>(byte)) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace Bloodborne::Backend

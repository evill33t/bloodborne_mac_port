// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <stdexcept>
#include <vector>

#include <QByteArray>
#include <QList>
#include <QString>

namespace Bloodborne::Backend {

// The game's online services speak JSON (backend/bb_dummy_server.py was their first stand-in).
// This is JSON the way that Python server read and wrote it, so the game sees the same bytes:
// integers and floats stay apart (4 is not 4.0, which QJsonValue merges), objects keep their
// order, and output is json.dumps(separators=(",", ":")), ASCII only.
class Json {
public:
    enum class Kind { Null, Bool, Int, Float, String, Array, Object };

    Json();
    static Json Bool(bool value);
    static Json Int(qint64 value);
    static Json Float(double value);
    static Json String(const QString& value);
    static Json Array();
    static Json Array(std::vector<Json> items);
    static Json Object();

    Kind kind() const {
        return m_kind;
    }
    bool IsNull() const {
        return m_kind == Kind::Null;
    }
    bool IsObject() const {
        return m_kind == Kind::Object;
    }
    bool IsArray() const {
        return m_kind == Kind::Array;
    }
    bool IsString() const {
        return m_kind == Kind::String;
    }
    bool BoolValue() const {
        return m_bool;
    }
    qint64 IntValue() const {
        return m_int;
    }
    double FloatValue() const {
        return m_float;
    }
    const QString& StringValue() const {
        return m_string;
    }
    // An array's items, or an object's values.
    const std::vector<Json>& Items() const {
        return m_items;
    }
    // An object's keys, in order.
    const std::vector<QString>& Keys() const {
        return m_keys;
    }

    // Objects: the value under `key` (the last one when the key repeats), or nullptr.
    const Json* Find(const QString& key) const;
    // dict.get(key, default): `fallback` when the key is missing, the value (maybe null) else.
    Json Get(const QString& key, const Json& fallback = Json()) const;
    bool Has(const QString& key) const {
        return Find(key) != nullptr;
    }
    // Objects: sets `key`, keeping its first place when it is already there.
    Json& Set(const QString& key, Json value);
    // Arrays: adds an item.
    Json& Append(Json value);

    bool operator==(const Json& other) const;

private:
    Kind m_kind = Kind::Null;
    bool m_bool = false;
    qint64 m_int = 0;
    double m_float = 0;
    QString m_string;
    std::vector<Json> m_items;
    std::vector<QString> m_keys;
};

// json.loads of UTF-8 text: nullopt where Python raises (bad UTF-8, a byte order mark, a control
// character in a string, trailing text). NaN, Infinity and -Infinity are read as Python reads them.
// An integer too large for 64 bits is read as a float.
std::optional<Json> ParseJson(const QByteArray& text);

// json.dumps(value, separators=(",", ":")) with ensure_ascii: what the server answers with.
QByteArray DumpJson(const Json& value);
// json.dumps(value) with its default ", " and ": " separators: what the content store keeps.
QByteArray DumpJsonSpaced(const Json& value);

// repr(float), the way json.dumps writes a float: 19.0, 0.1, 1e+16, 1e-05, inf as Infinity.
QByteArray PythonFloat(double value);

// What a Python expression does where it raises: the request is answered without content.
struct PythonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// int(value) where it works, `fallback` where Python's int() raises (as_int in the Python server).
qint64 AsInt(const Json& value, qint64 fallback = 0);
// str(value): "None", "True", "12", "1.5", or the string itself.
QString PyStr(const Json& value);
// bool(value): false for null, false, 0, 0.0, "", [] and {}.
bool Truthy(const Json& value);
// `for item in value`: an array's items, an object's keys, a string's characters. Raises
// PythonError for numbers, booleans and null, which Python cannot iterate.
std::vector<Json> Iterate(const Json& value);

// zlib's CRC-32 (binascii.crc32).
quint32 Crc32(const QByteArray& data);

} // namespace Bloodborne::Backend

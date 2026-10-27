// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The game's online services answered from shadNet must write the same JSON
// bytes the Python backend (json.dumps) wrote, and read requests the way
// json.loads read them.
#include <iostream>

#include "bloodborne_backend_json.h"

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

using namespace Bloodborne::Backend;

QByteArray RoundTrip(const QByteArray &text) {
  const auto parsed = ParseJson(text);
  return parsed ? DumpJson(*parsed) : QByteArray("<error>");
}

} // namespace

int main() {
  // Integers and floats stay apart, as json.loads keeps them (QJsonValue would
  // merge them).
  CHECK(RoundTrip("{\"a\":4,\"b\":4.0,\"c\":-0.5,\"d\":1e2}") ==
        "{\"a\":4,\"b\":4.0,\"c\":-0.5,\"d\":100.0}");
  // Object order is kept; a repeated key keeps its first place and its last
  // value.
  CHECK(RoundTrip("{\"z\":1,\"a\":2,\"z\":3}") == "{\"z\":3,\"a\":2}");
  // ensure_ascii: everything outside printable ASCII is \u-escaped, surrogate
  // pairs included.
  CHECK(RoundTrip("[\"caf\xc3\xa9\",\"\xf0\x9f\x98\x80\",\"\\u007f\","
                  "\"tab\\there\"]") ==
        "[\"caf\\u00e9\",\"\\ud83d\\ude00\",\"\\u007f\",\"tab\\there\"]");
  CHECK(RoundTrip(" [ null , true , false , \"\\/\" ] ") ==
        "[null,true,false,\"/\"]");
  CHECK(RoundTrip("[NaN,Infinity,-Infinity]") == "[NaN,Infinity,-Infinity]");
  CHECK(RoundTrip("123456789012345678901234567890") ==
        "1.2345678901234568e+29");

  // json.loads raises on these; the server then reads the request as {}.
  CHECK(!ParseJson("{\"a\":1} x"));
  CHECK(!ParseJson("{\"a\":\"line\nbreak\"}"));
  CHECK(!ParseJson("\xef\xbb\xbf{}"));
  CHECK(!ParseJson("{\"a\":\xff}"));
  CHECK(!ParseJson("{'a':1}"));
  CHECK(!ParseJson("[1,]"));
  CHECK(!ParseJson(""));
  CHECK(!ParseJson("01"));

  // repr(float).
  CHECK(PythonFloat(19) == "19.0");
  CHECK(PythonFloat(0.1) == "0.1");
  CHECK(PythonFloat(-0.0) == "-0.0");
  CHECK(PythonFloat(100000) == "100000.0");
  CHECK(PythonFloat(1e16) == "1e+16");
  CHECK(PythonFloat(1.5e16) == "1.5e+16");
  CHECK(PythonFloat(123456789012345.6) == "123456789012345.6");
  CHECK(PythonFloat(0.0001) == "0.0001");
  CHECK(PythonFloat(0.00001) == "1e-05");
  CHECK(PythonFloat(2.5e-7) == "2.5e-07");

  // json.dumps() with its default separators, what the content store keeps.
  CHECK(DumpJsonSpaced(*ParseJson("[{\"a\":1,\"b\":[2,3]}]")) ==
        "[{\"a\": 1, \"b\": [2, 3]}]");

  // int(value) the way as_int uses it.
  CHECK(AsInt(Json::Int(7)) == 7);
  CHECK(AsInt(Json::Float(-7.9)) == -7);
  CHECK(AsInt(Json::Bool(true)) == 1);
  CHECK(AsInt(Json::String(QStringLiteral(" 42 "))) == 42);
  CHECK(AsInt(Json::String(QStringLiteral("-1_000"))) == -1000);
  CHECK(AsInt(Json::String(QStringLiteral("+5"))) == 5);
  CHECK(AsInt(Json::String(QStringLiteral("1.5")), -1) == -1);
  CHECK(AsInt(Json::String(QStringLiteral("1__0")), -1) == -1);
  CHECK(AsInt(Json::String(QStringLiteral("")), -1) == -1);
  CHECK(AsInt(Json(), -1) == -1);
  CHECK(AsInt(Json::Array(), -1) == -1);
  CHECK(AsInt(Json::Float(std::numeric_limits<double>::infinity()), -1) == -1);

  // str(value) and bool(value).
  CHECK(PyStr(Json()) == QStringLiteral("None"));
  CHECK(PyStr(Json::Bool(false)) == QStringLiteral("False"));
  CHECK(PyStr(Json::Int(12)) == QStringLiteral("12"));
  CHECK(PyStr(Json::Float(12)) == QStringLiteral("12.0"));
  CHECK(!Truthy(Json::Float(0)));
  CHECK(!Truthy(Json::String(QString())));
  CHECK(!Truthy(Json::Object()));
  CHECK(Truthy(Json::Array({Json()})));

  // Iterating a string gives its characters, a dict its keys, a number raises.
  CHECK(Iterate(Json::String(QStringLiteral("ab"))).size() == 2);
  bool raised = false;
  try {
    Iterate(Json::Int(1));
  } catch (const PythonError &) {
    raised = true;
  }
  CHECK(raised);

  // binascii.crc32.
  CHECK(Crc32("123456789") == 0xCBF43926u);
  CHECK(Crc32("") == 0);

  std::cout << "bloodborne_backend_json: all checks passed\n";
  return 0;
}

#pragma once
#include <cstddef>
#include <cstdint>

// Reading the top-level members of a small JSON object, such as a login
// ({"user":"ann","password":"..."}) or a tag write ({"value":21.5}),
// without building a tree. Nested objects and arrays are skipped over.
// Malformed JSON finds nothing.
namespace HttpJson {

// The raw text of member key's value: "21.5", "true", or a string with
// its quotes. false: no such member, or the JSON is malformed.
bool raw(const char* json, size_t len, const char* key, const char*& value, size_t& valueLen);

// A string member, unescaped (\uXXXX as UTF-8) into out. false: not
// there, not a string, longer than cap - 1, or with a \u0000.
bool string(const char* json, size_t len, const char* key, char* out, size_t cap);

}  // namespace HttpJson

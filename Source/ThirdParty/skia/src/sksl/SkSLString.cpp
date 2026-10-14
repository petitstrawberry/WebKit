/*
 * Copyright 2017 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkStringView.h"
#include "src/sksl/SkSLDefines.h"
#include "src/sksl/SkSLString.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

template <typename RoundtripType, int kFullPrecision>
static std::string to_string_impl(RoundtripType value) {
#if defined(__scarlet__)
    // Locale-independent conversion, including the short representation and
    // full-precision roundtrip fallback. No C printf float extension is needed.
    char buffer[64];
    auto format = [&](int precision) {
        auto result = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value,
                                    std::chars_format::general, precision);
        if (result.ec != std::errc{}) SkUNREACHABLE;
        *result.ptr = '\0';
    };
    format(7);
    auto roundtripped = std::strtod(buffer, nullptr);
    if (value != static_cast<RoundtripType>(roundtripped) && std::isfinite(value)) {
        format(kFullPrecision);
        SkASSERTF(value == static_cast<RoundtripType>(std::strtod(buffer, nullptr)),
                  "%.17g -> %s", static_cast<double>(value), buffer);
    }
    std::string text(buffer);
#else
    std::stringstream buffer;
    buffer.imbue(std::locale::classic());
    buffer.precision(7);
    buffer << value;
    std::string text = buffer.str();

    double roundtripped;
    buffer >> roundtripped;
    if (value != (RoundtripType)roundtripped && std::isfinite(value)) {
        buffer.str({});
        buffer.clear();
        buffer.precision(kFullPrecision);
        buffer << value;
        text = buffer.str();
        SkASSERTF((buffer >> roundtripped, value == (RoundtripType)roundtripped),
                  "%.17g -> %s -> %.17g", value, text.c_str(), roundtripped);
    }

#endif

    // We need to emit a decimal point to distinguish floats from ints.
    if (!skstd::contains(text, '.') && !skstd::contains(text, 'e')) {
        text += ".0";
    }

    return text;
}

std::string skstd::to_string(float value) {
    return to_string_impl<float, 9>(value);
}

std::string skstd::to_string(double value) {
    return to_string_impl<double, 17>(value);
}

bool SkSL::stod(std::string_view s, SKSL_FLOAT* value) {
    std::string str(s.data(), s.size());
#if defined(__scarlet__)
    char* end = nullptr;
    errno = 0;
    auto parsed = std::strtod(str.c_str(), &end);
    if (end == str.c_str() || errno == ERANGE || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
#else
    std::stringstream buffer(str);
    buffer.imbue(std::locale::classic());
    buffer >> *value;
    return !buffer.fail() && std::isfinite(*value);
#endif
}

bool SkSL::stoi(std::string_view s, SKSL_INT* value) {
    if (s.empty()) {
        return false;
    }
    char suffix = s.back();
    if (suffix == 'u' || suffix == 'U') {
        s.remove_suffix(1);
    }
    std::string str(s);  // s is not null-terminated
    const char* strEnd = str.data() + str.length();
    char* p;
    errno = 0;
    unsigned long long result = strtoull(str.data(), &p, /*base=*/0);
    *value = static_cast<SKSL_INT>(result);
    return p == strEnd && errno == 0 && result <= 0xFFFFFFFF;
}

std::string SkSL::String::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string result;
    vappendf(&result, fmt, args);
    va_end(args);
    return result;
}

void SkSL::String::appendf(std::string *str, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vappendf(str, fmt, args);
    va_end(args);
}

void SkSL::String::vappendf(std::string *str, const char* fmt, va_list args) {
    #define BUFFER_SIZE 256
    char buffer[BUFFER_SIZE];
    va_list reuse;
    va_copy(reuse, args);
    int count = vsnprintf(buffer, BUFFER_SIZE, fmt, args);
    if (count < 0) {
        SkDebugf("SkSL::String: vsnprintf reported error.\n");
        va_end(reuse);
        return;
    }
    size_t size = static_cast<size_t>(count);
    if (BUFFER_SIZE >= size + 1) {
        str->append(buffer, size);
    } else {
        auto newBuffer = std::unique_ptr<char[]>(new char[size + 1]);
        vsnprintf(newBuffer.get(), size + 1, fmt, reuse);
        str->append(newBuffer.get(), size);
    }
    va_end(reuse);
}

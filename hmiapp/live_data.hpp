// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef ENTRY_AAD_HMIAPP_LIVE_DATA_HPP
#define ENTRY_AAD_HMIAPP_LIVE_DATA_HPP

#include <cmath>
#include <locale>
#include <sstream>
#include <string>

namespace evo {
namespace app {

namespace live_data {

// Find a direct member of a CDS object, ignoring nested members and quoted values.
inline std::string member(const std::string& object, const char* key) {
    size_t i = object.find_first_not_of(" \t\r\n");
    if (i == std::string::npos || object[i++] != '{') {
        return {};
    }
    std::string result;
    for (;;) {
        i = object.find_first_not_of(" \t\r\n", i);
        if (i == std::string::npos || object[i++] != '"') {
            return {};
        }
        const size_t end = object.find('"', i);
        if (end == std::string::npos) {
            return {};
        }
        const bool match = object.compare(i, end - i, key) == 0;
        i = object.find_first_not_of(" \t\r\n", end + 1);
        if (i == std::string::npos || object[i++] != ':') {
            return {};
        }
        const size_t start = object.find_first_not_of(" \t\r\n", i);
        if (start == std::string::npos) {
            return {};
        }
        std::string closers;
        bool quoted = false;
        for (i = start; i < object.size(); ++i) {
            const char ch = object[i];
            if (quoted) {
                if (ch == '\\') {
                    ++i;
                } else if (ch == '"') {
                    quoted = false;
                }
            } else if (ch == '"') {
                quoted = true;
            } else if (ch == '{' || ch == '[') {
                closers += ch == '{' ? '}' : ']';
            } else if (!closers.empty() && (ch == '}' || ch == ']')) {
                if (ch != closers.back()) {
                    return {};
                }
                closers.pop_back();
            } else if (closers.empty() && (ch == '}' || ch == ',')) {
                if (i == start || (match && !result.empty())) {
                    return {};
                }
                if (match) {
                    result = object.substr(start, i - start);
                }
                if (ch == '}') {
                    return object.find_first_not_of(" \t\r\n", i + 1) == std::string::npos ? result : std::string();
                }
                break;
            }
        }
        if (i >= object.size()) {
            return {};
        }
        ++i;
    }
}

inline bool number(const std::string& value, double& result) {
    if (value.empty() || (value[0] != '-' && (value[0] < '0' || value[0] > '9'))) {
        return false;
    }
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    if (!(stream >> result) || !std::isfinite(result)) {
        return false;
    }
    return (stream >> std::ws).eof();
}

struct Reading {
    double celsius = 0;
    bool available = false;

    void update(const std::string& temperatures, const char* key) {
        // EntryEVO publishes the raw byte minus 48; failed proxy reads also leave the byte at zero.
        // Keep the high-end byte codes out of the display until their meaning is confirmed.
        available = number(member(temperatures, key), celsius) && celsius > -48 && celsius < 206;
    }

    std::wstring text(bool fahrenheit) const {
        if (!available) {
            return {};
        }
        const double degrees = fahrenheit ? celsius * 1.8 + 32 : celsius;
        return std::to_wstring(static_cast<int>(std::round(degrees))) + (fahrenheit ? L" \u00b0F" : L" \u00b0C");
    }

    int gaugePosition() const {
        if (!available) {
            return -1;
        }
        if (celsius <= 0) {
            return 0;
        }
        if (celsius >= 120) {
            return 120;
        }
        return static_cast<int>(std::round(celsius));
    }
};

struct Model {
    Reading oil;
    Reading coolant;
    bool fahrenheit = false;

    void updateTemperatures(const char* payload) {
        const auto temperatures = member(payload, "temperature");
        oil.update(temperatures, "oil");
        coolant.update(temperatures, "engine");
    }

    void updateUnits(const char* payload) {
        double unit = 0;
        fahrenheit = number(member(member(payload, "units"), "temperature"), unit) && unit == 2;
    }
};

}  // namespace live_data
}  // namespace app
}  // namespace evo

#endif

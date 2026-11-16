/**********************************************************************************************************************
    > File: mini_json.h
    > 轻量 JSON 解析器（零依赖，面向控制面小报文：终端链路状态上报等）
    > 支持子集：object/array/string/number/bool/null，UTF-8 原样存取。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_MINI_JSON_H
#define VWISE_CLOUD_MINI_JSON_H

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vwise {
namespace cloud {

class Json {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };

    Type type = NUL;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    static Json parse(const std::string &text, bool *ok = nullptr) {
        size_t pos = 0;
        Json j = parseValue(text, pos);
        skipWs(text, pos);
        bool good = !j.errored && pos == text.size();
        if (ok) *ok = good;
        return j;
    }

    /* 便捷取值（不存在返回默认） */
    double get_num(const std::string &key, double def) const {
        auto it = obj.find(key);
        return (it != obj.end() && it->second.type == NUM) ? it->second.num : def;
    }
    std::string get_str(const std::string &key, const std::string &def) const {
        auto it = obj.find(key);
        return (it != obj.end() && it->second.type == STR) ? it->second.str : def;
    }

private:
    bool errored = false;

    static void skipWs(const std::string &s, size_t &pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
            ++pos;
    }
    static void fail(Json &j) { j.errored = true; }

    static Json parseValue(const std::string &s, size_t &pos) {
        Json j;
        skipWs(s, pos);
        if (pos >= s.size()) { fail(j); return j; }
        char c = s[pos];
        if (c == '{') return parseObj(s, pos);
        if (c == '[') return parseArr(s, pos);
        if (c == '"') {
            j.type = STR;
            if (!parseStr(s, pos, j.str)) fail(j);
            return j;
        }
        if (s.compare(pos, 4, "true") == 0) {
            j.type = BOOL; j.b = true; pos += 4; return j;
        }
        if (s.compare(pos, 5, "false") == 0) {
            j.type = BOOL; j.b = false; pos += 5; return j;
        }
        if (s.compare(pos, 4, "null") == 0) {
            j.type = NUL; pos += 4; return j;
        }
        /* number */
        {
            char *end = nullptr;
            double v = strtod(s.c_str() + pos, &end);
            if (end == s.c_str() + pos) { fail(j); return j; }
            j.type = NUM;
            j.num = v;
            pos = (size_t)(end - s.c_str());
        }
        return j;
    }

    static bool parseStr(const std::string &s, size_t &pos, std::string &out) {
        out.clear();
        if (pos >= s.size() || s[pos] != '"') return false;
        ++pos;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos >= s.size()) return false;
                char e = s[pos++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (pos + 4 > s.size()) return false;
                        unsigned cp = (unsigned)strtoul(s.substr(pos, 4).c_str(), nullptr, 16);
                        pos += 4;
                        /* UTF-8 编码（BMP 内足够） */
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    static Json parseObj(const std::string &s, size_t &pos) {
        Json j;
        j.type = OBJ;
        ++pos; /* '{' */
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; return j; }
        while (true) {
            skipWs(s, pos);
            std::string key;
            if (!parseStr(s, pos, key)) { fail(j); return j; }
            skipWs(s, pos);
            if (pos >= s.size() || s[pos] != ':') { fail(j); return j; }
            ++pos;
            Json v = parseValue(s, pos);
            if (v.errored) { fail(j); return j; }
            j.obj[key] = v;
            skipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; return j; }
            fail(j);
            return j;
        }
    }

    static Json parseArr(const std::string &s, size_t &pos) {
        Json j;
        j.type = ARR;
        ++pos; /* '[' */
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') { ++pos; return j; }
        while (true) {
            Json v = parseValue(s, pos);
            if (v.errored) { fail(j); return j; }
            j.arr.push_back(v);
            skipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == ']') { ++pos; return j; }
            fail(j);
            return j;
        }
    }
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_MINI_JSON_H

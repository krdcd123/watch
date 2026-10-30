// mini_json.h - tiny dependency-free JSON value, parser and serializer.
// Object key order is preserved and unknown fields round-trip untouched, which
// matters here: we must never drop fields Stremio stored on a library item.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mj {

class Value {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value boolean(bool v) { Value x; x.type_ = Bool; x.b_ = v; return x; }
    static Value number(double v) { Value x; x.type_ = Number; x.n_ = v; return x; }
    static Value string(std::string v) { Value x; x.type_ = String; x.s_ = std::move(v); return x; }
    static Value array() { Value x; x.type_ = Array; return x; }
    static Value object() { Value x; x.type_ = Object; return x; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Null; }
    bool isArray() const { return type_ == Array; }
    bool isObject() const { return type_ == Object; }
    bool isString() const { return type_ == String; }

    // ---- scalar access (with defaults when type differs) ----
    bool asBool(bool def = false) const { return type_ == Bool ? b_ : def; }
    double asNum(double def = 0) const { return type_ == Number ? n_ : def; }
    int64_t asInt(int64_t def = 0) const { return type_ == Number ? (int64_t)std::llround(n_) : def; }
    std::string asStr(const std::string& def = "") const { return type_ == String ? s_ : def; }

    // ---- array ----
    std::vector<Value>& items() { return vals_; }
    const std::vector<Value>& items() const { return vals_; }
    Value& push(Value v) { if (type_ != Array) reset(Array); vals_.push_back(std::move(v)); return vals_.back(); }

    // ---- object ----
    const Value* find(const std::string& k) const {
        for (size_t i = 0; i < keys_.size(); i++) if (keys_[i] == k) return &vals_[i];
        return nullptr;
    }
    Value* find(const std::string& k) {
        for (size_t i = 0; i < keys_.size(); i++) if (keys_[i] == k) return &vals_[i];
        return nullptr;
    }
    Value& set(const std::string& k, Value v) {
        if (type_ != Object) reset(Object);
        if (Value* e = find(k)) { *e = std::move(v); return *e; }
        keys_.push_back(k);
        vals_.push_back(std::move(v));
        return vals_.back();
    }
    // Returns a reference to the member, creating an object member if needed.
    Value& operator[](const std::string& k) {
        if (type_ != Object) reset(Object);
        if (Value* e = find(k)) return *e;
        return set(k, Value());
    }
    void erase(const std::string& k) {
        for (size_t i = 0; i < keys_.size(); i++)
            if (keys_[i] == k) { keys_.erase(keys_.begin() + i); vals_.erase(vals_.begin() + i); return; }
    }
    const std::vector<std::string>& keys() const { return keys_; }

    std::string getStr(const std::string& k, const std::string& def = "") const {
        const Value* v = find(k); return v ? v->asStr(def) : def;
    }
    int64_t getInt(const std::string& k, int64_t def = 0) const {
        const Value* v = find(k); return v ? v->asInt(def) : def;
    }
    bool getBool(const std::string& k, bool def = false) const {
        const Value* v = find(k); return v ? v->asBool(def) : def;
    }

    // ---- serialize ----
    std::string dump() const { std::string o; dumpTo(o); return o; }

    void dumpTo(std::string& o) const {
        switch (type_) {
        case Null: o += "null"; break;
        case Bool: o += b_ ? "true" : "false"; break;
        case Number: {
            char buf[40];
            if (std::isfinite(n_) && n_ == std::floor(n_) && std::fabs(n_) < 9.0e15)
                snprintf(buf, sizeof buf, "%lld", (long long)n_);
            else if (std::isfinite(n_))
                snprintf(buf, sizeof buf, "%.17g", n_);
            else
                snprintf(buf, sizeof buf, "null");
            o += buf;
            break;
        }
        case String: dumpString(o, s_); break;
        case Array:
            o += '[';
            for (size_t i = 0; i < vals_.size(); i++) { if (i) o += ','; vals_[i].dumpTo(o); }
            o += ']';
            break;
        case Object:
            o += '{';
            for (size_t i = 0; i < keys_.size(); i++) {
                if (i) o += ',';
                dumpString(o, keys_[i]);
                o += ':';
                vals_[i].dumpTo(o);
            }
            o += '}';
            break;
        }
    }

private:
    void reset(Type t) { type_ = t; keys_.clear(); vals_.clear(); }

    static void dumpString(std::string& o, const std::string& s) {
        o += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
            }
        }
        o += '"';
    }

    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<std::string> keys_;  // object keys (parallel to vals_)
    std::vector<Value> vals_;        // array items or object values
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    Parser(const std::string& t) : p_(t.c_str()), e_(t.c_str() + t.size()) {}

    bool parse(Value& out, std::string* err) {
        ws();
        if (!value(out, 0)) { if (err) *err = err_; return false; }
        ws();
        if (p_ != e_) { if (err) *err = "trailing characters"; return false; }
        return true;
    }

private:
    const char* p_;
    const char* e_;
    std::string err_;

    bool fail(const char* m) { err_ = m; return false; }
    void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++; }

    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(e_ - p_) >= n && memcmp(p_, s, n) == 0) { p_ += n; return true; }
        return false;
    }

    bool value(Value& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (p_ >= e_) return fail("unexpected end");
        switch (*p_) {
        case '{': return object(out, depth);
        case '[': return array(out, depth);
        case '"': { std::string s; if (!str(s)) return false; out = Value::string(std::move(s)); return true; }
        case 't': if (lit("true")) { out = Value::boolean(true); return true; } return fail("bad literal");
        case 'f': if (lit("false")) { out = Value::boolean(false); return true; } return fail("bad literal");
        case 'n': if (lit("null")) { out = Value(); return true; } return fail("bad literal");
        default: return number(out);
        }
    }

    bool number(Value& out) {
        char* end = nullptr;
        double d = strtod(p_, &end);
        if (end == p_ || end > e_) return fail("bad number");
        p_ = end;
        out = Value::number(d);
        return true;
    }

    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }

    bool hex4(uint32_t& v) {
        if (e_ - p_ < 4) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool str(std::string& out) {
        p_++;  // opening quote
        while (p_ < e_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= e_) break;
            char x = *p_++;
            switch (x) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Value& out, int depth) {
        p_++;
        out = Value::array();
        ws();
        if (p_ < e_ && *p_ == ']') { p_++; return true; }
        for (;;) {
            Value v;
            ws();
            if (!value(v, depth + 1)) return false;
            out.items().push_back(std::move(v));
            ws();
            if (p_ >= e_) return fail("unterminated array");
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == ']') { p_++; return true; }
            return fail("expected , or ]");
        }
    }

    bool object(Value& out, int depth) {
        p_++;
        out = Value::object();
        ws();
        if (p_ < e_ && *p_ == '}') { p_++; return true; }
        for (;;) {
            ws();
            if (p_ >= e_ || *p_ != '"') return fail("expected key");
            std::string k;
            if (!str(k)) return false;
            ws();
            if (p_ >= e_ || *p_ != ':') return fail("expected :");
            p_++;
            ws();
            Value v;
            if (!value(v, depth + 1)) return false;
            out.set(k, std::move(v));
            ws();
            if (p_ >= e_) return fail("unterminated object");
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == '}') { p_++; return true; }
            return fail("expected , or }");
        }
    }
};

inline bool parse(const std::string& text, Value& out, std::string* err = nullptr) {
    Parser p(text);
    return p.parse(out, err);
}

}  // namespace mj

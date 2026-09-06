#pragma once

// Minimal Arduino String for the punkmesh port: heap-backed (the C++
// allocator set in mcshim.cpp → protocol pool), covering exactly the surface
// punkmesh.cpp uses — construction, concatenation, c_str/length. NOT
// layout-compatible with the firmware's Arduino String: String values must
// NEVER cross the module boundary (String-flavored mstore calls go through
// the mcs_* C bridges instead).

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

class String {
    char* _buf;
    unsigned _len;

    void assign(const char* s, unsigned n) {
        char* nb = new char[n + 1];
        if (s && n) memcpy(nb, s, n);
        nb[n] = '\0';
        delete[] _buf;
        _buf = nb;
        _len = n;
    }

public:
    String() : _buf(nullptr), _len(0) { assign("", 0); }
    String(const char* s) : _buf(nullptr), _len(0) {
        assign(s ? s : "", s ? (unsigned)strlen(s) : 0);
    }
    String(const String& o) : _buf(nullptr), _len(0) { assign(o._buf, o._len); }
    String(int v) : _buf(nullptr), _len(0) {
        char b[16];
        snprintf(b, sizeof(b), "%d", v);
        assign(b, (unsigned)strlen(b));
    }
    String(unsigned v) : _buf(nullptr), _len(0) {
        char b[16];
        snprintf(b, sizeof(b), "%u", v);
        assign(b, (unsigned)strlen(b));
    }
    ~String() { delete[] _buf; }

    String& operator=(const String& o) {
        if (this != &o) assign(o._buf, o._len);
        return *this;
    }
    String& operator=(const char* s) {
        assign(s ? s : "", s ? (unsigned)strlen(s) : 0);
        return *this;
    }

    String& operator+=(const char* s) {
        if (s && *s) {
            unsigned sl = (unsigned)strlen(s);
            char* nb = new char[_len + sl + 1];
            memcpy(nb, _buf, _len);
            memcpy(nb + _len, s, sl + 1);
            delete[] _buf;
            _buf = nb;
            _len += sl;
        }
        return *this;
    }
    String& operator+=(const String& o) { return (*this += o.c_str()); }
    String& operator+=(char c) {
        char b[2] = { c, 0 };
        return (*this += b);
    }

    friend String operator+(String a, const char* b)   { a += b; return a; }
    friend String operator+(String a, const String& b) { a += b; return a; }
    friend String operator+(const char* a, const String& b) {
        String r(a);
        r += b;
        return r;
    }

    bool operator==(const char* s) const   { return strcmp(_buf, s ? s : "") == 0; }
    bool operator==(const String& o) const { return strcmp(_buf, o._buf) == 0; }
    bool operator!=(const char* s) const   { return !(*this == s); }

    const char* c_str() const  { return _buf; }
    unsigned length() const    { return _len; }
    bool isEmpty() const       { return _len == 0; }
};

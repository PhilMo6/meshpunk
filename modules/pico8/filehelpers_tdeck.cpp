// T-Deck replacement for fake-08's filehelpers.cpp.
// Uses C-style fopen/fread instead of std::ifstream to avoid pulling
// in the massive iostream/locale/codecvt dependency tree from libstdc++.

#include "fake08-src/source/filehelpers.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
    extern void* malloc(size_t);
    extern void  free(void*);
}

std::string get_file_contents(std::string filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) return "";

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return ""; }
    fseek(f, 0, SEEK_SET);

    std::string contents(sz, '\0');
    size_t rd = fread(&contents[0], 1, sz, f);
    fclose(f);
    contents.resize(rd);
    return contents;
}

std::vector<char> get_file_as_buffer(std::string filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) return std::vector<char>();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return std::vector<char>(); }
    fseek(f, 0, SEEK_SET);

    std::vector<char> buffer(sz);
    size_t rd = fread(buffer.data(), 1, sz, f);
    fclose(f);
    buffer.resize(rd);
    return buffer;
}

std::string get_first_four_chars(std::string filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) return "";

    char buf[4];
    size_t rd = fread(buf, 1, 4, f);
    fclose(f);
    return std::string(buf, rd);
}

std::string getDirectory(const std::string& fname) {
    size_t pos = fname.find_last_of("\\/");
    return (std::string::npos == pos) ? "" : fname.substr(0, pos);
}

bool isAbsolutePath(std::string const &path) {
    if (path.empty()) return false;
    if (path[0] == '/') return true;
    size_t colonPos = path.find_first_of(":");
    size_t slashPos = path.find_first_of("/");
    size_t backSlashPos = path.find_first_of("\\");
    if (colonPos != std::string::npos &&
        (slashPos == (colonPos + 1) || backSlashPos == (colonPos + 1)))
        return true;
    return false;
}

std::string getFileExtension(std::string const &path) {
    size_t pos = path.find_last_of(".");
    return (std::string::npos == pos) ? "" : path.substr(pos);
}

bool hasEnding(std::string const &fullString, std::string const &ending) {
    if (fullString.length() >= ending.length())
        return (0 == fullString.compare(fullString.length() - ending.length(), ending.length(), ending));
    return false;
}

bool isHiddenFile(std::string const &fullString) {
    if (fullString.length() >= 2)
        return (0 == fullString.compare(0, 2, "._"));
    return false;
}

bool isCartFile(std::string const &fullString) {
    return !isHiddenFile(fullString) &&
        (hasEnding(fullString, ".p8") || hasEnding(fullString, ".png"));
}

bool isCPostFile(std::string const &fullString) {
    return !isHiddenFile(fullString) &&
        fullString.rfind("cpost", 0) == 0;
}

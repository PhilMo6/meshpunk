#include <string>

struct charset
{
    // Convert between UTF-8 strings and 8-bit PICO-8 strings
    static std::string utf8_to_pico8(std::string const &str);
    static std::string pico8_to_utf8(std::string const &str);

    // Map uppercase letters to PICO-8 glyphs
    static std::string upper_to_emoji(std::string str);
};

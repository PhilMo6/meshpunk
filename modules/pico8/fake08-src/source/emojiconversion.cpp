
//
//  ZEPTO-8 — Fantasy console emulator
//
//  Copyright © 2016—2020 Sam Hocevar <sam@hocevar.net>
//
//  This program is free software. It comes without any warranty, to
//  the extent permitted by applicable law. You can redistribute it
//  and/or modify it under the terms of the Do What the Fuck You Want
//  to Public License, Version 2, as published by the WTFPL Task Force.
//  See http://www.wtfpl.net/ for more details.
//

// T-Deck stub: no emoji keyboard, so UTF-8 ↔ PICO-8 conversions are
// identity pass-throughs.  This removes heavy deps on <regex>, <locale>,
// <codecvt>, and <map>.

#include "emojiconversion.h"

std::string charset::utf8_to_pico8(std::string const &str)
{
    return str;
}

std::string charset::pico8_to_utf8(std::string const &str)
{
    return str;
}

std::string charset::upper_to_emoji(std::string str)
{
    return str;
}

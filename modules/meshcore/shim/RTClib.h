#pragma once

// Minimal DateTime for the punkmesh port (one call site: log-date
// formatting). Epoch → civil date, standard days-from-civil inverse.

#include <stdint.h>

class DateTime {
    uint16_t _y;
    uint8_t  _mo, _d, _h, _mi, _s;

public:
    explicit DateTime(uint32_t epoch) {
        uint32_t days = epoch / 86400;
        uint32_t rem  = epoch % 86400;
        _h  = (uint8_t)(rem / 3600);
        _mi = (uint8_t)((rem % 3600) / 60);
        _s  = (uint8_t)(rem % 60);
        // civil-from-days (Howard Hinnant), era-based, valid for epoch dates
        int32_t z = (int32_t)days + 719468;
        int32_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int32_t y = (int32_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp < 10 ? mp + 3 : mp - 9;
        _y  = (uint16_t)(y + (m <= 2));
        _mo = (uint8_t)m;
        _d  = (uint8_t)d;
    }

    uint16_t year() const   { return _y; }
    uint8_t  month() const  { return _mo; }
    uint8_t  day() const    { return _d; }
    uint8_t  hour() const   { return _h; }
    uint8_t  minute() const { return _mi; }
    uint8_t  second() const { return _s; }
};

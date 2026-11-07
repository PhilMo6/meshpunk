// Single-precision math functions MicroPython's math module uses that the
// firmware does not export, built on the ones it does (expf, logf, sqrtf,
// atan2f, floorf, fabsf, powf, sinf). A static newlib libm cannot be linked
// into a PIC module: its error-handling objects carry pointer tables in
// read-only data.

#include <math.h>
#include <stdint.h>
#include <string.h>

float truncf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;          // |x| >= 2^23, inf or NaN
    float t = (float)(int32_t)x;
    return (t == 0.0f && x < 0.0f) ? -0.0f : t;
}

float modff(float x, float* iptr) {
    float i = truncf(x);
    *iptr = i;
    if (isinf(x)) return x < 0.0f ? -0.0f : 0.0f;
    return x - i;
}

// Round to nearest, ties to even (the default FP rounding mode).
float nearbyintf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    float r = floorf(x + 0.5f);
    if (r - x == 0.5f && fmodf(r, 2.0f) != 0.0f) r -= 1.0f;
    return (r == 0.0f && x < 0.0f) ? -0.0f : r;
}

float atanf(float x) {
    return atan2f(x, 1.0f);
}

float expm1f(float x) {
    if (fabsf(x) < 1e-5f) return x + 0.5f * x * x;
    return expf(x) - 1.0f;
}

float sinhf(float x) {
    if (fabsf(x) < 1e-4f) return x;
    float e = expf(x);
    return 0.5f * (e - 1.0f / e);
}

float coshf(float x) {
    float e = expf(x);
    return 0.5f * (e + 1.0f / e);
}

float tanhf(float x) {
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return -1.0f;
    if (fabsf(x) < 1e-4f) return x;
    float e = expf(2.0f * x);
    return (e - 1.0f) / (e + 1.0f);
}

float asinhf(float x) {
    float a = fabsf(x);
    if (a < 1e-4f) return x;
    float r = logf(a + sqrtf(a * a + 1.0f));
    return x < 0.0f ? -r : r;
}

float acoshf(float x) {
    return logf(x + sqrtf(x * x - 1.0f));
}

float atanhf(float x) {
    if (fabsf(x) < 1e-4f) return x;
    return 0.5f * logf((1.0f + x) / (1.0f - x));
}

// erf: Abramowitz & Stegun 7.1.26 (max error 1.5e-7).
float erff(float x) {
    float a = fabsf(x);
    float t = 1.0f / (1.0f + 0.3275911f * a);
    float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t
                       - 0.284496736f) * t + 0.254829592f) * t * expf(-a * a);
    return x < 0.0f ? -y : y;
}

float erfcf(float x) {
    return 1.0f - erff(x);
}

// Gamma: Lanczos approximation (g = 7, n = 9) with the reflection formula.
static const float LANCZOS[9] = {
    0.99999999999980993f, 676.5203681218851f, -1259.1392167224028f,
    771.32342877765313f, -176.61502916214059f, 12.507343278686905f,
    -0.13857109526572012f, 9.9843695780195716e-6f, 1.5056327351493116e-7f,
};
#define PI_F 3.14159265358979f

float tgammaf(float x) {
    if (x < 0.5f) return PI_F / (sinf(PI_F * x) * tgammaf(1.0f - x));
    x -= 1.0f;
    float a = LANCZOS[0];
    float t = x + 7.5f;
    for (int i = 1; i < 9; i++) a += LANCZOS[i] / (x + (float)i);
    return sqrtf(2.0f * PI_F) * powf(t, x + 0.5f) * expf(-t) * a;
}

float lgammaf(float x) {
    if (x < 0.5f) return logf(PI_F / fabsf(sinf(PI_F * x))) - lgammaf(1.0f - x);
    x -= 1.0f;
    float a = LANCZOS[0];
    float t = x + 7.5f;
    for (int i = 1; i < 9; i++) a += LANCZOS[i] / (x + (float)i);
    return 0.5f * logf(2.0f * PI_F) + (x + 0.5f) * logf(t) - t + logf(a);
}

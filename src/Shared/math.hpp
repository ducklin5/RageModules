#pragma once
#include <cmath>

inline auto clamp(double value, double min = 0.L, double max = 0.L) -> double {
    return std::fmax(std::fmin(value, max), min);
}

inline auto lerp(double a, double b, double t) -> double {
    return a + (b - a) * t;
}

inline auto lerp(double a, double b, double t, double min, double max) -> double {
    return clamp(lerp(a, b, t), min, max);
}

inline auto sign_round(double value, double sign) -> double {
    return sign > 0 ? std::ceil(value)
        : sign < 0 ? std::floor(value)
            : value;
}

struct DeltaAdjustment {
    double more;
    double less;
    double actual;
};

inline auto delta_adjust(double value, double delta) -> DeltaAdjustment {
    double newValue = value + delta;
    double more = sign_round(newValue, delta);
    double less = sign_round(newValue, -delta);
    return { more, less, newValue };
}
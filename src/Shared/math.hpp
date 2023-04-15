#pragma once
#include <cmath>

inline auto clamp(double value, double min = 0.L, double max = 0.L) -> double {
    return std::fmax(std::fmin(value, max), min);
}
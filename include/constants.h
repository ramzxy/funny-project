#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <cstdint>


constexpr uint32_t BUFFER_SIZE = 4096; 
constexpr uint32_t BUFFER_MASK = BUFFER_SIZE - 1;

// CUBIC Constants
constexpr double CUBIC_C = 0.4;
constexpr double CUBIC_BETA = 0.7; // Backoff factor (30% reduction)

#endif // CONSTANTS_H

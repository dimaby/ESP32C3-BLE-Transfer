#ifndef COMMON_H
#define COMMON_H

#include <Arduino.h>

// Stringify macros for converting values to strings during preprocessing
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define DEBUG

// If the DEBUG macro is defined, enable logging
#ifdef DEBUG
    #define LOG_INFO(...) do { \
        Serial.printf(__VA_ARGS__); \
        Serial.println(); \
    } while (0)
    
    #define LOG_ERROR(...) do { \
        Serial.printf(__VA_ARGS__); \
        Serial.println(); \
    } while (0)
#else
    // In production build, logging is disabled
    #define LOG_INFO(...)
    #define LOG_ERROR(...)
#endif

#endif // COMMON_H

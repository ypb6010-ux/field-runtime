#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #if defined(CORE_BUILDING_LIBRARY)
        #define CORE_EXPORT __declspec(dllexport)
    #else
        #define CORE_EXPORT __declspec(dllimport)
    #endif
    #define CORE_HIDDEN
#else
    #if defined(CORE_BUILDING_LIBRARY)
        #define CORE_EXPORT __attribute__((visibility("default")))
    #else
        #define CORE_EXPORT
    #endif
    #define CORE_HIDDEN __attribute__((visibility("hidden")))
#endif

#define CORE_DISABLE_COPY(Class)                  \
    Class(const Class&)            = delete;      \
    Class& operator=(const Class&) = delete;

#define CORE_DISABLE_MOVE(Class)                  \
    Class(Class&&)            = delete;           \
    Class& operator=(Class&&) = delete;

#define CORE_DISABLE_COPY_MOVE(Class)             \
    CORE_DISABLE_COPY(Class)                      \
    CORE_DISABLE_MOVE(Class)

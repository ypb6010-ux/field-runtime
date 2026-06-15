// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

// CORE_STATIC_BUILD: the Qt-free FieldRuntimeBase static library — no dll
// import/export decoration (its symbols are linked directly by consumers, e.g.
// the without-Qt gateway). The Qt Core SHARED library still uses the dllexport
// path below via CORE_BUILDING_LIBRARY.
#if defined(CORE_STATIC_BUILD)
    #define CORE_EXPORT
    #define CORE_HIDDEN
#elif defined(_WIN32) || defined(_WIN64)
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

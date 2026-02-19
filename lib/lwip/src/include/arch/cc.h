/*
 * lwIP Architecture Definitions for TinyPAN
 * 
 * This file provides platform-specific types and macros for lwIP.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/* Define basic types */
typedef uint8_t     u8_t;
typedef int8_t      s8_t;
typedef uint16_t    u16_t;
typedef int16_t     s16_t;
typedef uint32_t    u32_t;
typedef int32_t     s32_t;

/* Pointer-sized types */
typedef uintptr_t   mem_ptr_t;

/* Format specifiers for printf */
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

/* ============================================================================
 * Compiler Hints
 * ============================================================================ */

/* Packed structures */
#if defined(__GNUC__) || defined(__TCC__)
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#elif defined(_MSC_VER)
#define PACK_STRUCT_BEGIN   __pragma(pack(push, 1))
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END     __pragma(pack(pop))
#define PACK_STRUCT_FIELD(x) x
#else
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#endif

/* ============================================================================
 * Byte Order
 * ============================================================================ */

/* We assume little-endian for x86/x64 Windows */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* ============================================================================
 * Random Number Generator
 * ============================================================================ */

#define LWIP_RAND() ((u32_t)rand())

/* ============================================================================
 * Debug Output
 * ============================================================================ */

#ifndef LWIP_PLATFORM_DIAG
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while(0)
#endif

#ifndef LWIP_PLATFORM_ASSERT
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("Assertion \"%s\" failed at line %d in %s\n", x, __LINE__, __FILE__); \
    abort(); \
} while(0)
#endif

#endif /* LWIP_ARCH_CC_H */

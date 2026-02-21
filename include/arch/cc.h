/*
 * TinyPAN lwIP Architecture Configuration
 * 
 * Provides compiler-specific types and diagnostic macros required by
 * the lwIP stack. Since we are using standard C99/GCC, this is minimal.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Endianness */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif

#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN // x86/ARM are usually little endian
#endif

/* Standard diagnostic macros used by lwIP */
#define LWIP_PLATFORM_DIAG(x)   do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    printf("Assertion \"%s\" failed at line %d in %s\n", x, __LINE__, __FILE__); \
    abort(); \
} while(0)

/* Format strings for printf */
#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "zu"

/* Compiler hints */
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

#endif /* LWIP_ARCH_CC_H */

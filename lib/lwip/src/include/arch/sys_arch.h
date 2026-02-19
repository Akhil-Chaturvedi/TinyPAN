/*
 * lwIP System Architecture for NO_SYS mode
 * 
 * When running without an OS, this file provides stub implementations.
 */

#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

/* No OS - lightweight protection not needed */
#define SYS_LIGHTWEIGHT_PROT 0

typedef int sys_prot_t;

#endif /* LWIP_ARCH_SYS_ARCH_H */

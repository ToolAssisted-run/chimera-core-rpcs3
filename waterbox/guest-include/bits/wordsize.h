/* glibc's private header, wanted by code that assumes glibc; the guest is LP64 */
#pragma once
#define __WORDSIZE 64
#define __WORDSIZE_TIME64_COMPAT32 1
#define __SYSCALL_WORDSIZE 64

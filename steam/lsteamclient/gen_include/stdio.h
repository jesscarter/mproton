/* mproton: stub for gen_wrapper.py, see README.md */
#ifndef __MPROTON_GEN_STDIO_H
#define __MPROTON_GEN_STDIO_H
#define __need_size_t
#define __need_NULL
#include <stddef.h>
typedef __builtin_va_list va_list;
typedef struct __mproton_FILE FILE;
#ifdef __cplusplus
extern "C" {
#endif
int printf( const char *, ... );
int sprintf( char *, const char *, ... );
int snprintf( char *, size_t, const char *, ... );
int _snprintf( char *, size_t, const char *, ... );
int vsnprintf( char *, size_t, const char *, va_list );
int sscanf( const char *, const char *, ... );
#ifdef __cplusplus
}
#endif
#endif

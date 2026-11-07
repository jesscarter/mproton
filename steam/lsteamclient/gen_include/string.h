/* mproton: stub for gen_wrapper.py, see README.md */
#ifndef __MPROTON_GEN_STRING_H
#define __MPROTON_GEN_STRING_H
#define __need_size_t
#define __need_NULL
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy( void *, const void *, size_t );
void *memset( void *, int, size_t );
int memcmp( const void *, const void *, size_t );
size_t strlen( const char * );
char *strncpy( char *, const char *, size_t );
int strcmp( const char *, const char * );
int strncmp( const char *, const char *, size_t );
#ifdef __cplusplus
}
#endif
#endif

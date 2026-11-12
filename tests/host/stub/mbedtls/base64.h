#pragma once
#include <stddef.h>
// Not reached: the auth test loads its key directly.
static inline int mbedtls_base64_decode(unsigned char *d, size_t dl, size_t *ol, const unsigned char *s, size_t sl)
{ (void)d; (void)dl; (void)s; (void)sl; *ol = 0; return -1; }

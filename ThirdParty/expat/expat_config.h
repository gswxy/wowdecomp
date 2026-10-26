/* [wowport] expat_config.h for the client build (replaces the configure-generated
   file shipped in the 2.9.0 tarball). Settings carried over from the 1.95.5
   winconfig.h the client used: XML_NS, XML_DTD, XML_CONTEXT_BYTES 1024, little endian. */

#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234

#define HAVE_MEMMOVE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

#define XML_NS 1
#define XML_DTD 1
#define XML_GE 1
#define XML_CONTEXT_BYTES 1024

/* Entropy for the hash salt: rand_s on Windows (built in), /dev/urandom elsewhere. */
#if ! defined(_WIN32)
#  define XML_DEV_URANDOM 1
#endif

#define PACKAGE "expat"
#define PACKAGE_NAME "expat"
#define PACKAGE_VERSION "2.9.0"

#endif /* EXPAT_CONFIG_H */

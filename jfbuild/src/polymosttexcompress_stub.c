// Stub implementation of polymosttexcompress for Xbox.
// The real implementation (polymosttexcompress.cc) uses libsquish (C++)
// which is not available on nxdk. Texture compression is disabled.

#include "build.h"

#if USE_POLYMOST && USE_OPENGL

int ptcompress_getstorage(int width, int height, int format)
{
	(void)width; (void)height; (void)format;
	return 0;
}

int ptcompress_compress(void *bgra, int width, int height, unsigned char *output, int format)
{
	(void)bgra; (void)width; (void)height; (void)output; (void)format;
	return -1;  // Compression not available.
}

#endif

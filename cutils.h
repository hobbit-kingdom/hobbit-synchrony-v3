#ifndef __CUTILS_H__
#define __CUTILS_H__

#include <cstring>

// copy C-string into a buffer, truncating if it's too long
// maxlen = size of buffer in bytes
// return number of bytes written
static int strlcpy(char *dest, const char *src, size_t maxlen)
{
	const char *end = (const char *)memchr(src, '\0', maxlen);
	if(end) {
		memcpy(dest, src, (end - src) + 1);
		return (end - src) + 1;
	} else {
		memcpy(dest, src, maxlen - 1);
		dest[maxlen-1] = '\0';
		return maxlen;
	}
}

// lazy version, accepts an actual array of known length as first parameter, then automatically uses it's length as maxlen
template<size_t N> int strlcpy(char (&dest)[N], const char *src)
{
	return strlcpy(dest, src, N);
}

#endif

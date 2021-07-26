#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <ctype.h>

size_t
parseline(char *base, char *dst, size_t dstsz)
{
	char *s, *t, *separator;
	size_t len;

	/*
	 * Do we have a separator? If not, return NULL.
	 */
	s = base;
	while (*s != '\0' && *s != '\n')
		s++;
	if (*s == '\0') {
		return -1;
	} else {
	}
	separator = s;

	/*	
	 * We had a separator. Extract the line.
	 */
	s = base;
	len = 0;
	while (s != separator && ++len < dstsz - 1) {
		if (iscntrl(*s) || !isascii(*s)) {
			len--;
			s++;
		} else
			*dst++ = *s++;
	}
	*dst = '\0';

	/*
	 * Overwrite old base with the remaining content, if any.
	 */
	s = base;
	t = separator + 1;
	while (*t != '\0')
		*s++ = *t++;
	*s = '\0';

	return separator - base + 1;
}

#ifdef TEST
#include <ctype.h>
#include <unistd.h>
#include <err.h>

int main(int argc, char *argv[])
{
	int n;
	int len;
	int sz;
	char buf[1024], dst[1024];

	sz = 0;
	for (;;) {
		if (sizeof(buf) - 1 - sz <= 0) {
			warnx("discarded %d bytes; too long line", sz);
			sz = 0;
		}
		n = read(0, &buf[sz], sizeof(buf) - 1 - sz);
		if (n > 0) {
			sz += n;
			buf[sz] = '\0';
			while ((len = parseline(buf, dst, sizeof(dst)))
			    != -1) {
				puts(dst);
				sz -= len;
			}
		} else if (n < 0)
			err(1, "read");
		else if (n == 0)
			break;
	}
}
#endif

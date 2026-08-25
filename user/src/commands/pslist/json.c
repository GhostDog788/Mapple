#include <commands/pslist/json.h>

#include <errno.h>
#include <stdio.h>

int mapple_pslist_print_json_indent(unsigned int width)
{
	while (width--) {
		if (putchar(' ') == EOF)
			return EIO;
	}
	return 0;
}

int mapple_pslist_print_json_bytes(const unsigned char *bytes, size_t length)
{
	size_t index;
	int ret;

	if (putchar('"') == EOF)
		return EIO;
	for (index = 0; index < length; ++index) {
		switch (bytes[index]) {
		case '"':
			ret = fputs("\\\"", stdout);
			break;
		case '\\':
			ret = fputs("\\\\", stdout);
			break;
		case '\b':
			ret = fputs("\\b", stdout);
			break;
		case '\f':
			ret = fputs("\\f", stdout);
			break;
		case '\n':
			ret = fputs("\\n", stdout);
			break;
		case '\r':
			ret = fputs("\\r", stdout);
			break;
		case '\t':
			ret = fputs("\\t", stdout);
			break;
		default:
			if (bytes[index] >= 0x20 && bytes[index] < 0x7f)
				ret = putchar(bytes[index]);
			else
				ret = printf("\\u%04x", bytes[index]);
			break;
		}
		if (ret < 0)
			return EIO;
	}
	return putchar('"') == EOF ? EIO : 0;
}

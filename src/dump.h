#ifndef _dump_h_
#define _dump_h_

#include <stdio.h>
#include <stdlib.h>

#define LINE_SIZE 0x10

static inline void dump_line(int line, const unsigned char *data, int size, void(*print)(void *, const char *), void *ud) {
	int i;
	char tmp[256];
	int idx = sprintf(tmp,"%08x: ", line*LINE_SIZE);
	for (i=0; i<LINE_SIZE; i++) {
		if (i%8 == 0)
			idx += sprintf(tmp+idx, " ");
		if (i > size)
			idx += sprintf(tmp+idx, "  ");
		else
			idx += sprintf(tmp+idx, "%02x", data[i]);
		if (i%2 != 0)
			idx += sprintf(tmp+idx, " ");
	}
	idx += sprintf(tmp+idx, "  ");
	for (i=0; i<LINE_SIZE; i++) {
		if (i > size) {
			idx += sprintf(tmp+idx, " ");
		} else {
			unsigned char c = data[i];
			if (c >= 32 && c <= 127)
				idx += sprintf(tmp+idx, "%c", c);
			else
				idx += sprintf(tmp+idx, ".");
		}
	}
	idx += sprintf(tmp+idx, "\n");
	if (line%LINE_SIZE == LINE_SIZE-1)
		idx += sprintf(tmp+idx, "\n");
	print(ud, tmp);
}

static inline void dump(const unsigned char *data, int size, void(*print)(void *, const char *), void *ud) {
	int i;
	int line = size/LINE_SIZE;
	for (i=0; i<line; ++i)
		dump_line(i, data+i*LINE_SIZE, LINE_SIZE, print, ud);
	if (size > line*LINE_SIZE)
		dump_line(line, data+line*LINE_SIZE, size%LINE_SIZE, print, ud);
}


#endif // _dump_h_

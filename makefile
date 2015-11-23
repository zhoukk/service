
BUILD ?= .

CFLAGS := -g -Wall
LIBS := -ldl -lrt -lpthread -lm -llua
LDFLAGS :=
SHARED := -fPIC --shared
EXPORT := -Wl,-E -Wl,-rpath,/usr/local/lib

SRC = epoll.c index.c hash.c env.c lalloc.c lserial.c lservice.c queue.c service.c socket.c timer.c main.c

all : $(BUILD)/service socket.so crypt.so netpack.so sproto.so lpeg.so

$(BUILD)/service : $(foreach v, $(SRC), src/$(v))
		$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(EXPORT) $(LIBS)

socket.so : luaclib/lsocket.c
		$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Isrc

crypt.so : luaclib/lcrypt.c luaclib/lsha1.c
		$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Isrc

netpack.so : luaclib/lnetpack.c
		$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Isrc

sproto.so : luaclib/sproto/lsproto.c luaclib/sproto/sproto.c
		$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Iluaclib/sproto

lpeg.so :
		cd 3rd/lpeg-0.12.2 && $(MAKE) CC=$(CC) && cp ./lpeg.so ../../		

clean:
	rm -f $(BUILD)/service socket.so

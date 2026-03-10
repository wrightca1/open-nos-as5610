# platform-mgrd — minimal platform daemon (AS5610-52X)
# Build: make [CROSS_COMPILE=powerpc-linux-gnu-]
# Install: DESTDIR=/path make install

CC = $(CROSS_COMPILE)gcc
CFLAGS = -std=gnu99 -Wall -O2 -static
TARGET = platform-mgrd

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

install: $(TARGET)
	install -d $(DESTDIR)/usr/sbin
	install -m 755 $(TARGET) $(DESTDIR)/usr/sbin/

clean:
	rm -f $(TARGET)

.PHONY: all install clean

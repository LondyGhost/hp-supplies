CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wno-deprecated-declarations
PKG      = gtk+-3.0 libusb-1.0
PREFIX  ?= /usr
APP_VERSION ?= 1.2.1

all: hp-supplies

hp-supplies: src/main.c
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(PKG)) \
		-DAPP_VERSION='"$(APP_VERSION)"' -o $@ $< \
		$(shell pkg-config --libs $(PKG)) -lpthread

install: hp-supplies
	install -Dm755 hp-supplies $(DESTDIR)$(PREFIX)/bin/hp-supplies
	install -Dm644 data/hp-supplies.desktop $(DESTDIR)$(PREFIX)/share/applications/hp-supplies.desktop
	install -Dm644 data/hp-supplies.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/hp-supplies.svg
	install -Dm644 data/60-hp-supplies.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/60-hp-supplies.rules

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/hp-supplies
	rm -f $(DESTDIR)$(PREFIX)/share/applications/hp-supplies.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/hp-supplies.svg
	rm -f $(DESTDIR)$(PREFIX)/lib/udev/rules.d/60-hp-supplies.rules

clean:
	rm -f hp-supplies

.PHONY: all install uninstall clean

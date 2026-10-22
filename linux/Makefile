# mnpaper - Linux (Wayland) build.
#
# Dependencies: libwayland-client and libsystemd (sd-bus, for the tray) at
# runtime, and wayland-scanner only when regenerating the vendored protocol
# code (already committed under protocols/, so a plain build needs just the
# two libraries and their headers).
#
#   make            build ./mnpaper
#   make test       build and run the headless tests (core + tray; the tray
#                   case starts its own dbus-daemon, no display needed)
#   make regen      re-run wayland-scanner after editing a protocol .xml

CC      ?= cc
CFLAGS  ?= -O2
WARN     = -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -lwayland-client -lpthread -lm -lsystemd

BIN = mnpaper
PROT = protocols/wlr-layer-shell-unstable-v1
XDG = protocols/xdg-shell

all: $(BIN)

$(BIN): mnpaper.c tray.c tray.h paper_core.h $(PROT).h $(PROT).c $(XDG).c ../version.h
	$(CC) $(CFLAGS) $(WARN) mnpaper.c tray.c $(PROT).c $(XDG).c -o $@ $(LDLIBS)

# regenerate the vendored protocol code only when the XML changes; the XML is
# an ORDER-ONLY prerequisite, so the committed generated files are treated as
# sources and a plain build never runs wayland-scanner.
$(PROT).h: | $(PROT).xml
	wayland-scanner client-header $(PROT).xml $@

$(PROT).c: | $(PROT).xml
	wayland-scanner private-code $(PROT).xml $@

$(XDG).c: | $(XDG).xml
	wayland-scanner private-code $(XDG).xml $@

# explicit regeneration for when the XML really changes
regen:
	wayland-scanner client-header $(PROT).xml $(PROT).h
	wayland-scanner private-code $(PROT).xml $(PROT).c
	wayland-scanner private-code $(XDG).xml $(XDG).c

test: $(BIN) tests/test_core tests/test_tray
	./tests/test_core
	./tests/test_tray

tests/test_core: tests/test_core.c paper_core.h
	$(CC) $(CFLAGS) $(WARN) tests/test_core.c -o $@ $(LDLIBS)

# the tray test drives the real tray code over a private session bus
tests/test_tray: tests/test_tray.c tray.c tray.h
	$(CC) $(CFLAGS) $(WARN) tests/test_tray.c tray.c -o $@ $(LDLIBS)

clean:
	rm -f $(BIN) tests/test_core tests/test_tray

.PHONY: all test clean regen

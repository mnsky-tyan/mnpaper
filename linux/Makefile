# mnpaper - Linux (Wayland) build.
#
# Dependencies: libwayland-client (runtime) and wayland-scanner only when
# regenerating the vendored protocol code (already committed under
# protocols/, so a plain build needs just libwayland).
#
#   make            build ./mnpaper
#   make test       build and run the headless core tests (no display needed)

CC      ?= cc
CFLAGS  ?= -O2
WARN     = -Wall -Wextra -Wno-unused-parameter
LDLIBS   = -lwayland-client -lpthread -lm

BIN = mnpaper
PROT = protocols/wlr-layer-shell-unstable-v1
XDG = protocols/xdg-shell

all: $(BIN)

$(BIN): mnpaper.c paper_core.h $(PROT).h $(PROT).c ../version.h
	$(CC) $(CFLAGS) $(WARN) mnpaper.c $(PROT).c $(XDG).c -o $@ $(LDLIBS)

# regenerate the vendored protocol code only when the XML changes
$(PROT).h: $(PROT).xml
	wayland-scanner client-header $< $@

$(PROT).c: $(PROT).xml
	wayland-scanner private-code $< $@

test: tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.c paper_core.h
	$(CC) $(CFLAGS) $(WARN) tests/test_core.c -o $@ $(LDLIBS)

clean:
	rm -f $(BIN) tests/test_core

.PHONY: all test clean

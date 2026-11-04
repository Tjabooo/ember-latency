# ember-latency: make && make install (per user, no root), make uninstall
CC      ?= cc
CFLAGS  ?= -O2
PREFIX  ?= $(HOME)/.local
LIBDIR  ?= $(PREFIX)/lib/ember-latency
LAYERDIR ?= $(PREFIX)/share/vulkan/implicit_layer.d
LIB      = libVkLayer_ember_latency.so
MANIFEST = ember_latency.x86_64.json

all: $(LIB)

$(LIB): src/ember-latency-layer.c
	$(CC) $(CFLAGS) -shared -fPIC -fvisibility=hidden -pthread -Wall -Wextra -o $@ $<

install: $(LIB)
	install -Dm755 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	mkdir -p $(DESTDIR)$(LAYERDIR)
	sed 's#@LIB@#$(LIBDIR)/$(LIB)#' ember_latency.json.in > $(DESTDIR)$(LAYERDIR)/$(MANIFEST)
	@id -nG | grep -qw input || echo "note: add yourself to the input group so the layer can see key presses: sudo usermod -aG input $$USER (then log in again)"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB) $(DESTDIR)$(LAYERDIR)/$(MANIFEST)

clean:
	rm -f $(LIB)

.PHONY: all install uninstall clean

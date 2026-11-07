CXX ?= g++
PKG_CONFIG ?= pkg-config
OUTPUT ?= hyprland-fix-hdr.so
PACKAGES = pixman-1 libdrm hyprland aquamarine pangocairo libinput libudev wayland-server xkbcommon
PLUGIN_SOURCES = main.cpp src/screenshare.cpp

.PHONY: all clean FORCE
all: $(OUTPUT)

# Hyprland and dependency headers can change without touching plugin sources.
# Always compile with the headers selected by pkg-config, without release pins.
$(OUTPUT): $(PLUGIN_SOURCES) modules.hpp src/capture-shader.hpp Makefile FORCE
	$(PKG_CONFIG) --print-errors --exists $(PACKAGES)
	$(CXX) -shared -fPIC -fno-gnu-unique -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-narrowing -std=c++2b $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) $(PLUGIN_SOURCES) -o $(OUTPUT).new
	mv -f -- $(OUTPUT).new $(OUTPUT)

FORCE:

clean:
	rm -f -- $(OUTPUT) $(OUTPUT).new

# Optional local developer checks are not distributed with the plugin.
-include local/Makefile.include

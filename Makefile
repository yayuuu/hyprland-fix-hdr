CXX ?= g++
PKG_CONFIG ?= pkg-config
STRIP ?= strip
OUTPUT ?= hyprland-fix-hdr.so
PACKAGES = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon
AQUAMARINE_VERSION = $(shell $(PKG_CONFIG) --modversion aquamarine)
PLUGIN_SOURCES = main.cpp src/screenshare.cpp

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): $(PLUGIN_SOURCES) modules.hpp
	$(PKG_CONFIG) --exists $(PACKAGES)
	$(CXX) -shared -fPIC -fno-gnu-unique -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-narrowing -std=c++2b -Ibuild $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) -DFIX_HDR_AQUAMARINE_VERSION='"$(AQUAMARINE_VERSION)"' $(PLUGIN_SOURCES) -o $(OUTPUT).new
	mv -f -- $(OUTPUT).new $(OUTPUT)

build:
	mkdir -p build

clean:
	rm -f -- $(OUTPUT) $(OUTPUT).new

# Optional local developer checks are not distributed with the plugin.
-include local/Makefile.include

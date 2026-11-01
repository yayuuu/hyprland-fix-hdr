CXX ?= g++
PKG_CONFIG ?= pkg-config
OUTPUT ?= hyprland-fix-hdr.so
PACKAGES = pixman-1 libdrm hyprland aquamarine pangocairo libinput libudev wayland-server xkbcommon
PLUGIN_SOURCES = main.cpp src/screenshare.cpp

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): $(PLUGIN_SOURCES) modules.hpp src/capture-shader.hpp Makefile
	$(PKG_CONFIG) --exists $(PACKAGES)
	$(CXX) -shared -fPIC -fno-gnu-unique -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-narrowing -std=c++2b $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) $(PLUGIN_SOURCES) -o $(OUTPUT).new
	mv -f -- $(OUTPUT).new $(OUTPUT)

clean:
	rm -f -- $(OUTPUT) $(OUTPUT).new

# Optional local developer checks are not distributed with the plugin.
-include local/Makefile.include

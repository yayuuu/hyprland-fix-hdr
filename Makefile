CXX ?= g++
PKG_CONFIG ?= pkg-config
STRIP ?= strip
OUTPUT ?= hyprland-fix-hdr.so
PACKAGES = pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon
AQUAMARINE_VERSION = $(shell $(PKG_CONFIG) --modversion aquamarine)
EMBEDDED_DEPS = build/hdr-offload-worker
EMBEDDED_SOURCES = offload/embedded.cpp offload/worker-blob.S
PLUGIN_SOURCES = main.cpp src/screenshare.cpp src/offload.cpp $(EMBEDDED_SOURCES)

.PHONY: all clean
all: $(OUTPUT)

$(OUTPUT): $(PLUGIN_SOURCES) modules.hpp policy.hpp pq.hpp capture_wait.hpp $(wildcard offload/*.hpp) $(EMBEDDED_DEPS)
	$(PKG_CONFIG) --exists $(PACKAGES)
	$(CXX) -shared -fPIC -fno-gnu-unique -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-narrowing -std=c++2b -Ibuild $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) -DFIX_HDR_AQUAMARINE_VERSION='"$(AQUAMARINE_VERSION)"' -DHDR_SINGLEPASS_AQ_VERSION='"$(AQUAMARINE_VERSION)"' $(PLUGIN_SOURCES) -o $(OUTPUT).new
	mv -f -- $(OUTPUT).new $(OUTPUT)

build:
	mkdir -p build

build/pq.comp.spv: offload/pq.comp | build
	glslangValidator -V $< -o $@

build/hdr-offload-worker: offload/worker.cpp offload/protocol.hpp offload/supervisor.hpp offload/shader-blob.S build/pq.comp.spv
	$(CXX) -O2 -g -std=c++23 -Wall -Wextra -Werror -Wno-missing-field-initializers -Ibuild offload/worker.cpp offload/shader-blob.S -lvulkan -o $@.new
	$(STRIP) --strip-debug $@.new
	mv -f -- $@.new $@

clean:
	rm -f -- $(OUTPUT) $(OUTPUT).new build/hdr-offload-worker build/hdr-offload-worker.new build/pq.comp.spv

# Optional local developer checks are not distributed with the plugin.
-include local/Makefile.include

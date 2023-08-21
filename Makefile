# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

SVGO="$(shell yarn bin)/svgo"

# FLAGS will be passed to both the C and C++ compiler
FLAGS += -I. -O0 -g
BABYCAT_TARGET_DIR ?= dep/babycat/target/release

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=  -L$(BABYCAT_TARGET_DIR)/c/ -lbabycat $(shell cat $(BABYCAT_TARGET_DIR)/libbabycat.a.flags) -lsndfile -lfmt

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

.PHONY: formatSvgs final 

final: formatSvgs
	+$(MAKE) install


formatSvgs:
	@echo "Formatting resource SVGs"
	$(SVGO) res/*.svg -o res/*.svg --config=svgo.config.js

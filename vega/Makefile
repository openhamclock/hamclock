# Makefile for HamClock Vega OS wrapper

.PHONY: all build build-release clean clean-all

all: build

build:
	npm run build:debug

build-release:
	npm run build:release

clean:
	rm -rf buildinfo.json dist build CMakeUserPresets.json

clean-all: clean
	rm -rf node_modules

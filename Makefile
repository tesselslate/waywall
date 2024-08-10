.PHONY: all check clean configure_debug configure_release format lint

LUA=waywall/lua/api.lua \
	waywall/lua/helpers.lua \
	waywall/lua/init.lua

all: build
	ninja -C build

check: build
	ninja -C build test

clean: build
	ninja -C build clean

configure_debug: build
	meson configure build -Dbuildtype=debug -Db_sanitize=address,undefined

configure_release: build
	meson configure build -Dbuildtype=release -Db_sanitize=none

format: build
	ninja -C build clang-format
	stylua $(LUA)

lint: build
	ninja -C build scan-build
	selene $(LUA)

# non-PHONY

build:
	meson setup build

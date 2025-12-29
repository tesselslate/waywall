.PHONY: all check clean configure_debug configure_release format lint

LUA=waywall/lua/api.lua \
	waywall/lua/helpers.lua \
	waywall/lua/init.lua

all: build/build.ninja
	ninja -C build

check: build/build.ninja
	ninja -C build test

clean: build/build.ninja
	ninja -C build clean

configure_debug: build/build.ninja
	meson configure build -Dbuildtype=debug -Db_sanitize=address,undefined

configure_release: build/build.ninja
	meson configure build -Dbuildtype=release -Db_sanitize=none

format: build/build.ninja
	ninja -C build clang-format
	stylua $(LUA)

lint: build/build.ninja
	ninja -C build scan-build
	selene $(LUA)

# non-PHONY

build/build.ninja:
	meson setup build

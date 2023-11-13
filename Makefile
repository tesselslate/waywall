.PHONY: build clean run setup

build:
	ninja -C build

clean:
	ninja -C build clean

run: build
	build/waywall/waywall

setup:
	meson setup build

# configuration

conf_debug:
	meson configure -Db_sanitize=address,undefined build
	meson configure -Dbuildtype=debug build

conf_release:
	meson configure -Db_sanitize=none build
	meson configure -Dbuildtype=release build

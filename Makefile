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

conf_san:
	meson configure -Db_sanitize=address,undefined build

conf_nosan:
	meson configure -Db_sanitize=none build

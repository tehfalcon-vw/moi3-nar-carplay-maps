ZIG ?= zig
TARGET := build/altscreen_handler.so
SOURCE := src/altscreen_handler.c

.PHONY: all clean verify package

all: $(TARGET)

$(TARGET): $(SOURCE)
	mkdir -p build
	$(ZIG) cc -target aarch64-linux-gnu.2.17 -shared -fPIC -O2 -Wall -Wextra \
		-fno-stack-protector -Wl,-z,relro,-z,now -o $@ $< -ldl -lpthread

verify: $(TARGET)
	python tools/verify_build.py $(TARGET)

package: verify
	python tools/make_release.py

clean:
	rm -rf build dist

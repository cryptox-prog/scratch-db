.PHONY: all build test run clean

all: build

build:
	cmake -S . -B build && cmake --build build

test: build
	ctest --test-dir build --output-on-failure

run: build
	./build/db_cli

clean:
	rm -rf build

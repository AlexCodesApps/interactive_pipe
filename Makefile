.PHONY: run, build, clean

run: c.so
	lua init.lua

build: c.so

c.so: c.c
	cc c.c -shared -fPIC -o c.so

clean:
	rm c.so

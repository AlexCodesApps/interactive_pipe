.PHONY: run

run: c.so
	lua init.lua

c.so: c.c
	cc c.c -shared -fPIC -o c.so

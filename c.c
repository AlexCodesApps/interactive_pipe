#include <assert.h>
#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define CLOSE_PIPE(p) if (close(p[0]) || close(p[1])) { \
	return luaL_error(l, "couldn't close pipe"); \
}

int exec(lua_State * l) {
	const char * cmd = luaL_checkstring(l, 1);
	const char * content = luaL_checkstring(l, 2);
	void * ud;
	lua_Alloc alloc = lua_getallocf(l, &ud);
	if (!alloc) {
		return luaL_error(l, "couldn't get allocator\n");
	}
	int stdin_pipe[2];
	int stdout_pipe[2];
	if (pipe(stdin_pipe)) {
		goto error;
	}
	if (pipe(stdout_pipe)) {
		CLOSE_PIPE(stdin_pipe);
		goto error;
	}
	int pid = fork();
	if (pid == -1) {
		CLOSE_PIPE(stdin_pipe);
		CLOSE_PIPE(stdout_pipe);
		goto error;
	}
	if (pid == 0) {
		if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) exit(1);
		if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) exit(1);
		close(stdin_pipe[0]);
		close(stdin_pipe[1]);
		close(stdout_pipe[0]);
		close(stdout_pipe[1]);
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		exit(1);
	}
	close(stdin_pipe[0]);
	close(stdout_pipe[1]);
	int clen = strlen(content);
	assert(write(stdin_pipe[1], content, strlen(content)) == clen);
	close(stdin_pipe[1]);
	char buf[1024];
	char * str = alloc(ud, NULL, 0, 1);
	assert(str);
	size_t len = 0;
	ssize_t nread;
	while ((nread = read(stdout_pipe[0], buf, 1024)) > 0) {
		str = alloc(ud, str, len + 1, len + nread + 1);
		assert(str);
		memcpy(str + len, buf, nread);
		len += nread;
	}
	if (nread < 0) {
		close(stdout_pipe[0]);
		perror("read");
		goto error;
	}
	str[len] = '\0';
	close(stdout_pipe[1]);
	int wstatus = -1;
	int waitr = waitpid(pid, &wstatus, 0);
	if (waitr == -1 || waitr == 0) {
		close(stdout_pipe[0]);
		goto error;
	}
	if (!WIFEXITED(wstatus)) {
		close(stdout_pipe[0]);
		goto error;
	}
	int status = WEXITSTATUS(wstatus);
	lua_pushstring(l, str);
	lua_pushinteger(l, status);
	return 2;
error:
	lua_pushnil(l);
	return 1;
}

static const luaL_Reg c_table[] = {
	{"exec", exec},
	{NULL, NULL}
};

int luaopen_c(lua_State * l) {
	luaL_newlib(l, c_table);
	return 1;
}

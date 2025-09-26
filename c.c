#include <assert.h>
#include <errno.h>
#include <lua.h>
#include <lauxlib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

enum StateType {
	STATE_WRITING,
	STATE_READING,
	STATE_FINISHED,
};

enum Result {
	RESULT_OK,
	RESULT_INTERRUPTED,
	RESULT_ERRNO_ERR,
	RESULT_OOM,
	RESULT_ILLEGAL_STATE,
};

struct {
	enum StateType type;
	int pid;
	unsigned read_done : 1;
	unsigned write_done : 1;
	struct {
		const char * left;
		size_t left_len;
		int fd;
	} writing;
	struct {
		lua_Alloc allocator;
		void * alloc_ud;
		char * str;
		size_t str_len;
		int fd;
	} reading;
	jmp_buf on_cancel;
} state;

union Pipe {
	int fds[2];
	struct {
		int out;
		int in;
	};
};

void * safe_signal(int sig, void * handler) {
	struct sigaction sa, old;
	sigemptyset(&sa.sa_mask);
	sigaddset(&sa.sa_mask, sig);
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	assert(sigaction(sig, &sa, &old) == 0);
	return old.sa_handler;
}

void close_pipe(union Pipe pipe) {
	close(pipe.in);
	close(pipe.out);
}

enum Result init_state(const char * cmd, const char * contents, lua_Alloc allocator, void * alloc_ud, jmp_buf on_cancel) {
	union Pipe in, out;
	if (pipe(in.fds)) {
		return RESULT_ERRNO_ERR;
	}
	if (pipe(out.fds)) {
		int err = errno;
		close_pipe(in);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	int pid = fork();
	if (pid == -1) {
		int err = errno;
		close_pipe(in);
		close_pipe(out);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	if (pid == 0) {
		const char * shell = getenv("SHELL");
		if (!shell) {
			shell = "/bin/sh";
		}
		if (dup2(in.out, STDIN_FILENO) < 0) exit(1);
		if (dup2(out.in, STDOUT_FILENO) < 0) exit(1);
		close_pipe(in);
		close_pipe(out);
		execl(shell, shell, "-c", cmd, NULL);
		exit(1);
	}
	close(out.in);
	close(in.out);
	int flags = fcntl(out.out, F_GETFL);
	if (flags == -1) {
		int err = errno;
		kill(pid, SIGTERM);
		close(out.out);
		close(in.in);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	flags |= O_NONBLOCK;
	if (fcntl(out.out, F_SETFL, flags) == -1) {
		int err = errno;
		kill(pid, SIGTERM);
		close(out.out);
		close(in.in);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	flags = fcntl(in.in, F_GETFL);
	if (flags == -1) {
		int err = errno;
		kill(pid, SIGTERM);
		close(out.out);
		close(in.in);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	flags |= O_NONBLOCK;
	if (fcntl(in.in, F_SETFL, flags) == -1) {
		int err = errno;
		kill(pid, SIGTERM);
		close(out.out);
		close(in.in);
		errno = err;
		return RESULT_ERRNO_ERR;
	}
	state.type = STATE_WRITING;
	state.pid = pid;
	state.read_done = 0;
	state.reading.allocator = allocator;
	state.reading.alloc_ud = alloc_ud;
	state.reading.fd = out.out;
	state.reading.str = allocator(alloc_ud, NULL, 0, 1);
	if (!state.reading.str) {
		int err = errno;
		kill(pid, SIGTERM);
		close(out.out);
		close(in.in);
		errno = err;
		return RESULT_OOM;
	}
	state.reading.str_len = 0;
	state.write_done = 0;
	state.writing.fd = in.in;
	state.writing.left = contents;
	state.writing.left_len = strlen(contents);
	memcpy(state.on_cancel, on_cancel, sizeof(*on_cancel));
	return RESULT_OK;
}

enum Result finish_state(int * status, int * exit_type) {
	close(state.reading.fd);
	close(state.writing.fd);
	int wstatus;
	int wresult = waitpid(state.pid, &wstatus, 0);
	if (wresult == -1) {
		return RESULT_ERRNO_ERR;
	}
	*exit_type = 0;
	if (WIFEXITED(wstatus)) {
		*status = WEXITSTATUS(wstatus);
	} else if (WIFSIGNALED(wstatus)) {
		*status = 140;
	} else {
		return RESULT_ERRNO_ERR;
	}
	state.reading.str[state.reading.str_len] = '\0';
	return RESULT_OK;
}

enum Result write_state(void) {
	int fd = state.writing.fd;
	const char * left = state.writing.left;
	size_t len = state.writing.left_len;
	ssize_t result = write(fd, left, len);
	if (result == -1) {
		if (errno == EINTR) {
			return RESULT_INTERRUPTED;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			return RESULT_ERRNO_ERR;
		}
	} else if (result == len) {
		close(state.writing.fd);
		state.write_done = 1;
	} else {
		state.writing.left += result;
		state.writing.left_len -= result;
	}
	if (!state.read_done) {
		state.type = STATE_READING;
	} else if (state.write_done) {
		state.type = STATE_FINISHED;
	} else {
		return RESULT_ILLEGAL_STATE;
	}
	return RESULT_OK;
}

enum Result read_state(void) {
	char buf[1024];
	int fd = state.reading.fd;
	char ** str = &state.reading.str;
	size_t * str_len = &state.reading.str_len;
	for (;;) {
		ssize_t result = read(fd, buf, 1024);
		if (result == -1) {
			if (errno == EINTR) {
				return RESULT_INTERRUPTED;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				return RESULT_ERRNO_ERR;
			}
			break;
		} else if (result == 0) {
			close(state.reading.fd);
			state.read_done = 1;
			break;
		} else {
			char * new_str =
				state.reading.allocator(state.reading.alloc_ud, *str, *str_len + 1, *str_len + result + 1);
			if (!new_str) {
				return RESULT_OOM;
			}
			memcpy(new_str + *str_len, buf, result);
			*str_len += result;
			*str = new_str;
		}
	}
	if (!state.write_done) {
		state.type = STATE_WRITING;
	} else if (state.read_done) {
		state.type = STATE_FINISHED;
	} // implicitly try again on else branch
	return RESULT_OK;
}

enum Result run_state(const char * cmd, const char * contents, lua_Alloc allocator, void * alloc_ud, int * status, int * exit_type, char ** out_str) {
	jmp_buf on_cancel;
	if (setjmp(on_cancel)) {
		return RESULT_INTERRUPTED;
	}
	enum Result res = init_state(cmd, contents, allocator, alloc_ud, on_cancel);
	if (res != RESULT_OK) {
		return res;
	}
	for (;;) {
		switch (state.type) {
		case STATE_WRITING:
			res = write_state();
			break;
		case STATE_READING:
			res = read_state();
			break;
		case STATE_FINISHED:
			goto outer;
		}
		if (res != RESULT_OK) {
			finish_state(status, exit_type);
			return res;
		}
	}
outer:
	res = finish_state(status, exit_type);
	if (res != RESULT_OK) {
		return res;
	}
	*out_str = state.reading.str;
	return RESULT_OK;
}

int exec(lua_State * l) {
	void (*pipe_handler)(int) = safe_signal(SIGPIPE, SIG_IGN);
	const char * cmd = luaL_checkstring(l, 1);
	const char * content = luaL_checkstring(l, 2);
	void * ud;
	lua_Alloc alloc = lua_getallocf(l, &ud);
	if (!alloc) {
		return luaL_error(l, "couldn't get allocator\n");
	}
	int status;
	int exit_type;
	char * str;
	enum Result res = run_state(cmd, content, alloc, ud, &status, &exit_type, &str);
	safe_signal(SIGPIPE, pipe_handler);
	if (res == RESULT_INTERRUPTED) {
		lua_pushnil(l);
		lua_pushinteger(l, 140);
		lua_pushinteger(l, 1);
		return 3;
	}
	assert(res == RESULT_OK);
	lua_pushstring(l, str);
	lua_pushinteger(l, status);
	lua_pushinteger(l, exit_type);
	return 3;
error:
	safe_signal(SIGPIPE, pipe_handler);
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

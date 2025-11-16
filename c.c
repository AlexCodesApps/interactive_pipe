#include <assert.h>
#include <errno.h>
#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <fcntl.h>

#define DEBUG(...)
// #define DEBUG(...) fprintf(stderr, __VA_ARGS__)

typedef enum {
	RESULT_OK = 0,
	RESULT_INTR,
	RESULT_OOM,
	RESULT_ERR,
} Result;

Result unblock_fd(int fd) {
	int flags = fcntl(fd, F_GETFL);
	if (flags == -1)
		return RESULT_ERR;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags))
		return RESULT_ERR;
	return RESULT_OK;
}

typedef struct {
	int pid;
	int stdin_fd;
	int stdout_fd;
	const char * to_write;
	size_t n_to_write;
	char * read;
	size_t n_read;
	size_t read_cap;
	lua_Alloc alloc;
	void * ud;
} State;

static void _exec_cleanup_err(State * state) {
	int wstatus;
	close(state->stdin_fd);
	close(state->stdout_fd);
	waitpid(state->pid, &wstatus, 0);
	state->alloc(state->ud, state->read, state->read_cap, 0);
}

static Result _exec_cleanup(State * state, int * status) {
	close(state->stdin_fd);
	close(state->stdout_fd);
	if (waitpid(state->pid, status, 0) < 0) {
		return RESULT_ERR;
	}
	return RESULT_OK;
}

static Result _exec_run(State * state) {
	char buf[1024];
	struct pollfd pfds[2];
	ssize_t res;
	pfds[0].fd = state->stdin_fd;
	pfds[0].events = POLLOUT;
	pfds[1].fd = state->stdout_fd;
	pfds[1].events = POLLIN;
	int alive = 2;
	while (alive > 0) {
		DEBUG("polling\n");
		int pollres = poll(pfds, 2, -1);
		if (pollres < 0) {
			return RESULT_ERR;
		}
		DEBUG("polled [%d]\n", pollres);
		DEBUG("revents[0] = %d, revents[1] = %d\n", pfds[0].revents, pfds[1].revents);
		if (pfds[0].revents & POLLOUT) {
			DEBUG("write available\n");
			res = write(pfds[0].fd, state->to_write, state->n_to_write);
			DEBUG("%zu bytes written\n", res);
			if (res < 0) {
				if (errno != EWOULDBLOCK && errno != EAGAIN) {
					return RESULT_ERR;
				}
			}
			state->to_write += res;
			state->n_to_write -= res;
			if (state->n_to_write == 0) {
				close(pfds[0].fd);
				pfds[0].fd = -1;
				state->stdin_fd = -1;
				alive -= 1;
			}
		}
		if (pfds[1].revents & POLLIN) {
			DEBUG("read available\n");
			while ((res = read(pfds[1].fd, buf, sizeof buf)) > 0) {
				DEBUG("%zu bytes read\n", res);
				size_t new_sz = state->n_read + res + 1; // 1 for null terminator
				if (new_sz > state->read_cap) {
					size_t new_cap = new_sz * 2;
					void * new_alloc = state->alloc(state->ud, state->read, state->read_cap, new_cap);
					if (!new_alloc) {
						return RESULT_OOM;
					}
					state->read = new_alloc;
					state->read_cap = new_cap;
				}
				memcpy(state->read + state->n_read, buf, res);
				state->n_read += res;
				state->read[state->n_read] = '\0';
			}
			if (res == -1) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					return RESULT_ERR;
				}
			} else if (res == 0) {
				DEBUG("eof\n");
				close(pfds[1].fd);
				pfds[1].fd = -1;
				state->stdout_fd = -1;
				alive -= 1;
			}
		}
		if (pfds[1].revents & POLLHUP) {
			close(pfds[1].fd);
			pfds[1].fd = -1;
			state->stdout_fd = -1;
			alive -= 1;
		}
	}
	return RESULT_OK;
}

Result _exec_init(State * state, const char * cmd, const char * in,
		lua_Alloc alloc, void * ud) {
	int infd[2], outfd[2];
	if (pipe(infd)) {
		return RESULT_ERR;
	}
	#define STDIN_READ_END infd[0]
	#define STDIN_WRITE_END infd[1]
	#define STDOUT_READ_END outfd[0]
	#define STDOUT_WRITE_END outfd[1]
	Result ret = RESULT_ERR;
	if (pipe(outfd)) {
		goto err_close_infd;
	}
	int pid = fork();
	if (pid < 0) {
		goto err_close_outfd;
	}
	if (pid == 0) {
	const char * shell = getenv("SHELL");
		if (!shell)
			shell = "/bin/sh";
		dup2(STDOUT_WRITE_END, STDOUT_FILENO);
		dup2(STDIN_READ_END, STDIN_FILENO);
		close(STDIN_READ_END);
		close(STDIN_WRITE_END);
		close(STDOUT_READ_END);
		close(STDOUT_WRITE_END);
		execl(shell, shell, "-c", cmd, NULL);
		exit(EXIT_SUCCESS);
	}
	close(STDIN_READ_END);
	close(STDOUT_WRITE_END);
	if (unblock_fd(STDIN_WRITE_END) || unblock_fd(STDOUT_READ_END)) {
		kill(pid, SIGINT);
		close(STDIN_WRITE_END);
		close(STDOUT_READ_END);
		return RESULT_ERR;
	}
#define INITIAL_CAPACITY 8
	state->pid = pid;
	state->stdin_fd = STDIN_WRITE_END;
	state->stdout_fd = STDOUT_READ_END;
	state->to_write = in;
	state->n_to_write = strlen(in);
	state->n_read = 0;
	state->alloc = alloc;
	state->ud = ud;
	state->read = alloc(ud, NULL, 0, INITIAL_CAPACITY);
	if (!state->read) {
		kill(pid, SIGINT);
		close(outfd[0]);
		close(infd[1]);
		return RESULT_OOM;
	}
	state->read[0] = '\0';
	state->read_cap = INITIAL_CAPACITY;
	return RESULT_OK;
err_close_outfd:
	close(outfd[0]);
	close(outfd[1]);
err_close_infd:
	close(infd[0]);
	close(infd[1]);
	return ret;
}
#undef STDIN_READ_END
#undef STDIN_WRITE_END
#undef STDOUT_READ_END
#undef STDOUT_WRITE_END


int exec(lua_State * l) {
	const char * cmd = luaL_checkstring(l, 1);
	const char * content = luaL_checkstring(l, 2);
	void * ud;
	lua_Alloc alloc = lua_getallocf(l, &ud);
	if (!alloc) {
		return luaL_error(l, "couldn't get allocator\n");
	}
	State state;
	Result res = _exec_init(&state, cmd, content, alloc, ud);
	if (res) {
		if (res == RESULT_ERR)
			perror("exec_init");
		else if (res == RESULT_OOM)
			puts("OOM memory error");
		exit(255);
	}
	res = _exec_run(&state);
	if (res) {
		if (res == RESULT_ERR)
			perror("exec_run");
		else if (res == RESULT_OOM)
			puts("OOM memory error");
		_exec_cleanup_err(&state);
		exit(255);
	}
	int status;
	if (_exec_cleanup(&state, &status)) {
		perror("exec_cleanup");
		exit(255);
	}
	assert(WIFEXITED(status));
	lua_pushstring(l, state.read);
	lua_pushinteger(l, WEXITSTATUS(status));
	lua_pushinteger(l, 0);
	return 3;
}

static const luaL_Reg c_table[] = {
	{"exec", exec},
	{NULL, NULL},
};

int luaopen_c(lua_State * l) {
	luaL_newlib(l, c_table);
	return 1;
}

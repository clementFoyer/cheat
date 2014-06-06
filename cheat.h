/*
Copyright (c) 2012, Guillermo "Tordek" Freschi
Copyright (c) 2013, Sampsa "Tuplanolla" Kiiskinen
All rights reserved.

The full license can be found in the LICENSE file that
 resides in the same directory as this file.
*/
#ifndef CHEAT_H
#define CHEAT_H

#ifndef __BASE_FILE__
#error "The __BASE_FILE__ macro isn't defined. Check the README for help."
#endif

#include <stddef.h> /* size_t */
#include <stdio.h> /* FILE, stdout, fputs(), fprintf(), fputc(), snprintf(), perror(), stderr */
#include <string.h> /* memset(), memcpy() */
#include <stdlib.h> /* free(), exit(), malloc(), realloc(), EXIT_FAILURE */

enum cheat_test_status {
	CHEAT_SUCCESS,
	CHEAT_FAILURE,
	CHEAT_IGNORE,
	CHEAT_SEGFAULT
};

struct cheat_test_suite {
	size_t test_count;
	size_t test_failures;
	enum cheat_test_status last_test_status;
	char** log;
	char* argv0;
	size_t log_size;
	int fork;
	FILE* captured_stdout;
};

typedef void cheat_test(struct cheat_test_suite* suite);

struct cheat_test_s {
	char const* name;
	cheat_test* test;
};

/* public interface: helpers */

#define TEST_IGNORE(test_name, test_body) TEST(test_name, {\
	suite->last_test_status = CHEAT_IGNORE;\
})

/* first pass: function declarations */

#define TEST(name, body) static void test_##name(struct cheat_test_suite* suite);
#define SET_UP(body) static void cheat_set_up(void);
#define TEAR_DOWN(body) static void cheat_tear_down(void);
#define GLOBALS(body)

#include __BASE_FILE__

#undef TEST
#undef SET_UP
#undef TEAR_DOWN
#undef GLOBALS

static void cheat_suite_init(struct cheat_test_suite* suite, char* argv0) {
	memset(suite, 0, sizeof *suite);
	suite->captured_stdout = stdout;
	suite->argv0 = argv0;
}

static void cheat_suite_summary(struct cheat_test_suite* suite) {
	if (suite->log) {
		size_t i;

		fputs("\n", suite->captured_stdout);
		for (i = 0; i < suite->log_size; ++i) {
			fputs(suite->log[i], suite->captured_stdout);
			free(suite->log[i]);
		}

		free(suite->log);
	}

	fprintf(suite->captured_stdout, "\n%zu failed tests of %zu tests run.\n", suite->test_failures, suite->test_count);
}

static void cheat_test_end(struct cheat_test_suite* suite) {
	suite->test_count++;

	switch (suite->last_test_status) {
		case CHEAT_SUCCESS:
			fputc('.', suite->captured_stdout);
			break;
		case CHEAT_FAILURE:
			fputc('F', suite->captured_stdout);
			suite->test_failures++;
			break;
		case CHEAT_IGNORE:
			fputc('I', suite->captured_stdout);
			break;
		case CHEAT_SEGFAULT:
			fputc('S', suite->captured_stdout);
			suite->test_failures++;
			break;
		default:
			exit(-1);
	}
}

static void cheat_log_append(struct cheat_test_suite* suite, char* message, size_t len) {
	char* buf;

	if (len == 0) {
		return;
	}

	buf = malloc(len + 1);
	memcpy(buf, message, len);

	buf[len] = '\0';

	suite->log_size++;
	suite->log = realloc(suite->log, (suite->log_size + 1) * sizeof (char*));
	suite->log[suite->log_size - 1] = buf; /* We give up our buffer! */
}

static void cheat_test_assert(
		struct cheat_test_suite* suite,
		int result,
		char const* assertion,
		char const* filename,
		int line) {
	if (result != 0)
		return;

	suite->last_test_status = CHEAT_FAILURE;
	if (suite->fork) {
		fprintf(suite->captured_stdout,
				"%s:%d: Assertion failed: '%s'.\n",
				filename,
				line,
				assertion);
	} else {
		char* buffer = NULL;
		size_t len = 255;
		size_t bufsize;

		do {
			bufsize = (len + 1);
			buffer = realloc(buffer, bufsize);
			const int what = snprintf(buffer, bufsize,
					"%s:%d: Assertion failed: '%s'.\n",
					filename,
					line,
					assertion);
			if (what == -1)
				exit(42);
			len = (size_t )what;
		} while (bufsize != (len + 1));

		cheat_log_append(suite, buffer, bufsize);
	}
}

static int run_test(struct cheat_test_s const* test, struct cheat_test_suite* suite) {
	suite->last_test_status = CHEAT_SUCCESS;

	cheat_set_up();
	(test->test)(suite);
	cheat_tear_down();

	return suite->last_test_status;
}

#if _POSIX_C_SOURCE >= 200112L

#include <unistd.h> /* pipe(), fork(), close(), STDOUT_FILENO, dup2(), execl(), read() */
#include <sys/types.h> /* pid_t, ssize_t */
#include <sys/wait.h> /* waitpid(), WIFEXITED(), WEXITSTATUS() */

#elif defined _WIN32

#include <windows.h> /* ... */

#endif

static void run_isolated_test(
		struct cheat_test_s const* test,
		struct cheat_test_suite* suite) {
#if _POSIX_C_SOURCE >= 200112L
	pid_t pid;
	int pipefd[2];

	pipe(pipefd);
	pid = fork();

	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		execl(suite->argv0, suite->argv0, test->name, NULL);
		exit(EXIT_FAILURE);
	} else {
		int status;
		char buf[255];
		ssize_t len;

		close(pipefd[1]);
		while ((len = read(pipefd[0], buf, 255)) != 0) {
			if (len == -1)
				exit(42);
			buf[len] = 0;
			cheat_log_append(suite, buf, (size_t )len + 1);
		}

		waitpid(pid, &status, 0);
		close(pipefd[0]);
		suite->last_test_status = WIFEXITED(status) ? WEXITSTATUS(status)
		                                            : CHEAT_SEGFAULT;
	}

#elif defined _WIN32

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof (SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = NULL;

	HANDLE stdoutPipe_read;
	HANDLE stdoutPipe_write;
	CreatePipe(&stdoutPipe_read, &stdoutPipe_write, &sa, 0);

	STARTUPINFO si = {
		.cb = sizeof (STARTUPINFO),
		.dwFlags = STARTF_USESTDHANDLES,
		.hStdOutput = stdoutPipe_write
	};

	PROCESS_INFORMATION pi = {0};

	CHAR command[255];
	snprintf(command, 255, "%s %s", suite->argv0, test->name);

	CreateProcess(
		NULL,
		command,
		NULL,
		NULL,
		TRUE,
		0,
		NULL,
		NULL,
		&si,
		&pi);

	CloseHandle(stdoutPipe_write);

	DWORD len;
	DWORD maxlen = 255;
	CHAR buffer[255];

	do {
		ReadFile(stdoutPipe_read, buffer, maxlen, &len, NULL);
		buffer[len] = '\0';
		cheat_log_append(suite, buffer, len);
	} while (len > 0);

	WaitForSingleObject(pi.hProcess, INFINITE);

	DWORD status;
	GetExitCodeProcess(pi.hProcess, &status);

	suite->last_test_status = (status & 0x80000000) ? CHEAT_SEGFAULT
													: status;

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

#else
#warning "Running isolated tests isn't supported in this environment. You'll have to use --no-fork." /* TODO move */
	fputs("Running isolated tests isn't supported in this environment. Please use --no-fork.\n", stderr);
	exit(EXIT_FAILURE);
#endif
}

/* second pass: listing functions */

#define TEST(test_name, body) { #test_name, test_##test_name },
#define SET_UP(body)
#define TEAR_DOWN(body)
#define GLOBALS(body)

static struct cheat_test_s const cheat_tests[] = {
#include __BASE_FILE__
};

static const size_t cheat_test_count = sizeof cheat_tests / sizeof *cheat_tests;

#undef TEST
#undef SET_UP
#undef TEAR_DOWN
#undef GLOBALS

int main(int argc, char* argv[]) {
	struct cheat_test_suite suite;
	size_t i;

	cheat_suite_init(&suite, argv[0]);

	suite.fork = 1;

	if (argc > 1) {
		if (argv[1][0] == '-') {
			if (strcmp(argv[1], "-n") == 0
					|| strcmp(argv[1], "--no-fork") == 0) {
				suite.fork = 0;
			}
		} else {
			for (i = 0; i < cheat_test_count; ++i) {
				struct cheat_test_s const current_test = cheat_tests[i];

				if (strcmp(argv[1], current_test.name) == 0) {
					return run_test(&current_test, &suite);
				}
			}

			return -1;
		}
	}

	for (i = 0; i < cheat_test_count; ++i) {
		struct cheat_test_s const current_test = cheat_tests[i];

		if (suite.fork) {
			run_isolated_test(&current_test, &suite);
		} else {
			run_test(&current_test, &suite);
		}

		cheat_test_end(&suite);
	}

	cheat_suite_summary(&suite);

	return (int )suite.test_failures; /* TODO truncate */
}

/* third pass: function definitions */

/* This is a part of the public interface. The other part is for the helpers. */

#define TEST(test_name, test_body) static void test_##test_name(struct cheat_test_suite* suite) test_body
#define SET_UP(body) static void cheat_set_up(void) body
#define TEAR_DOWN(body) static void cheat_tear_down(void) body
#define GLOBALS(body) body

#define cheat_assert(assertion) cheat_test_assert(suite, assertion, #assertion, __FILE__, __LINE__)

#endif

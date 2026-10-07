#include "platform/platform.h"
#include "platform/platform_process.h"
#include "test_platform_helpers.h"

#include <errno.h>

#ifdef _WIN32
#include <stdio.h>
#include <windows.h>
#else
#include <signal.h>
#endif

static void test_platform_misc_helpers(void) {
	ASSERT_TRUE(spine_platform_process_id() > 0);
	ASSERT_TRUE(spine_platform_stdout_is_terminal() == 0 || spine_platform_stdout_is_terminal() == 1);
	ASSERT_TRUE(spine_platform_stderr_is_terminal() == 0 || spine_platform_stderr_is_terminal() == 1);
}

static void test_platform_pipe_helpers(void) {
	int pipe_fds[2];

	ASSERT_INT_EQ(spine_process_pipe(pipe_fds), 0);
	ASSERT_INT_EQ(spine_process_close_fd(pipe_fds[0]), 0);
	ASSERT_INT_EQ(spine_process_close_fd(pipe_fds[1]), 0);
}

static void test_platform_spawn_and_wait(void) {
	spine_pid_t pid;
	int status;
#ifdef _WIN32
	char cmd_path[] = "C:\\Windows\\System32\\cmd.exe";
	char cmd_flag[] = "/c";
	char cmd_body[] = "exit 0";
	char *argv[] = { cmd_path, cmd_flag, cmd_body, NULL };
#else
	char shell_path[] = "/bin/sh";
	char shell_flag[] = "-c";
	char shell_body[] = "exit 0";
	char *argv[] = { shell_path, shell_flag, shell_body, NULL };
#endif

	ASSERT_INT_EQ(spine_process_spawn_retry(&pid, argv[0], NULL, NULL, argv, NULL, 1, 1000), 0);
	ASSERT_INT_EQ(spine_process_wait(pid, &status), 0);
	ASSERT_INT_EQ(status, 0);
}

static void test_platform_spawn_and_terminate(void) {
	spine_pid_t pid;
	int status;
#ifdef _WIN32
	char cmd_path[] = "C:\\Windows\\System32\\cmd.exe";
	char cmd_flag[] = "/c";
	char cmd_body[] = "ping -n 3 127.0.0.1 >NUL";
	char *argv[] = { cmd_path, cmd_flag, cmd_body, NULL };
#else
	/* Own the child signal policy as well as its PID. Test runners may
	 * ignore/block SIGTERM, which posix_spawn otherwise passes to children. */
	char sleep_path[] = "/bin/sleep";
	char sleep_duration[] = "30";
	char *argv[] = { sleep_path, sleep_duration, NULL };
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t mask;
	struct sigaction ignored = {0};
	struct sigaction previous;
	ignored.sa_handler = SIG_IGN;
	sigemptyset(&ignored.sa_mask);
	int initialized = posix_spawnattr_init(&attributes);
	ASSERT_INT_EQ(initialized, 0);
	if (initialized != 0) return;
	int policy_set = sigaction(SIGTERM, &ignored, &previous);
	ASSERT_INT_EQ(policy_set, 0);
	if (policy_set != 0) {
		posix_spawnattr_destroy(&attributes);
		return;
	}
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGTERM);
	sigemptyset(&mask);
	ASSERT_INT_EQ(posix_spawnattr_setsigdefault(&attributes, &defaults), 0);
	ASSERT_INT_EQ(posix_spawnattr_setsigmask(&attributes, &mask), 0);
	ASSERT_INT_EQ(posix_spawnattr_setflags(&attributes,
		POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK), 0);
#endif

#ifdef _WIN32
	ASSERT_INT_EQ(spine_process_spawn_retry(&pid, argv[0], NULL, NULL, argv, NULL, 1, 1000), 0);
#else
	int spawned = spine_process_spawn_retry(&pid, argv[0], NULL, &attributes, argv, NULL, 1, 1000);
	ASSERT_INT_EQ(sigaction(SIGTERM, &previous, NULL), 0);
	ASSERT_INT_EQ(posix_spawnattr_destroy(&attributes), 0);
	ASSERT_INT_EQ(spawned, 0);
	if (spawned != 0) return;
#endif
	ASSERT_INT_EQ(spine_process_terminate(pid), 0);
	ASSERT_INT_EQ(spine_process_wait(pid, &status), 0);
	ASSERT_TRUE(status != 0);
}

#ifdef _WIN32
static void test_platform_spawn_utf8_path_argument(void) {
	wchar_t temp_dir[MAX_PATH];
	wchar_t script_path[MAX_PATH];
	HANDLE script_handle;
	DWORD bytes_written;
	const char script_body[] = "@echo off\r\nexit /b 0\r\n";
	int utf8_len;
	char utf8_script_path[MAX_PATH * 4];
	spine_pid_t pid;
	int status;
	char cmd_path[] = "C:\\Windows\\System32\\cmd.exe";
	char cmd_flag[] = "/c";
	char *argv[] = { cmd_path, cmd_flag, utf8_script_path, NULL };

	ASSERT_TRUE(GetTempPathW(MAX_PATH, temp_dir) > 0);
	if (swprintf(script_path, MAX_PATH, L"%ls%ls", temp_dir, L"spine-utf8-\x03A9.cmd") < 0) {
		ASSERT_TRUE(0);
		return;
	}

	script_handle = CreateFileW(
		script_path,
		GENERIC_WRITE,
		0,
		NULL,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);
	ASSERT_TRUE(script_handle != INVALID_HANDLE_VALUE);
	if (script_handle == INVALID_HANDLE_VALUE) {
		return;
	}

	ASSERT_TRUE(WriteFile(script_handle, script_body, (DWORD) (sizeof(script_body) - 1), &bytes_written, NULL) != 0);
	CloseHandle(script_handle);

	utf8_len = WideCharToMultiByte(CP_UTF8, 0, script_path, -1, utf8_script_path, (int) sizeof(utf8_script_path), NULL, NULL);
	ASSERT_TRUE(utf8_len > 0);

	ASSERT_INT_EQ(spine_process_spawn_retry(&pid, argv[0], NULL, NULL, argv, NULL, 1, 1000), 0);
	ASSERT_INT_EQ(spine_process_wait(pid, &status), 0);
	ASSERT_INT_EQ(status, 0);

	DeleteFileW(script_path);
}

static void test_platform_spawn_custom_env_not_supported(void) {
	spine_pid_t pid;
	char cmd_path[] = "C:\\Windows\\System32\\cmd.exe";
	char cmd_flag[] = "/c";
	char cmd_body[] = "exit 0";
	char *argv[] = { cmd_path, cmd_flag, cmd_body, NULL };
	char *envp[] = { "SPINE_TEST_ENV=1", NULL };

	ASSERT_INT_EQ(spine_process_spawn_retry(&pid, argv[0], NULL, NULL, argv, envp, 1, 1000), ENOTSUP);
}
#endif

int main(void) {
	test_platform_misc_helpers();
	test_platform_pipe_helpers();
	test_platform_spawn_and_wait();
	test_platform_spawn_and_terminate();
#ifdef _WIN32
	test_platform_spawn_utf8_path_argument();
	test_platform_spawn_custom_env_not_supported();
#endif
	return finish_tests("platform process tests");
}

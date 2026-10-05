/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version. 	   |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 |                                                                         |
 | You should have received a copy of the GNU Lesser General Public        |
 | License along with this library; if not, write to the Free Software     |
 | Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA           |
 | 02110-1301, USA                                                         |
 |                                                                         |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
 | This poller would not have been possible without:                       |
 |   - Larry Adams (current development and enhancements)                  |
 |   - Rivo Nurges (rrd support, mysql poller cache, misc functions)       |
 |   - RTG (core poller code, pthreads, snmp, autoconf examples)           |
 |   - Brady Alleman/Doug Warner (threading ideas, implementation details) |
 +-------------------------------------------------------------------------+
 | - Cacti - http://www.cacti.net/                                         |
 +-------------------------------------------------------------------------+
*/

#include "common.h"
#include "spine.h"

/*! \fn char *php_cmd(const char *php_command, int php_process)
 *  \brief calls the script server and executes a script command
 *  \param php_command the formatted php script server command
 *  \param php_process the php script server process to call
 *
 *  This function is called directly by the spine poller when a script server
 *  request has been initiated for a host.  It will place the PHP Script Server
 *  command on it's output pipe and then wait the pre-defined timeout period for
 *  a response on the PHP Script Servers output pipe.
 *
 *  \return pointer to the string results.  Must be freed by the parent.
 *
 */
static char *php_undefined_result(void) {
	char *result = strdup("U");
	if (result == NULL) die("ERROR: Fatal malloc error: PHP undefined result!");
	return result;
}

static bool php_write_command(int fd, const char *command) {
	size_t sent = 0;
	size_t length = strlen(command);
	while (sent < length) {
		ssize_t bytes = write(fd, command + sent, length - sent);
		if (bytes < 0 && errno == EINTR) continue;
		if (bytes <= 0) return false;
		sent += (size_t)bytes;
	}
	return true;
}

char *php_cmd(const char *php_command, int php_process) {
	static const int process_locks[MAX_PHP_SERVERS] = {
		LOCK_PHP_PROC_0, LOCK_PHP_PROC_1, LOCK_PHP_PROC_2, LOCK_PHP_PROC_3,
		LOCK_PHP_PROC_4, LOCK_PHP_PROC_5, LOCK_PHP_PROC_6, LOCK_PHP_PROC_7,
		LOCK_PHP_PROC_8, LOCK_PHP_PROC_9, LOCK_PHP_PROC_10, LOCK_PHP_PROC_11,
		LOCK_PHP_PROC_12, LOCK_PHP_PROC_13, LOCK_PHP_PROC_14
	};
	assert(php_command != NULL);
	char command[BUFSIZE];
	/* A request occupies one protocol line, including CR-LF and its NUL. */
	if (strlen(php_command) > sizeof(command) - 3 || strpbrk(php_command, "\r\n") != NULL) {
		SPINE_LOG(("ERROR: SS[%i] Invalid PHP Script Server command framing or length", php_process));
		return php_undefined_result();
	}
	if (php_process < 0 || php_process >= set.php_servers || php_process >= MAX_PHP_SERVERS || php_processes == NULL) {
		SPINE_LOG(("ERROR: SS[%i] Invalid PHP Script Server process", php_process));
		return php_undefined_result();
	}
	spine_snprintf(command, sizeof(command), "%s\r\n", php_command);
	int lock = process_locks[php_process];
	thread_mutex_lock(lock);
	char *result = NULL;
	for (int retry = 0; retry < 3; retry++) {
		if (php_write_command(php_processes[php_process].php_write_fd, command)) {
			result = php_readpipe(php_process, command);
			if (result[0] == '\0') SET_UNDEFINED(result);
			break;
		}
		SPINE_LOG(("ERROR: SS[%i] PHP Script Server communications lost sending command. Restarting PHP Script Server", php_process));
		php_close(php_process);
		if (!php_init(php_process)) break;
	}
	thread_mutex_unlock(lock);
	return result != NULL ? result : php_undefined_result();
}

/*!  \fn in php_get_process()
 *  \brief returns the next php script server process to utilize
 *
 *  This very simple function simply returns the next PHP Script Server
 *  process id to poll using a round robin algorithm.
 *
 *  \return the integer number of the next script server to use
 *
 */
int php_get_process(void) {
	int i;

	thread_mutex_lock(LOCK_PHP);
	if (set.php_current_server >= set.php_servers) {
		set.php_current_server = 0;
	}
	i = set.php_current_server;
	set.php_current_server++;
	thread_mutex_unlock(LOCK_PHP);

	return i;
}

/*! \fn char *php_readpipe(int php_process, char *command)
 *  \brief read a line from a PHP Script Server process
 *  \param php_process the PHP Script Server process to obtain output from
 *
 *  This function will read the output pipe from the PHP Script Server process
 *  and return that string to the Spine thread requesting the output.  If for
 *  some reason the PHP Script Server process does not respond in time, it will
 *  be closed using the php_close function, then restarted.
 *
 *  \return a string pointer to the PHP Script Server response
 */
char *php_readpipe(int php_process, const char *command) {
	if (php_processes == NULL || php_process < 0 || php_process >= set.php_servers || php_process >= MAX_PHP_SERVERS) {
		return php_undefined_result();
	}
	fd_set fds;
	struct timeval timeout;
	double begin_time = 0;
	double end_time = 0;
	double remaining_usec = 0;
	char *result_string;

	ssize_t i;
	char *bptr;

	if (!(result_string = (char *)malloc(RESULTS_BUFFER))) {
		die("ERROR: Fatal malloc error: php.c php_readpipe!");
	}
	result_string[0] = '\0';

	/* record start time */
	begin_time = get_time_as_double();

	/* establish timeout value for the PHP script server to respond */
	timeout.tv_sec = set.script_timeout;
	timeout.tv_usec = 0;

	/* check to see which pipe talked and take action
	 * should only be the READ pipe */
	retry:

	/* initialize file descriptors to review for input/output */
	FD_ZERO(&fds);
	FD_SET(php_processes[php_process].php_read_fd,&fds);

	switch (select(php_processes[php_process].php_read_fd+1, &fds, NULL, NULL, &timeout)) {
	case -1:
		switch (errno) {
			case EBADF:
				SPINE_LOG(("ERROR: SS[%i] An invalid file descriptor was given in one of the sets.", php_process));
				break;
			case EINTR:
				#ifndef SOLAR_THREAD
				/* take a moment */
				usleep(2000);
				#endif

				/* record end time */
				end_time = get_time_as_double();

				/* re-establish new timeout value */
				timeout.tv_sec  = rint(floor(set.script_timeout-(end_time-begin_time)));
				remaining_usec  = set.script_timeout - timeout.tv_sec - (end_time - begin_time);

				if (remaining_usec > 0) {
					timeout.tv_usec = rint(remaining_usec * 1000000);
				} else {
					timeout.tv_usec = 0;
				}

				if (timeout.tv_sec + timeout.tv_usec > 0) {
					goto retry;
				} else {
					SPINE_LOG(("WARNING: SS[%i] The Script Server script timed out while processing EINTR's.", php_process));
				}

				break;
			case EINVAL:
				SPINE_LOG(("ERROR: SS[%i] N is negative or the value contained within timeout is invalid.", php_process));
				break;
			case ENOMEM:
				SPINE_LOG(("ERROR: SS[%i] Select was unable to allocate memory for internal tables.", php_process));
				break;
			default:
				SPINE_LOG(("ERROR: SS[%i] Unknown fatal select() error", php_process));
				break;
		}

		SET_UNDEFINED(result_string);

		/* kill script server because it is misbehaving */
		php_close(php_process);
		if (strcmp(command, "INIT") != 0) php_init(php_process);
		break;
	case 0:
		/* record end time */
		end_time = get_time_as_double();
		SPINE_LOG(("WARNING: SS[%i] The PHP Script Server did not respond in time for Timeout[%0.2f], Command[%s] and will therefore be restarted", php_process, end_time - begin_time, command));
		SET_UNDEFINED(result_string);

		/* kill script server because it is misbehaving */
		php_close(php_process);
		if (strcmp(command, "INIT") != 0) php_init(php_process);
		break;
	default:
		if (FD_ISSET(php_processes[php_process].php_read_fd, &fds)) {
			bptr = result_string;

			while (1) {
				i = read(php_processes[php_process].php_read_fd, bptr, RESULTS_BUFFER - 1 - (bptr - result_string));

				if (i <= 0) {
					SET_UNDEFINED(result_string);
					break;
				}

				bptr += i;
				*bptr = '\0';	/* make what we've got into a string */

				if (strchr(result_string, '\n') != NULL) {
					break;
				}

				if (bptr >= result_string + RESULTS_BUFFER - 1) {
					SPINE_LOG(("ERROR: SS[%i] The Script Server result was longer than the acceptable range", php_process));
					SET_UNDEFINED(result_string);
					break;
				}
			}
		} else {
			SPINE_LOG(("ERROR: SS[%i] The FD was not set as expected", php_process));
			SET_UNDEFINED(result_string);
		}

		php_processes[php_process].php_state = PHP_READY;
	}

	return result_string;
}

/*! \fn int php_init(int php_process)
 *  \brief initialize either a specific PHP Script Server or all of them.
 *  \param php_process the process number to start or PHP_INIT
 *
 *  This function will either start an individual PHP Script Server process
 *  or all of them if the input parameter is the PHP_INIT constant.  The function
 *  will check the status of the process to verify that it is ready to process
 *  scripts as well.
 *
 *  \return TRUE if the PHP Script Server is know running or FALSE otherwise
 */
static void php_build_arguments(char **argv, char *poller_id, size_t capacity) {
	argv[0] = set.path_php;
	argv[1] = "-q";
	argv[2] = set.path_php_server;
	if (set.cacti_version <= 1222) {
		argv[3] = "spine";
		spine_snprintf(poller_id, capacity, "%d", set.poller_id);
		argv[4] = poller_id;
		argv[5] = NULL;
		return;
	}
	argv[3] = "--environ=spine";
	spine_snprintf(poller_id, capacity, "--poller=%d", set.poller_id);
	argv[4] = poller_id;
	argv[5] = NULL;
	if (set.poller_id > 1) {
		argv[5] = set.mode == REMOTE_ONLINE ? "--mode=online" : "--mode=offline";
		argv[6] = NULL;
	}
}

static pid_t php_fork_server(int process) {
	for (int attempt = 0; attempt < 4; attempt++) {
		pid_t pid = fork();
		if (pid >= 0) return pid;
		if (errno != EAGAIN && errno != ENOMEM) break;
		#ifndef SOLAR_THREAD
		usleep(50000);
		#endif
	}
	SPINE_LOG(("ERROR: SS[%i] Could not fork PHP Script Server: %s", process, strerror(errno)));
	return -1;
}

static bool php_start_process(int process) {
	int requests[2];
	int responses[2];
	char poller_id[TINY_BUFSIZE];
	char *argv[7];
	int cancel_state;
	php_build_arguments(argv, poller_id, sizeof(poller_id));
	if (pipe(requests) < 0) {
		SPINE_LOG(("ERROR: SS[%i] Could not allocate PHP request pipe", process));
		return FALSE;
	}
	if (pipe(responses) < 0) {
		close(requests[0]);
		close(requests[1]);
		SPINE_LOG(("ERROR: SS[%i] Could not allocate PHP response pipe", process));
		return FALSE;
	}
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
	pid_t pid = php_fork_server(process);
	if (pid == 0) {
		if (dup2(requests[0], STDIN_FILENO) < 0 || dup2(responses[1], STDOUT_FILENO) < 0) _exit(127);
		int descriptors[] = {requests[0], requests[1], responses[0], responses[1]};
		for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
			if (descriptors[i] != STDIN_FILENO && descriptors[i] != STDOUT_FILENO) close(descriptors[i]);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	close(requests[0]);
	close(responses[1]);
	if (pid < 0) {
		close(requests[1]);
		close(responses[0]);
		pthread_setcancelstate(cancel_state, NULL);
		return FALSE;
	}
	php_t *server = &php_processes[process];
	server->php_pid = pid;
	server->php_write_fd = requests[1];
	server->php_read_fd = responses[0];
	server->php_state = PHP_BUSY;
	pthread_setcancelstate(cancel_state, NULL);
	char *result = php_readpipe(process, "INIT");
	bool started = strstr(result, "Started") != NULL;
	if (started) {
		SPINE_LOG_DEBUG(("DEBUG: SS[%i] Confirmed PHP Script Server running using readfd[%i], writefd[%i]",
			process, server->php_read_fd, server->php_write_fd));
		server->php_state = PHP_READY;
	} else {
		SPINE_LOG(("ERROR: SS[%i] Script Server did not start properly return message was: '%s'", process, result));
		server->php_state = PHP_BUSY;
		php_close(process);
	}
	free(result);
	return started;
}

int php_init(int php_process) {
	if (php_processes == NULL || set.php_servers < 0 || set.php_servers > MAX_PHP_SERVERS) return FALSE;
	if (php_process != PHP_INIT) {
		if (php_process < 0 || php_process >= set.php_servers) return FALSE;
		return php_start_process(php_process);
	}
	for (int process = 0; process < set.php_servers; process++) {
		if (!php_start_process(process)) return FALSE;
	}
	return TRUE;
}

/*! \fn void php_close(int php_process)
 *  \brief close the php script server process
 *  \param php_process the process to close or PHP_INIT
 *
 *  This function will take an input parameter of either a specially coded
 *  PHP_INIT parameter or an integer stating the process number.  With that
 *  information is will close and/or terminate the child PHP Script Server
 *  process and then return to the calling function.
 *
 *  TODO: Make ending of the child process not be reliant on SIG_TERM in cases
 *  where the child process is hung for one reason or another.
 *
 */
void php_close(int php_process) {
	int i;
	int num_processes;
	ssize_t len;

	if (php_process == PHP_INIT) {
		num_processes = set.php_servers;
	} else {
		num_processes = 1;
	}

	for(i = 0; i < num_processes; i++) {
		php_t *phpp;

		SPINE_LOG_DEBUG(("DEBUG: SS[%i] Script Server Shutdown Started", i));

		/* tell the script server to close */
		if (php_process == PHP_INIT) {
			phpp = &php_processes[i];
		} else {
			phpp = &php_processes[php_process];
		}

		/* If we still have a valid write pipe, tell PHP to close down
		 * by sending a "quit" message, then closing the input channel
		 * so it gets an EOF.
		 *
		 * Then we wait a moment before actually killing it to allow for
		 * a clean shutdown.
		 */
		if (phpp->php_write_fd >= 0) {
			static const char quit[] = "quit\r\n";

			len = write(phpp->php_write_fd, quit, strlen(quit));

			if (len >= 0) {
				close(phpp->php_write_fd);
				phpp->php_write_fd = -1;
			}

			/* wait before killing php */
			#ifndef SOLAR_THREAD
			usleep(50000);			/* 50 msec */
			#endif
		}

		/* only try to kill the process if the PID looks valid.
		 * Trying to kill a negative number is bad news (it's
	 	 * a process group leader), and PID 1 is "init".
	  	 */
		if (phpp->php_pid > 1) {
			/* end the php script server process */
			kill(phpp->php_pid, SIGTERM);

			/* reset this PID variable? */
		}

		/* close file descriptors */
		close(phpp->php_read_fd);
		phpp->php_read_fd  = -1;
	}
}

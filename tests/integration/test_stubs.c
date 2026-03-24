/*
 +-------------------------------------------------------------------------+
 | Stubs for spine symbols not needed by integration tests.               |
 | Links against SPINE_OBJS_CORE without pulling in spine.o/php.o/etc.   |
 +-------------------------------------------------------------------------+
*/

#include <stdlib.h>
#include "common.h"
#include "spine.h"

/* spine.c globals */
int *debug_devices = NULL;
int  set_option_enabled = 0;
poller_thread_t **details = NULL;

/* php.c functions */
char *php_cmd(const char *php_command, int php_process) {
	(void)php_command;
	(void)php_process;
	return NULL;
}

int php_init(int php_process) {
	(void)php_process;
	return 0;
}

void php_close(int php_process) {
	(void)php_process;
}

int php_get_process(void) {
	return 0;
}

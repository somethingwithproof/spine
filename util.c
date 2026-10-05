/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.      |
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
#include "regex.h"
#include <stddef.h>
#include <limits.h>

/* Bounded formatting must reject truncation before a partial SQL statement or
 * command can be used. Return the number actually written, never the size that
 * would have been needed, so append callers cannot advance past the buffer. */
int spine_snprintf(char *output, size_t capacity, const char *format, ...) {
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(output, capacity, format, args);
	va_end(args);
	if (length < 0 || (size_t) length >= capacity) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Formatted output exceeds its destination buffer");
	}
	return length;
}

int spine_count_to_int(unsigned long long count) {
	if (count > INT_MAX) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Result count exceeds supported integer range");
	}
	return (int)count;
}

void spine_sleep_usec(unsigned int microseconds) {
	struct timespec requested = {
		(time_t)(microseconds / 1000000), (long)(microseconds % 1000000) * 1000
	};
	while (nanosleep(&requested, &requested) != 0) {
		if (errno != EINTR) {
			set.exit_code = EXIT_FAILURE;
			die("ERROR: Unable to wait for retry delay");
		}
	}
}

double spine_monotonic_time(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Unable to read monotonic clock");
	}
	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000;
}

static int spine_wait_fd(int fd, double deadline, bool writable) {
	if (fd < 0 || fd >= FD_SETSIZE) {
		errno = EBADF;
		return -1;
	}
	if (!isfinite(deadline)) {
		errno = EINVAL;
		return -1;
	}
	for (;;) {
		double remaining = deadline - spine_monotonic_time();
		if (remaining <= 0) return 0;
		if (remaining > INT_MAX) {
			errno = EINVAL;
			return -1;
		}
		struct timeval timeout;
		timeout.tv_sec = (time_t)remaining;
		timeout.tv_usec = (suseconds_t)((remaining - (double)timeout.tv_sec) * 1000000);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int status = select(fd + 1, writable ? NULL : &fds, writable ? &fds : NULL, NULL, &timeout);
		if (status < 0 && errno == EINTR) continue;
		return status;
	}
}

int spine_wait_readable(int fd, double deadline) {
	return spine_wait_fd(fd, deadline, FALSE);
}

int spine_wait_writable(int fd, double deadline) {
	return spine_wait_fd(fd, deadline, TRUE);
}


int spine_permits_init(spine_permits_t *permits, int count) {
	if (count < 0) return EINVAL;
	int status = pthread_mutex_init(&permits->mutex, NULL);
	if (status == 0) permits->available = count;
	return status;
}

int spine_permits_destroy(spine_permits_t *permits) {
	return pthread_mutex_destroy(&permits->mutex);
}

static void spine_permits_lock(spine_permits_t *permits) {
	if (pthread_mutex_lock(&permits->mutex) != 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Unable to lock process permits");
	}
}

static void spine_permits_unlock(spine_permits_t *permits) {
	if (pthread_mutex_unlock(&permits->mutex) != 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Unable to unlock process permits");
	}
}

int spine_permits_try_acquire(spine_permits_t *permits) {
	spine_permits_lock(permits);
	int status = EAGAIN;
	if (permits->available > 0) {
		permits->available--;
		status = 0;
	}
	spine_permits_unlock(permits);
	return status;
}

int spine_permits_release(spine_permits_t *permits) {
	spine_permits_lock(permits);
	int status = EOVERFLOW;
	if (permits->available < INT_MAX) {
		permits->available++;
		status = 0;
	}
	spine_permits_unlock(permits);
	return status;
}

int spine_permits_available(spine_permits_t *permits) {
	spine_permits_lock(permits);
	int available = permits->available;
	spine_permits_unlock(permits);
	return available;
}

void spine_clear_sensitive(void *buffer, size_t length) {
	volatile unsigned char *bytes = buffer;
	while (length > 0) {
		*bytes++ = 0;
		length--;
	}
}

/* Preserve printable device output while keeping each log record on one line.
 * Also remove terminal controls and Unicode line/paragraph separators. */
void spine_sanitize_log_message(char *message) {
	for (unsigned char *p = (unsigned char *)message; *p != 0; p++) {
		if (*p < 0x20 || *p == 0x7f) {
			*p = ' ';
		} else if (p[0] == 0xc2 && p[1] == 0x85) {
			p[0] = ' ';
			p[1] = ' ';
		} else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
			p[0] = ' ';
			p[1] = ' ';
			p[2] = ' ';
		}
	}
}

static int nopts = 0;

/*! Override Options Structure
 *
 * When we fetch a setting from the database, we allow the user to override
 * it from the command line. These overrides are provided with the --option
 * parameter and stored in this table: we *use* them when the config code
 * reads from the DB.
 *
 * It's not an error to set an option which is unknown, but maybe should be.
 *
 */
static struct {
	const char *opt;
	const char *val;
} opttable[256];

/*! \fn void set_option(const char *option, const char *value)
 *  \brief Override spine setting from the Cacti settings table.
 *
 *	Called from the command-line processing code, this provides a value
 *	to replace any DB-stored option settings.
 *
 */
void set_option(const char *option, const char *value) {
	opttable[nopts  ].opt = option;
	opttable[nopts++].val = value;
}

/*! \fn static char *getsetting(MYSQL *psql, int mode, const char *setting)
 *  \brief Returns a character pointer to a Cacti setting.
 *
 *  Given a pointer to a database and the name of a setting, return the string
 *  which represents the value from the settings table. Return NULL if we
 *  can't find a setting for whatever reason.
 *
 *  NOTE: if the user has provided one of these options on the command line,
 *  it's intercepted here and returned, overriding the database setting.
 *
 *  \return the database option setting
 *
 */
static char *getsetting(MYSQL *psql, int mode, const char *setting) {
	char      qstring[BUFSIZE];
	char      *retval;
	MYSQL_RES *result;
	MYSQL_ROW mysql_row;

	assert(psql    != 0);
	assert(setting != 0);

	/* see if it's in the option table */
	for (int i = 0; i < nopts; i++) {
		if (STRIMATCH(setting, opttable[i].opt)) {
			/* FOUND IT! */
			retval = strdup(opttable[i].val);
			return retval;
		}
	}

	spine_snprintf(qstring, sizeof(qstring), "SELECT SQL_NO_CACHE value FROM settings WHERE name = '%s'", setting);

	result = db_query(psql, mode, qstring);
	if (result == NULL) return strdup("");
	mysql_row = mysql_num_rows(result) > 0 ? mysql_fetch_row(result) : NULL;
	retval = mysql_row != NULL && mysql_row[0] != NULL ? strdup(mysql_row[0]) : strdup("");
	db_free_result(result);
	return retval;
}

/*! \fn int putsetting(MYSQL *psql, const char *setting, const char *value)
 *  \brief Set's a specific Cacti setting.
 *
 *  Given a pointer to a database and the name of a setting, and value of that setting
 *  set the Cacti setting in the database to the value.
 *
 *  \return true for successful or false for failed
 *
 */
int putsetting(MYSQL *psql, int mode, const char *mysetting, const char *myvalue) {
	char  qstring[BUFSIZE];
	int   result = 0;

	assert(psql    != 0);
	assert(mysetting != 0);
	assert(myvalue   != 0);

	if (set.dbonupdate == 0) {
		spine_snprintf(qstring, sizeof(qstring), "INSERT INTO settings (name, value) "
			"VALUES ('%s', '%s') "
			"ON DUPLICATE KEY UPDATE value = VALUES(value)", mysetting, myvalue);
	} else {
		spine_snprintf(qstring, sizeof(qstring), "INSERT INTO settings (name, value) "
			"VALUES ('%s', '%s') AS rs "
			"ON DUPLICATE KEY UPDATE value = rs.value", mysetting, myvalue);
	}

	result = db_insert(psql, mode, qstring);

	if (result == 0) {
		return TRUE;
	} else {
		return FALSE;
	}
}

/*! \fn static char *getpsetting(MYSQL *psql, const char *setting)
 *  \brief Returns a character pointer to a Cacti poller setting.
 *
 *  Given a pointer to a database and the name of a setting,
 *  return the string which represents the value from the poller table.
 *  Return NULL if we can't find a setting for whatever reason.
 *
 *  NOTE: if the user has provided one of these options on the command line,
 *  it's intercepted here and returned, overriding the database setting.
 *
 *  \return the database option setting
 *
 */
static char *getpsetting(MYSQL *psql, int mode, const char *setting) {
	char      qstring[BUFSIZE];
	char      *retval;
	MYSQL_RES *result;
	MYSQL_ROW mysql_row;

	assert(psql    != 0);
	assert(setting != 0);

	/* see if it's in the option table */
	for (int i = 0; i < nopts; i++) {
		if (STRIMATCH(setting, opttable[i].opt)) {
			/* FOUND IT! */
			retval = strdup(opttable[i].val);
			return retval;
		}
	}

	spine_snprintf(qstring, sizeof(qstring), "SELECT SQL_NO_CACHE %s FROM poller WHERE id = '%d'", setting, set.poller_id);

	result = db_query(psql, mode, qstring);
	if (result == NULL) return NULL;
	mysql_row = mysql_num_rows(result) > 0 ? mysql_fetch_row(result) : NULL;
	retval = mysql_row != NULL && mysql_row[0] != NULL ? strdup(mysql_row[0]) : NULL;
	db_free_result(result);
	return retval;
}

/*! \fn static int getboolsetting(MYSQL *psql, int mode, const char *setting, int dflt)
 *  \brief Obtains a boolean option from the database.
 *
 *	Given the parameters for fetching a setting from the database,
 *	do so for a *Boolean* value. We parse the usual set of words
 *	meaning true/false, and if we don't get a value, or if we don't
 *	understand what we fetched, we use the default value provided.
 *
 *  \return boolean TRUE or FALSE based upon database setting or the DEFAULT if not found
 */
static int getboolsetting(MYSQL *psql, int mode, const char *setting, int dflt) {
	char *rc;

	assert(psql    != 0);
	assert(setting != 0);

	rc = getsetting(psql, mode, setting);

	if (rc == 0) return dflt;

	if (STRIMATCH(rc, "on"  ) ||
		STRIMATCH(rc, "yes" ) ||
		STRIMATCH(rc, "true") ||
		STRIMATCH(rc, "1"   ) ) {
		free(rc);
		return TRUE;
	}

	if (STRIMATCH(rc, "off"  ) ||
		STRIMATCH(rc, "no"   ) ||
		STRIMATCH(rc, "false") ||
		STRIMATCH(rc, "0"    ) ) {
		free(rc);
		return FALSE;
	}

	/* doesn't really match one of our keywords: what to do? */
	free(rc);

	return dflt;
}

/*! \fn static char *getglobalvariable(MYSQL *psql, const char *setting)
 *  \brief Returns a character pointer to a MySQL global variable setting.
 *
 *  Given a pointer to a database and the name of a global variable, return the string
 *  which represents that value from the settings table. Return NULL if we
 *  can't find a variable for whatever reason.
 *
 *  \return the database global variable setting
 *
 */
static char *getglobalvariable(MYSQL *psql, int mode, const char *setting) {
	char      qstring[BUFSIZE];
	char      *retval;
	MYSQL_RES *result;
	MYSQL_ROW mysql_row;

	assert(psql    != 0);
	assert(setting != 0);

	/* see if it's in the option table */
	for (int i = 0; i < nopts; i++) {
		if (STRIMATCH(setting, opttable[i].opt)) {
			/* FOUND IT! */
			return strdup(opttable[i].val);
		}
	}

	spine_snprintf(qstring, sizeof(qstring), "SHOW GLOBAL VARIABLES LIKE '%s'", setting);

	result = db_query(psql, mode, qstring);
	if (result == NULL) return NULL;
	mysql_row = mysql_num_rows(result) > 0 ? mysql_fetch_row(result) : NULL;
	retval = mysql_row != NULL && mysql_row[1] != NULL ? strdup(mysql_row[1]) : NULL;
	db_free_result(result);
	return retval;
}

/*! \fn int is_debug_device(int device_id)
 *  \brief Determine if a device is a debug device
 *
 */
int is_debug_device(int device_id) {
	extern int *debug_devices;
	int i = 0;

	while (i < 100) {
		if (debug_devices[i] == '\0') break;
		if (debug_devices[i] == device_id) {
			return TRUE;
		}

		i++;
	}

	return FALSE;
}

void parse_debug_devices(char *device_list, int *devices, size_t capacity) {
	if (capacity == 0) return;
	devices[0] = 0;
	char *saveptr = NULL;
	const char *token = strtok_r(device_list, ",", &saveptr);
	for (size_t i = 0; token != NULL && i < capacity - 1; i++) {
		devices[i] = atoi(token);
		devices[i + 1] = 0;
		token = strtok_r(NULL, ",", &saveptr);
	}
}

/*! \fn void read_config_options(void)
 *  \brief Reads the default Spine runtime parameters from the database and set's the global array
 *
 *  load default values from the database for poller processing
 *
 */
void read_config_options() {
	MYSQL      mysql;
	MYSQL      mysqlr;
	MYSQL_RES  *result;
	int        num_rows;
	int        mode;
	char       web_root[BUFSIZE];
	char       sqlbuf[HUGE_BUFSIZE];
	char       *sqlp;
	char *res;
	char       spine_capabilities[BUFSIZE];

	/* publish spine snmpv3 capabilities to the database */
	memset(spine_capabilities, 0, sizeof(spine_capabilities));

	db_connect(LOCAL, &mysql);

	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		db_connect(REMOTE, &mysqlr);
		mode = REMOTE;
	} else {
		mode = LOCAL;
	}

	/* get the mysql server version */
	if ((res = getglobalvariable(&mysql, LOCAL, "version")) != 0) {
		snprintf(set.dbversion, BUFSIZE, "%s", res);
		free(res);
	}

	if (STRIMATCH(set.dbversion, "mariadb")) {
		set.dbonupdate = 0;
	} else if (strpos(set.dbversion, "8.") == 0) {
		set.dbonupdate = 1;
	} else {
		set.dbonupdate = 0;
	}

	/* get the cacti version from the database */
	set.cacti_version = get_cacti_version(&mysql, LOCAL);

	/* log the path_webroot variable */
	SPINE_LOG_DEBUG(("DEBUG: The binary Cacti version is %d", set.cacti_version));

	/* get logging level from database - overrides spine.conf */
	if ((res = getsetting(&mysql, LOCAL, "log_verbosity")) != 0) {
		const int n = atoi(res);
		free(res);
		if (n != 0) set.log_level = n;
	}

	/* determine script server path operation and default log file processing */
	if ((res = getsetting(&mysql, LOCAL, "path_webroot")) != 0) {
		snprintf(set.path_php_server, BUFSIZE, "%s/script_server.php", res);
		snprintf(web_root, BUFSIZE, "%s", res);
		free(res);
	}

	/* determine logfile path */
	if ((res = getsetting(&mysql, LOCAL, "path_cactilog")) != 0) {
		if (strlen(res) != 0) {
			snprintf(set.path_logfile, DBL_BUFSIZE, "%s", res);
		} else {
			if (strlen(web_root) != 0) {
				snprintf(set.path_logfile, DBL_BUFSIZE, "%s/log/cacti.log", web_root);
			} else {
				set.path_logfile[0] ='\0';
			}
		}
		free(res);
	} else {
		snprintf(set.path_logfile, DBL_BUFSIZE, "%s/log/cacti.log", web_root);
 	}

	/* get log separator */
	if ((res = getsetting(&mysql, LOCAL, "default_datechar")) != 0) {
		set.log_datetime_separator = atoi(res);
		free(res);

		if (set.log_datetime_separator < GDC_MIN || set.log_datetime_separator > GDC_MAX) {
			set.log_datetime_separator = GDC_DEFAULT;
		}
	}

	/* get log separator */
	if ((res = getsetting(&mysql, LOCAL, "default_datechar")) != 0) {
		set.log_datetime_separator = atoi(res);
		free(res);

		if (set.log_datetime_separator < GDC_MIN || set.log_datetime_separator > GDC_MAX) {
			set.log_datetime_separator = GDC_DEFAULT;
		}
	}

	/* determine log file, syslog or both, default is 1 or log file only */
	if ((res = getsetting(&mysql, LOCAL, "log_destination")) != 0) {
		set.log_destination = parse_logdest(res, LOGDEST_FILE);
		free(res);
	} else {
		set.log_destination = LOGDEST_FILE;
	}

	/* log the path_webroot variable */
	SPINE_LOG_DEBUG(("DEBUG: The path_php_server variable is %s", set.path_php_server));

	/* log the path_cactilog variable */
	SPINE_LOG_DEBUG(("DEBUG: The path_cactilog variable is %s", set.path_logfile));

	/* the version variable */
	SPINE_LOG_DEBUG(("DEBUG: The version variable is %s", set.dbversion));

	/* log the log_destination variable */
	SPINE_LOG_DEBUG(("DEBUG: The log_destination variable is %i (%s)",
		set.log_destination,
		printable_logdest(set.log_destination)));

	set.logfile_processed = TRUE;

	/* get PHP Path Information for Scripting */
	if ((res = getsetting(&mysql, LOCAL, "path_php_binary")) != 0) {
		STRNCOPY(set.path_php, res);
		free(res);
	}

	/* log the path_php variable */
	SPINE_LOG_DEBUG(("DEBUG: The path_php variable is %s", set.path_php));

	/* set availability_method */
	if ((res = getsetting(&mysql, LOCAL, "availability_method")) != 0) {
		set.availability_method = atoi(res);
		free(res);
	}

	/* log the availability_method variable */
	SPINE_LOG_DEBUG(("DEBUG: The availability_method variable is %i", set.availability_method));

	/* set ping_recovery_count */
	if ((res = getsetting(&mysql, LOCAL, "ping_recovery_count")) != 0) {
		set.ping_recovery_count = atoi(res);
		free(res);
	}

	/* log the ping_recovery_count variable */
	SPINE_LOG_DEBUG(("DEBUG: The ping_recovery_count variable is %i", set.ping_recovery_count));

	/* set ping_failure_count */
	if ((res = getsetting(&mysql, LOCAL, "ping_failure_count")) != 0) {
		set.ping_failure_count = atoi(res);
		free(res);
	}

	/* log the ping_failure_count variable */
	SPINE_LOG_DEBUG(("DEBUG: The ping_failure_count variable is %i", set.ping_failure_count));

	/* set ping_method */
	if ((res = getsetting(&mysql, LOCAL, "ping_method")) != 0) {
		set.ping_method = atoi(res);
		free(res);
	}

	/* log the ping_method variable */
	SPINE_LOG_DEBUG(("DEBUG: The ping_method variable is %i", set.ping_method));

	/* set ping_retries */
	if ((res = getsetting(&mysql, LOCAL, "ping_retries")) != 0) {
		set.ping_retries = atoi(res);
		free(res);
	}

	/* log the ping_retries variable */
	SPINE_LOG_DEBUG(("DEBUG: The ping_retries variable is %i", set.ping_retries));

	/* set ping_timeout */
	if ((res = getsetting(&mysql, LOCAL, "ping_timeout")) != 0) {
		set.ping_timeout = atoi(res);
		free(res);
	} else {
		set.ping_timeout = 400;
	}

	/* log the ping_timeout variable */
	SPINE_LOG_DEBUG(("DEBUG: The ping_timeout variable is %i", set.ping_timeout));

	/* set snmp_retries */
	if ((res = getsetting(&mysql, LOCAL, "snmp_retries")) != 0) {
		set.snmp_retries = atoi(res);
		free(res);
	} else {
		set.snmp_retries = 3;
	}

	/* log the snmp_retries variable */
	SPINE_LOG_DEBUG(("DEBUG: The snmp_retries variable is %i", set.snmp_retries));

	/* set logging option for errors */
	set.log_perror = getboolsetting(&mysql, LOCAL, "log_perror", FALSE);

	/* log the log_perror variable */
	SPINE_LOG_DEBUG(("DEBUG: The log_perror variable is %i", set.log_perror));

	/* set logging option for errors */
	set.log_pwarn = getboolsetting(&mysql, LOCAL, "log_pwarn", FALSE);

	/* log the log_pwarn variable */
	SPINE_LOG_DEBUG(("DEBUG: The log_pwarn variable is %i", set.log_pwarn));

	/* set option to increase insert performance */
	set.boost_redirect = getboolsetting(&mysql, LOCAL, "boost_redirect", FALSE);

	/* log the boost_redirect variable */
	SPINE_LOG_DEBUG(("DEBUG: The boost_redirect variable is %i", set.boost_redirect));

	/* set option for determining if boost is enabled */
	set.boost_enabled = getboolsetting(&mysql, LOCAL, "boost_rrd_update_enable", FALSE);

	/* log the boost_rrd_update_enable variable */
	SPINE_LOG_DEBUG(("DEBUG: The boost_rrd_update_enable variable is %i", set.boost_enabled));

	/* set logging option for statistics */
	set.log_pstats = getboolsetting(&mysql, LOCAL, "log_pstats", FALSE);

	/* log the log_pstats variable */
	SPINE_LOG_DEBUG(("DEBUG: The log_pstats variable is %i", set.log_pstats));

	/* get Cacti defined max threads override spine.conf */
	if ((set.threads_set == FALSE) && ((res = getpsetting(&mysql, mode, "threads")) != 0)) {
		set.threads = atoi(res);
		free(res);
		if (set.threads > MAX_THREADS) {
			set.threads = MAX_THREADS;
		}
	}

	/* log the threads variable */
	SPINE_LOG_DEBUG(("DEBUG: The threads variable is %i", set.threads));

	/* get the poller_interval for those who have elected to go with a 1 minute polling interval */
	if ((res = getsetting(&mysql, LOCAL, "poller_interval")) != 0) {
		set.poller_interval = atoi(res);
		free(res);
	} else {
		set.poller_interval = 0;
	}

	/* log the poller_interval variable */
	if (set.poller_interval == 0) {
		SPINE_LOG_DEBUG(("DEBUG: The polling interval is the system default"));
	} else {
		SPINE_LOG_DEBUG(("DEBUG: The polling interval is %i seconds", set.poller_interval));
	}

	/* get the concurrent_processes variable to determine thread sleep values */
	if ((res = getsetting(&mysql, LOCAL, "concurrent_processes")) != 0) {
		set.num_parent_processes = atoi(res);
		free(res);
	} else {
		set.num_parent_processes = 1;
	}

	/* log the concurrent processes variable */
	SPINE_LOG_DEBUG(("DEBUG: The number of concurrent processes is %i", set.num_parent_processes));

	/* get the script timeout to establish timeouts */
	if ((res = getsetting(&mysql, LOCAL, "script_timeout")) != 0) {
		set.script_timeout = atoi(res);
		free(res);
		if (set.script_timeout < 5) {
			set.script_timeout = 5;
		}
	} else {
		set.script_timeout = 25;
	}

	/* log the script timeout value */
	SPINE_LOG_DEBUG(("DEBUG: The script timeout is %i", set.script_timeout));

	/* get selective_device_debug string */
	if ((res = getsetting(&mysql, LOCAL, "selective_device_debug")) != 0) {
		STRNCOPY(set.selective_device_debug, res);
		free(res);
	}

	/* log the selective_device_debug variable */
	SPINE_LOG_DEBUG(("DEBUG: The selective_device_debug variable is %s", set.selective_device_debug));

	/* get spine_log_level */
	if ((res = getsetting(&mysql, LOCAL, "spine_log_level")) != 0) {
		set.spine_log_level = atoi(res);
		free(res);
	}

	/* log the spine_log_level variable */
	SPINE_LOG_DEBUG(("DEBUG: The spine_log_level variable is %i", set.spine_log_level));

	/* get the number of script server processes to run */
	if ((res = getsetting(&mysql, LOCAL, "php_servers")) != 0) {
		set.php_servers = atoi(res);
		free(res);

		if (set.php_servers > MAX_PHP_SERVERS) {
			set.php_servers = MAX_PHP_SERVERS;
		}

		if (set.php_servers <= 0) {
			set.php_servers = 1;
		}
	} else {
		set.php_servers = 2;
	}

	/* log the script timeout value */
	SPINE_LOG_DEBUG(("DEBUG: The number of php script servers to run is %i", set.php_servers));

	/* get the number of active profiles on the system run */
	if ((res = getsetting(&mysql, LOCAL, "active_profiles")) != 0) {
		set.active_profiles = atoi(res);
		free(res);

		if (set.active_profiles <= 0) {
			set.active_profiles = 0;
		}
	} else {
		set.active_profiles = 0;
	}

	/* log the script timeout value */
	SPINE_LOG_DEBUG(("DEBUG: The number of active data source profiles is %i", set.active_profiles));

	/* get the number of snmp_ports in use */
	if ((res = getsetting(&mysql, LOCAL, "total_snmp_ports")) != 0) {
		set.total_snmp_ports = atoi(res);
		free(res);

		if (set.total_snmp_ports <= 0) {
			set.total_snmp_ports = 0;
		}
	} else {
		set.total_snmp_ports = 0;
	}

	/* log the script timeout value */
	SPINE_LOG_DEBUG(("DEBUG: The number of snmp ports on the system is %i", set.total_snmp_ports));

	/*----------------------------------------------------------------
	 * determine if the php script server is required by searching for
	 * all the host records for an action of POLLER_ACTION_PHP_SCRIPT_SERVER.
	 * If we get even one, it means we have to deal with the PHP script
	 * server.
	 *
	 */
	set.php_required = FALSE;		/* assume no */

	/* log the requirement for the script server */
	if (!strlen(set.host_id_list)) {
		sqlp = sqlbuf;
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "SELECT SQL_NO_CACHE action FROM poller_item");
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " WHERE action=%d", POLLER_ACTION_PHP_SCRIPT_SERVER);
		sqlp += append_hostrange(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "host_id");
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " AND poller_id=%i", set.poller_id);
		spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " LIMIT 1");

		result = db_query(&mysql, LOCAL, sqlbuf);
		num_rows = spine_count_to_int(mysql_num_rows(result));
		db_free_result(result);

		if (num_rows > 0) set.php_required = TRUE;

		SPINE_LOG_DEBUG(("DEBUG: StartDevice='%i', EndDevice='%i', TotalPHPScripts='%i'",
			set.start_host_id,
			set.end_host_id,
			num_rows));
	} else {
		sqlp = sqlbuf;
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "SELECT SQL_NO_CACHE action FROM poller_item");
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " WHERE action=%d", POLLER_ACTION_PHP_SCRIPT_SERVER);
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " AND host_id IN(%s)", set.host_id_list);
		sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " AND poller_id=%i", set.poller_id);
		spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " LIMIT 1");

		result = db_query(&mysql, LOCAL, sqlbuf);
		num_rows = spine_count_to_int(mysql_num_rows(result));
		db_free_result(result);

		if (num_rows > 0) set.php_required = TRUE;

		SPINE_LOG_DEBUG(("DEBUG: Device List to be polled='%s', TotalPHPScripts='%i'",
			set.host_id_list,
			num_rows));
	}

	SPINE_LOG_DEBUG(("DEBUG: The PHP Script Server is %sRequired",
		set.php_required
		? ""
		: "Not "));

	/* determine the maximum oid's to obtain in a single get request */
	if ((res = getsetting(&mysql, LOCAL, "max_get_size")) != 0) {
		set.snmp_max_get_size = atoi(res);
		free(res);

		if (set.snmp_max_get_size > 128) {
			set.snmp_max_get_size = 128;
		}
	} else {
		set.snmp_max_get_size = 25;
	}

	/* log the snmp_max_get_size variable */
	SPINE_LOG_DEBUG(("DEBUG: The Maximum SNMP OID Get Size is %i", set.snmp_max_get_size));

	int authCount = 0;

	strcat(spine_capabilities, "{ authProtocols: \"");
	#ifndef NETSNMP_DISABLE_MD5
	strcat(spine_capabilities, "MD5");
	authCount++;
	#endif

	strcat(spine_capabilities, (authCount == 0 ? "SHA":",SHA"));
	authCount++;

	#if defined(NETSNMP_USMAUTH_HMAC128SHA224)
	strcat(spine_capabilities, ",SHA224,SHA256");
	authCount++;
	#endif

	#if defined(NETSNMP_USMAUTH_HMAC192SHA256)
	strcat(spine_capabilities, ",SHA384,SHA512");
	authCount++;
	#endif
	strcat(spine_capabilities, "\"");

	int privCount = 0;

	strcat(spine_capabilities, ", privProtocols: \"");

	#ifndef NETSNMP_DISABLE_DES
	strcat(spine_capabilities, "DES");
	privCount++;
	#endif

	#ifdef HAVE_AES
	strcat(spine_capabilities, (privCount == 0 ? "AES128":",AES128"));
	privCount++;
	#endif

	#if defined(NETSNMP_DRAFT_BLUMENTHAL_AES_04)
	strcat(spine_capabilities, (privCount == 0 ? "AES192":",AES192"));
	privCount++;
	#endif

	#if defined(NETSNMP_DRAFT_BLUMENTHAL_AES_04)
	strcat(spine_capabilities, (privCount == 0 ? "AES256":",AES256"));
	privCount++;
	#endif
	strcat(spine_capabilities, "\" }");

	if (set.poller_id == 1) {
		putsetting(&mysql, LOCAL, "spine_capabilities", spine_capabilities);
	}

	db_disconnect(&mysql);

	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		db_disconnect(&mysqlr);
	}
}

void poller_push_data_to_main() {
	MYSQL      mysql;
	MYSQL      mysqlr;
	MYSQL_RES  *result;
	MYSQL_ROW  row;
	int        num_rows;
	int        rows;
	char       sqlbuf[HUGE_BUFSIZE];
	char       *sqlp;
	char       query[MEGA_BUFSIZE];
	char       prefix[BUFSIZE];
	char       suffix[BUFSIZE];
	// tmpstr needs to be greater than 2 * the maximum column size being processed below
	char       tmpstr[DBL_BUFSIZE];

	db_connect(LOCAL, &mysql);
	db_connect(REMOTE, &mysqlr);

	/* Since MySQL 5.7 the sql_mode defaults are too strict for cacti */
	db_insert(&mysql, LOCAL, "SET SESSION sql_mode = (SELECT REPLACE(@@sql_mode,'NO_ZERO_DATE', ''))");
	db_insert(&mysql, LOCAL, "SET SESSION sql_mode = (SELECT REPLACE(@@sql_mode,'ONLY_FULL_GROUP_BY', ''))");
	db_insert(&mysqlr, REMOTE, "SET SESSION sql_mode = (SELECT REPLACE(@@sql_mode,'NO_ZERO_DATE', ''))");
	db_insert(&mysqlr, REMOTE, "SET SESSION sql_mode = (SELECT REPLACE(@@sql_mode,'ONLY_FULL_GROUP_BY', ''))");

	SPINE_LOG_MEDIUM(("Pushing Host Status to Main Server"));

	if (strlen(set.host_id_list)) {
		snprintf(query, MEGA_BUFSIZE, "SELECT SQL_NO_CACHE id, snmp_sysDescr, snmp_sysObjectID, "
			"snmp_sysUpTimeInstance, snmp_sysContact, snmp_sysName, snmp_sysLocation, "
			"status, status_event_count, status_fail_date, status_rec_date, "
			"status_last_error, min_time, max_time, cur_time, avg_time, polling_time, "
			"total_polls, failed_polls, availability, last_updated "
			"FROM host "
			"WHERE poller_id = %d "
			"AND id IN (%s)", set.poller_id, set.host_id_list);
	} else {
		snprintf(query, MEGA_BUFSIZE, "SELECT SQL_NO_CACHE id, snmp_sysDescr, snmp_sysObjectID, "
			"snmp_sysUpTimeInstance, snmp_sysContact, snmp_sysName, snmp_sysLocation, "
			"status, status_event_count, status_fail_date, status_rec_date, "
			"status_last_error, min_time, max_time, cur_time, avg_time, polling_time, "
			"total_polls, failed_polls, availability, last_updated "
			"FROM host "
			"WHERE poller_id = %d", set.poller_id);
	}

	snprintf(prefix, BUFSIZE, "INSERT INTO host (id, snmp_sysDescr, snmp_sysObjectID, "
		"snmp_sysUpTimeInstance, snmp_sysContact, snmp_sysName, snmp_sysLocation, "
		"status, status_event_count, status_fail_date, status_rec_date, "
		"status_last_error, min_time, max_time, cur_time, avg_time, polling_time, "
		"total_polls, failed_polls, availability, last_updated) VALUES ");

	if (set.dbonupdate == 0) {
		snprintf(suffix, BUFSIZE, " ON DUPLICATE KEY UPDATE "
			"snmp_sysDescr=VALUES(snmp_sysDescr), "
			"snmp_sysObjectID=VALUES(snmp_sysObjectID), "
			"snmp_sysUpTimeInstance=VALUES(snmp_sysUpTimeInstance), "
			"snmp_sysContact=VALUES(snmp_sysContact), "
			"snmp_sysName=VALUES(snmp_sysName), "
			"snmp_sysLocation=VALUES(snmp_sysLocation), "
			"status=VALUES(status), "
			"status_event_count=VALUES(status_event_count), "
			"status_fail_date=VALUES(status_fail_date), "
			"status_rec_date=VALUES(status_rec_date), "
			"status_last_error=VALUES(status_last_error), "
			"min_time=VALUES(min_time), "
			"max_time=VALUES(max_time), "
			"cur_time=VALUES(cur_time), "
			"avg_time=VALUES(avg_time), "
			"polling_time=VALUES(polling_time), "
			"total_polls=VALUES(total_polls), "
			"failed_polls=VALUES(failed_polls), "
			"availability=VALUES(availability), "
			"last_updated=VALUES(last_updated)");
	} else {
		snprintf(suffix, BUFSIZE, " AS rs ON DUPLICATE KEY UPDATE "
			"snmp_sysDescr=rs.snmp_sysDescr, "
			"snmp_sysObjectID=rs.snmp_sysObjectID, "
			"snmp_sysUpTimeInstance=rs.snmp_sysUpTimeInstance, "
			"snmp_sysContact=rs.snmp_sysContact, "
			"snmp_sysName=rs.snmp_sysName, "
			"snmp_sysLocation=rs.snmp_sysLocation, "
			"status=rs.status, "
			"status_event_count=rs.status_event_count, "
			"status_fail_date=rs.status_fail_date, "
			"status_rec_date=rs.status_rec_date, "
			"status_last_error=rs.status_last_error, "
			"min_time=rs.min_time, "
			"max_time=rs.max_time, "
			"cur_time=rs.cur_time, "
			"avg_time=rs.avg_time, "
			"polling_time=rs.polling_time, "
			"total_polls=rs.total_polls, "
			"failed_polls=rs.failed_polls, "
			"availability=rs.availability, "
			"last_updated=rs.last_updated");
	}

	if ((result = db_query(&mysql, LOCAL, query)) != 0) {
		num_rows = spine_count_to_int(mysql_num_rows(result));
		rows = 0;

		if (num_rows > 0) {
			while ((row = mysql_fetch_row(result))) {
				if (rows < 500) {
					if (rows == 0) {
						sqlp  = sqlbuf;
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", prefix);
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " (");
					} else {
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), ", (");
					}

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[0]); // id mediumint

					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[1]); // snmp_sysDescr varchar(300)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[2]); // snmp_sysObjectID varchar(128)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[3]); // snmp_sysUpTimeInstance bigint
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[4]); // snmp_sysContact varchar(300)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[5]); // snmp_sysName varchar(300)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[6]); // snmp_sysLocation varchar(300)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[7]); // status tinyint
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[8]); // status_event_count mediumint

					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[9]);  // status_fail_date timestamp
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[10]); // status_rec_date timestamp
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);
					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[11]); // status_last_error varchar(255)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[12]); // min_time decimal(10,5)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[13]); // max_time decimal(10,5)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[14]); // cur_time decimal(10,5)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[15]); // avg_time decimal(10,5)
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[16]); // polling_time double
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[17]); // total_polls int
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[18]); // failed_polls int
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[19]); // availability decimal(8,5)

					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[20]); // last_updated timestamp
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s'", tmpstr);

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), ")");

					rows++;
				} else {
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", suffix);
					db_insert(&mysqlr, REMOTE, sqlbuf);

					rows = 0;
				}
			}
		}

		if (rows > 0) {
			sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", suffix);
			db_insert(&mysqlr, REMOTE, sqlbuf);
		}
	}

	db_free_result(result);

	SPINE_LOG_MEDIUM(("Pushing Poller Item RRD Next Step to Main Server"));

	if (strlen(set.host_id_list)) {
		snprintf(query, MEGA_BUFSIZE, "SELECT SQL_NO_CACHE local_data_id, host_id, rrd_name, rrd_step, rrd_next_step "
			"FROM poller_item "
			"WHERE poller_id = %d "
			"AND host_id IN (%s)", set.poller_id, set.host_id_list);
	} else {
		snprintf(query, MEGA_BUFSIZE, "SELECT SQL_NO_CACHE local_data_id, host_id, rrd_name, rrd_step, rrd_next_step "
			"FROM poller_item "
			"WHERE poller_id = %d ",
			set.poller_id);
	}

	snprintf(prefix, BUFSIZE, "INSERT INTO poller_item (local_data_id, host_id, rrd_name, rrd_step, rrd_next_step) VALUES ");

	if (set.dbonupdate == 0) {
		snprintf(suffix, BUFSIZE, " ON DUPLICATE KEY UPDATE "
			"rrd_next_step=VALUES(rrd_next_step)");
	} else {
		snprintf(suffix, BUFSIZE, " AS rs ON DUPLICATE KEY UPDATE "
			"rrd_next_step=rs.rrd_next_step");
	}

	if ((result = db_query(&mysql, LOCAL, query)) != 0) {
		num_rows = spine_count_to_int(mysql_num_rows(result));
		rows = 0;

		if (num_rows > 0) {
			while ((row = mysql_fetch_row(result))) {
				if (rows < 10000) {
					if (rows == 0) {
						sqlp = sqlbuf;
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", prefix);
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), " (");
					} else {
						sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), ", (");
					}

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[0]); // local_data_id
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[1]); // host_id

					db_escape(&mysql, tmpstr, sizeof(tmpstr), row[2]); // rrd_name
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "'%s', ", tmpstr);

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s, ", row[3]); // rrd_step
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s",   row[4]); // rrd_next_step

					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), ")");

					rows++;
				} else {
					sqlp += spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", suffix);
					db_insert(&mysqlr, REMOTE, sqlbuf);

					rows = 0;
				}
			}
		}

		if (rows > 0) {
			spine_snprintf(sqlp, sizeof(sqlbuf) - (size_t)(sqlp - sqlbuf), "%s", suffix);
			db_insert(&mysqlr, REMOTE, sqlbuf);
		}
	}

	db_free_result(result);

	db_disconnect(&mysql);
	db_disconnect(&mysqlr);
}

/*! \fn int read_spine_config(const char *file)
 *  \brief obtain default startup variables from the spine.conf file.
 *  \param file the spine config file
 *
 *  \return 0 if successful or -1 if the file could not be opened
 */
static int apply_config_directive(const char *name, const char *value) {
	/* Immutable metadata cannot be changed by configuration values written
	 * into set. Offsets and capacities describe the actual destination fields. */
	static const struct {
		const char *name;
		size_t offset;
		size_t capacity;
	} strings[] = {
		{"RDB_Host", offsetof(config_t, rdb_host), sizeof(set.rdb_host)},
		{"RDB_Database", offsetof(config_t, rdb_db), sizeof(set.rdb_db)},
		{"RDB_User", offsetof(config_t, rdb_user), sizeof(set.rdb_user)},
		{"RDB_Pass", offsetof(config_t, rdb_pass), sizeof(set.rdb_pass)},
		{"RDB_SSL_Key", offsetof(config_t, rdb_ssl_key), sizeof(set.rdb_ssl_key)},
		{"RDB_SSL_Cert", offsetof(config_t, rdb_ssl_cert), sizeof(set.rdb_ssl_cert)},
		{"RDB_SSL_CA", offsetof(config_t, rdb_ssl_ca), sizeof(set.rdb_ssl_ca)},
		{"DB_Host", offsetof(config_t, db_host), sizeof(set.db_host)},
		{"DB_Database", offsetof(config_t, db_db), sizeof(set.db_db)},
		{"DB_User", offsetof(config_t, db_user), sizeof(set.db_user)},
		{"DB_Pass", offsetof(config_t, db_pass), sizeof(set.db_pass)},
		{"DB_SSL_Key", offsetof(config_t, db_ssl_key), sizeof(set.db_ssl_key)},
		{"DB_SSL_Cert", offsetof(config_t, db_ssl_cert), sizeof(set.db_ssl_cert)},
		{"DB_SSL_CA", offsetof(config_t, db_ssl_ca), sizeof(set.db_ssl_ca)},
		{"SNMP_Clientaddr", offsetof(config_t, snmp_clientaddr), sizeof(set.snmp_clientaddr)},
	};
	for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
		if (STRIMATCH(name, strings[i].name)) {
			strncopy((char *)&set + strings[i].offset, value, strings[i].capacity);
			return TRUE;
		}
	}
	if (STRIMATCH(name, "RDB_Port")) set.rdb_port = atoi(value);
	else if (STRIMATCH(name, "RDB_UseSSL")) set.rdb_ssl = atoi(value);
	else if (STRIMATCH(name, "DB_Port")) set.db_port = atoi(value);
	else if (STRIMATCH(name, "DB_UseSSL")) set.db_ssl = atoi(value);
	else if (STRIMATCH(name, "Poller")) set.poller_id = atoi(value);
	else if (STRIMATCH(name, "DB_PreG")) {
		if (!set.stderr_notty) fprintf(stderr, "WARNING: DB_PreG is no longer supported\n");
	} else if (STRIMATCH(name, "Cacti_Log")) {
		STRNCOPY(set.path_logfile, value);
		set.logfile_processed = 1;
		set.log_destination = LOGDEST_BOTH;
	} else {
		return FALSE;
	}
	return TRUE;
}

int read_spine_config(const char *file) {
	FILE *fp = fopen(file, "rb");
	char buff[BUFSIZE];
	char name[BUFSIZE];
	char value[BUFSIZE];
	char display_file[BUFSIZE];
	strncopy(display_file, file, sizeof(display_file));
	spine_sanitize_log_message(display_file);

	if (fp == NULL) {
		if (set.log_level == POLLER_VERBOSITY_DEBUG && !set.stderr_notty) {
			fprintf(stderr, "ERROR: Could not open config file [%s]\n", display_file);
		}
		return -1;
	}
	if (!set.stdout_notty) {
		fprintf(stdout, "SPINE: Using spine config file [%s]\n", display_file);
	}
	while (fgets(buff, sizeof(buff), fp) != NULL) {
		if (buff[0] == '#' || buff[0] == ' ' || buff[0] == '\n') continue;
		if (sscanf(buff, "%15s %255s", name, value) != 2) continue;
		if (!apply_config_directive(name, value) && !set.stderr_notty) {
			spine_sanitize_log_message(name);
			fprintf(stderr, "WARNING: Unrecognized directive: %s in %s\n", name, display_file);
		}
	}
	int failed = ferror(fp);
	if (fclose(fp) != 0) failed = TRUE;
	return failed ? -1 : 0;
}

/*! \fn void config_defaults(void)
 *  \brief populates the global configuration structure with default spine.conf file settings
 *  \param *set global runtime parameters
 *
 */
void config_defaults() {
	set.threads = DEFAULT_THREADS;

	/* default server */
	set.db_port  = DEFAULT_DB_PORT;

	STRNCOPY(set.db_host, DEFAULT_DB_HOST);
	STRNCOPY(set.db_db,   DEFAULT_DB_DB  );
	STRNCOPY(set.db_user, DEFAULT_DB_USER);
	STRNCOPY(set.db_pass, DEFAULT_DB_PASS);

	/* remote default server */
	set.rdb_port  = DEFAULT_DB_PORT;

	STRNCOPY(set.rdb_host, DEFAULT_DB_HOST);
	STRNCOPY(set.rdb_db,   DEFAULT_DB_DB  );
	STRNCOPY(set.rdb_user, DEFAULT_DB_USER);
	STRNCOPY(set.rdb_pass, DEFAULT_DB_PASS);

	STRNCOPY(config_paths[0], CONFIG_PATH_1);
	STRNCOPY(config_paths[1], CONFIG_PATH_2);
	STRNCOPY(config_paths[2], CONFIG_PATH_3);
	STRNCOPY(config_paths[3], CONFIG_PATH_4);

	set.log_destination = LOGDEST_FILE;
}

/*! \fn void die(const char *format, ...)
 *  \brief a method to end Spine while returning the fatal error to stderr
 *
 *	Given a printf-style argument list, format it to the standard
 *	error, append a newline, then exit Spine.
 *
 */
void die(const char *format, ...) {
	va_list	args;
	char logmessage[BUFSIZE];
	char flogmessage[DBL_BUFSIZE];
	int old_errno = errno;

	va_start(args, format);
	vsnprintf(logmessage, sizeof(logmessage), format, args);
	va_end(args);

	if (set.log_perror) {
		char perr[BUFSIZE];
		snprintf(perr, BUFSIZE, " [%d, %s]", old_errno, strerror(old_errno));
		size_t used = strlen(logmessage);
		snprintf(logmessage + used, sizeof(logmessage) - used, "%s", perr);
	}
	spine_sanitize_log_message(logmessage);

	if (set.logfile_processed) {
		if (set.parent_fork == SPINE_PARENT) {
			snprintf(flogmessage, DBL_BUFSIZE, "%s (Spine parent)", logmessage);
		} else {
			snprintf(flogmessage, DBL_BUFSIZE, "%s (Spine thread)", logmessage);
		}
	} else {
		snprintf(flogmessage, DBL_BUFSIZE, "%s (Spine init)", logmessage);
	}

	fprintf(stderr, "%s", flogmessage);

	if ((set.parent_fork == SPINE_PARENT) && (set.php_initialized)) {
		php_close(PHP_INIT);
	}

	exit(set.exit_code);
}

char *get_date_format(void) {
	static const char separators[] = {'-', '/', '.'};
	static const char *const formats[][3] = {
		{"%m", "%d", "%Y"}, {"%b", "%d", "%Y"},
		{"%d", "%m", "%Y"}, {"%d", "%b", "%Y"},
		{"%Y", "%m", "%d"}, {"%Y", "%b", "%d"}
	};
	char *log_fmt = malloc(GD_FMT_SIZE);
	if (log_fmt == NULL) die("ERROR: Fatal malloc error: util.c get_date_format!");
	if (set.log_datetime_separator < GDC_MIN || set.log_datetime_separator > GDC_MAX) {
		set.log_datetime_separator = GDC_DEFAULT;
	}
	if (set.log_datetime_format < GD_MIN || set.log_datetime_format > GD_MAX) {
		set.log_datetime_format = GD_DEFAULT;
	}
	char separator = separators[set.log_datetime_separator];
	const char *const *parts = formats[set.log_datetime_format];
	spine_snprintf(log_fmt, GD_FMT_SIZE, "%s%c%s%c%s %%H:%%M:%%S - ",
		parts[0], separator, parts[1], separator, parts[2]);
	return log_fmt;
}

/*! \fn void spine_log(const char *format, ...)
 *  \brief output's log information to the desired cacti logfile.
 *  \param *logmessage a pointer to the pre-formatted log message.
 *
 */
bool spine_should_log_device(int host_id, int verbosity) {
	return is_debug_device(host_id) || set.log_level >= verbosity;
}

int spine_log(const char *format, ...) {
	va_list	args;

	FILE *log_file = NULL;
	FILE *fp = NULL;

	/* variables for time display */
	time_t nowbin;
	struct tm now_time;
	const struct tm *now_ptr;

	/* keep track of an errored log file */
	static int log_error = FALSE;

	int  of = 20;
	char logprefix[LOGSIZE];        /* Formatted Log Prefix */
	char ulogmessage[LOGSIZE];      /* Un-Formatted Log Message */
	char flogmessage[LOGSIZE];      /* Formatted Log Message */
	char stdoutmessage[LOGSIZE+of]; /* Message for stdout */

	double cur_time;

	va_start(args, format);
	vsnprintf(ulogmessage, LOGSIZE - 1, format, args);
	va_end(args);
	spine_sanitize_log_message(ulogmessage);

	/* default for "console" messages to go to stdout */
	fp = stdout;

	/* log message prefix */

	snprintf(logprefix, LOGSIZE, "SPINE: Poller[%i] PID[%i] PT[%ld] ", set.poller_id, getpid(), (unsigned long int)pthread_self());

	/* get time for poller_output table */
	nowbin = time(&nowbin);

	localtime_r(&nowbin,&now_time);
	now_ptr = &now_time;

	if (IS_LOGGING_TO_STDOUT()) {
		cur_time = get_time_as_double();
		snprintf(stdoutmessage, LOGSIZE + of, "Total[%3.4f] %s", cur_time - start_time, ulogmessage);
		puts(stdoutmessage);
		return TRUE;
	}

	char * log_fmt = get_date_format();

	if (strlen(log_fmt) == 0) {
		#ifdef DISABLE_STDERR
		fp = stdout;
		#else
		fp = stderr;
		#endif

		if ((set.stderr_notty) && (fp == stderr)) {
			/* do nothing stderr does not exist */
		} else if ((set.stdout_notty) && (fp == stdout)) {
			/* do nothing stdout does not exist */
		} else {
			fprintf(fp, "ERROR: Could not get format from get_date_format()\n");
		}
	}

	int prefix_len = spine_count_to_int(strlen(logprefix));
	int ulog_len   = spine_count_to_int(strlen(ulogmessage));
	int flog_len   = 0;

	if ((flog_len = strftime(flogmessage, 50, log_fmt, now_ptr)) == 0) {
		flogmessage[0] = '\0';
		#ifdef DISABLE_STDERR
		fp = stdout;
		#else
		fp = stderr;
		#endif

		if ((set.stderr_notty) && (fp == stderr)) {
			/* do nothing stderr does not exist */
		} else if ((set.stdout_notty) && (fp == stdout)) {
			/* do nothing stdout does not exist */
		} else {
			fprintf(fp, "ERROR: Could not get string from strftime()\n");
		}
	}

	/* determine how many characters to append */
	if (prefix_len > LOGSIZE - flog_len - 2) {
		prefix_len = LOGSIZE - flog_len - 2;
	}

	if (ulog_len + prefix_len > LOGSIZE - flog_len - 2) {
		ulog_len = LOGSIZE - flog_len - prefix_len - 2;
	}

	snprintf(flogmessage + flog_len, sizeof(flogmessage) - (size_t) flog_len,
		"%.*s%.*s", prefix_len, logprefix, ulog_len, ulogmessage);

	/* output to syslog/eventlog */
	if (IS_LOGGING_TO_SYSLOG()) {
		openlog("Cacti", LOG_NDELAY | LOG_PID, LOG_SYSLOG);

		if ((strstr(flogmessage,"ERROR") || (strstr(flogmessage, "FATAL"))) && (set.log_perror)) {
			syslog(LOG_CRIT,"%s\n", flogmessage);
		}

		if ((strstr(flogmessage,"WARNING")) && (set.log_pwarn)){
			syslog(LOG_WARNING,"%s\n", flogmessage);
		}

		if ((strstr(flogmessage,"STATS")) && (set.log_pstats)){
			syslog(LOG_NOTICE,"%s\n", flogmessage);
		}

		closelog();
	}

	/* append a line feed to the log message if needed */
	if (!strstr(flogmessage, "\n")) {
		size_t used = strlen(flogmessage);
		snprintf(flogmessage + used, sizeof(flogmessage) - used, "\n");
	}

	if ((IS_LOGGING_TO_FILE() &&
		(set.log_level != POLLER_VERBOSITY_NONE) &&
		(strlen(set.path_logfile) != 0)) && (set.logfile_processed)) {
		if (!file_exists(set.path_logfile)) {
			log_file = fopen(set.path_logfile, "w");
		} else {
			log_file = fopen(set.path_logfile, "a");
		}

		if (log_file) {
			fputs(flogmessage, log_file);
			fclose(log_file);
		} else {
			if (!log_error) {
				printf("ERROR: Spine Log File Could Not Be Opened/Created\n");
				log_error = TRUE;
			}
		}
	}

	if (set.log_level >= POLLER_VERBOSITY_NONE) {
		if ((strstr(flogmessage,"ERROR"))   ||
			(strstr(flogmessage,"WARNING")) ||
			(strstr(flogmessage,"FATAL"))) {
			#ifdef DISABLE_STDERR
			fp = stdout;
			#else
			fp = stderr;
			#endif
		}

		if ((set.stderr_notty) && (fp == stderr)) {
			/* do nothing stderr does not exist */
		} else if ((set.stdout_notty) && (fp == stdout)) {
			/* do nothing stdout does not exist */
		} else {
			fprintf(fp, "%s", flogmessage);
		}
	}

	free(log_fmt);

	return TRUE;
}

/*! \fn int file_exists(const char *filename)
 *  \brief checks for the existence of a file.
 *  \param *filename the name of the file to check for.
 *
 *  \return TRUE if found FALSE if not.
 *
 */
int file_exists(const char *filename) {
	struct stat file_stat;

	if (stat(filename, &file_stat)) {
		return FALSE;
	} else {
		return TRUE;
	}
}

/*! \fn all_digits(const char *string)
 *  \brief verifies that a string is contains only numeric characters
 *  \param string the string to check
 *
 *  This function has no leeway: spaces and minus signs and decimal points
 *  are not digits, and an empty string is (by convention) not
 *  all-digits too.
 *
 *  \return TRUE if not alpha or special characters found, FALSE if non numeric found
 *
 */
int all_digits(const char *string) {
	/* empty string is not all digits */
	if ( *string == '\0' ) return FALSE;

	while ( isdigit((int)*string) )
		string++;

	return *string == '\0';
}

/*! \fn is_ipaddress(const char *string)
 *  \brief verifies that a string is an ip address either v4 or v6
 *  \param string the string to check
 *
 *  This function simply checks to see if a string object is an ip address.
 *  If it is, it returns true else false.
 *
 *  \return TRUE if an ip address, or FALSE if non
 *
 */
int is_ipaddress(const char *string) {
	while (*string) {
		if ((isdigit((int)*string)) ||
			(*string == '.') ||
			(*string == ':')) {
			string++;

			continue;
		}

		return FALSE;
	}

	return TRUE;
}

/*! \fn int is_numeric(const char *string)
 *  \brief check to see if a string is long or double
 *  \param string the string to check
 *
 *  \return TRUE if long or double, FALSE if not
 *
 */
int is_numeric(char *string) {
	char *end_ptr_long;
	char *end_ptr_double;
	int conv_base=10;
	size_t length;

	length = strlen(trim(string));

	if (!length) {
		return FALSE;
	}

 	/* check for an integer */
	errno = 0;
	strtol(string, &end_ptr_long, conv_base);

	if (errno != ERANGE) {
		if (end_ptr_long == string + length) { /* integer string */
			return TRUE;
		} else if ((end_ptr_long == string) && (*end_ptr_long != '\0' &&
				*end_ptr_long != '.' &&
				*end_ptr_long != '-' &&
				*end_ptr_long != '+')) { /* ignore partial string matches but doubles can begin with '+', '-', '.' */
			return FALSE;
		}
	} else {
		end_ptr_long = NULL;
	}

 	/* check for a float */
	errno = 0;
	strtod(string, &end_ptr_double);
	if (errno != ERANGE) {
		if (end_ptr_double == string + length) { /* floating point string */
			return TRUE;
		}
	} else {
		end_ptr_double = NULL;
	}

	return FALSE;
}

/*! \fn int is_hexadecimal(const char *str, const short ignore_space)
 *  \brief test whether a string represents a hex number.
 *  \param str string to test
 *  \param ignore_space nonzero to skip tabs and spaces
 *
 *  \return TRUE if the string is valid hex, FALSE otherwise
 *
 *  The function is modified where the string needs to include
 *  at least one of the following string ' ', '-', or ':'
 *
 */
int is_hexadecimal(const char * str, const short ignore_special) {
	int i = 0;
	int delim_found = FALSE;

	if (!str) return FALSE;

	while (*str) {
		switch (*str) {
			case '0': case '1': case '2': case '3':
			case '4': case '5': case '6': case '7':
			case '8': case '9':
			case 'a': case 'A': case 'b': case 'B':
			case 'c': case 'C': case 'd': case 'D':
			case 'e': case 'E': case 'f': case 'F':
			case '"':
				break;
			case '-': case ':': case ' ':
				delim_found = TRUE;
				break;
			case '\t':
				if (ignore_special) {
					break;
				}
			default:
				return FALSE;
		}

		str++;
		i++;
	}

	if ((i < 3) || delim_found == FALSE) {
		return FALSE;
	}

	return TRUE;
}

/*! \fn char *strip_alpha(char *string)
 *  \brief remove trailing alpha characters from a string.
 *  \param string the string to strip characters from
 *
 *  \return a pointer to the modified string
 *
 */
char *strip_alpha(char *string) {
	size_t end = strlen(string);
	while (end > 0 && !isdigit((unsigned char)string[end - 1])) string[--end] = '\0';
	size_t start = 0;
	while (start < end && !isdigit((unsigned char)string[start]) && string[start] != '-') start++;
	return string + start;
}

/*! \fn char *add_slashes(const char *string)
 *  \brief add escaping to back slashes on for Windows type commands.
 *  \param string the string to replace slashes
 *
 *  \return a pointer to the modified string. Variable must be freed by parent.
 *
 */
char *add_slashes(const char *string) {
	size_t length = strlen(string);
	if (length > (SIZE_MAX - 1) / 2) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Escaped command exceeds addressable memory");
	}
	char *result = malloc(length * 2 + 1);
	if (result == NULL) die("ERROR: Fatal malloc error: util.c add_slashes!");
	size_t used = 0;
	for (size_t i = 0; i < length; i++) {
		if (string[i] == '\\') result[used++] = '\\';
		result[used++] = string[i];
	}
	result[used] = '\0';
	return result;
}

/*! \fn char *strncopy(char *dst, const char *src, size_t obuf)
 *  \brief copies source to destination add a NUL terminator
 *
 *	Copy from source to destination, insuring a NUL termination.
 *	The size of the buffer *includes* the terminating NUL. Note
 *	that strncpy() does NOT NUL terminate if the source is the
 *	size of the destination (yuck).
 *
 *	NOTE: it's very common to call this as:
 *
 *	  strncopy(buf, src, sizeof buf)
 *
 *	so we provide an STRNCOPY() macro which adds the size.
 *
 *  \return pointer to destination string
 *
*/
char *strncopy(char *dst, const char *src, size_t obuf) {
	assert(dst != 0);
	assert(src != 0);

	size_t len;

	if (obuf == 0) return dst;
	len = strlen(src);
	if (len >= obuf) len = obuf - 1;
	if (len) {
		memcpy(dst, src, len);
	}

	dst[len] = '\0';
	return dst;
}

/*! \fn double get_time_as_double()
 *  \brief fetches system time as a double-precision value
 *
 *  \return system time (at microsecond resolution) as a double
 */
double get_time_as_double(void) {
	struct timeval now;

	gettimeofday(&now, NULL);

	return (double)now.tv_sec + (double)now.tv_usec / 1000000;
}

/*! \fn trim()
 *  \brief removes leading and trailing blanks, tabs, line feeds and
 *         carriage returns from a string.
 *
 *  \return the trimmed string.
 */
char *trim(char *str) {
	return ltrim(rtrim(str));
}

/*! \fn rtrim()
 *  \brief removes trailing blanks, tabs, line feeds, carriage returns
 *         single and double quotes and back-slashed from a string.
 *
 *  \return the trimmed string.
 */
char *rtrim(char *str) {
	char    *end;
	const char *trim = " \"\'\\\t\n\r";

	if (!str) return NULL;

	end = str + strlen(str);

	while (end-- > str) {
		if (!strchr(trim, *end)) return str;

		*end = 0;
	}

	return str;
}

/*! \fn ltrim()
 *  \brief removes leading blanks, tabs, line feeds, carriage returns
 *         single and double quotes and back-slashed from a string.
 *
 *  \return the trimmed string.
 */
char *ltrim(char *str) {
	const char *trim = " \"\'\\\t\n\r";

	if (!str) return NULL;

	while (*str) {
		if (!strchr(trim, *str)) return str;

		++str;
	}

	return str;
}

/*! \fn reverse()
 *  \brief reverses a string in place.
 *
 *  \return the reversed string.
 */
char *reverse(char* str) {
	size_t start = 0;
	size_t end = strlen(str);
	while (start < end) {
		end--;
		if (start >= end) break;
		char byte = str[start];
		str[start++] = str[end];
		str[end] = byte;
	}
	return str;
}

/*! \fn strpos()
 *  \brief looks for the position of needle in haystack
 *
 *  \return the position of -1 if not found
 */
int strpos(const char *haystack, const char *needle) {
	const char *p = strstr(haystack, needle);

	if (p) {
		return p - haystack <= INT_MAX ? (int)(p - haystack) : -1;
	}

	return -1;
}

/*! \fn char_count()
 *  \brief counts occurrences of char in string.
 *
 *  \return number of occurrences.
 */
int char_count(const char *str, int chr) {
	const unsigned char *my_str = (const unsigned char *) str;
	const unsigned char my_chr = (unsigned char)chr;
	int count = 0;

	if (!my_chr) return 1;

	while (*my_str) {
		if (*my_str++ == my_chr) {
			count++;
		}
	}
	return count;
}

unsigned long long hex2dec(char *str) {
	unsigned long long number = 0;
	if (str == NULL) return 0;
	/* Retain the historical in-place reversal, but avoid floating-point
	 * rounding and undefined conversion when a sample exceeds 64 bits. */
	reverse(str);
	for (size_t i = strlen(str); i > 0; i--) {
		unsigned char c = (unsigned char)str[i - 1];
		unsigned int digit;
		if (c == '"' || c == ' ' || c == '\t') continue;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else return 0;
		if (number > (ULLONG_MAX - digit) / 16) return 0;
		number = number * 16 + digit;
	}
	return number;
}

int hasCaps() {
	#ifdef HAVE_LCAP
	cap_t caps;
	cap_value_t capval;
	cap_flag_value_t capflag;

	/* Recommended caps: cap_net_raw=eip */
	caps = cap_get_proc();
	if (caps == NULL) {
		SPINE_LOG(("ERROR: cap_get_proc failed."));
		return FALSE;
	}

    /* check if cap_net_raw is in effective set */
	if (cap_get_flag(caps, CAP_NET_RAW, CAP_EFFECTIVE, &capflag)) {
		SPINE_LOG(("ERROR: cap_get_flag for CAP_NET_RAW failed. ICMP ping will not work as non-root user."));
		return FALSE;
	}

	if (capflag != CAP_SET) {
		SPINE_LOG(("ERROR: Capability CAP_NET_RAW is not set. ICMP ping will not work as non-root user."));
		return FALSE;
	}

	SPINE_LOG_DEBUG(("DEBUG: Capability CAP_NET_RAW is set."));
	cap_free(caps);

	return TRUE;
	#else
	return FALSE;
	#endif
}

void checkAsRoot() {
	#ifndef __CYGWIN__
	#ifdef SOLAR_PRIV
	priv_set_t *privset;
	char *p;

	/* Get the basic set */
	privset = priv_str_to_set("basic", ",", NULL);
	if (privset == NULL) {
		die("ERROR: Could not get basic privset from priv_str_to_set().");
	} else {
		p = priv_set_to_str(privset, ',', 0);
		SPINE_LOG_DEBUG(("DEBUG: Basic privset is: '%s'.", p != NULL ? p : "Unknown"));
	}

	/* Add privilege to send/receive ICMP packets */
	if (priv_addset(privset, PRIV_NET_ICMPACCESS) < 0) {
		SPINE_LOG_DEBUG(("WARNING: Addition of PRIV_NET_ICMPACCESS to privset failed: '%s'.", strerror(errno)));
	}

	/* Compute the set of privileges that are never needed */
	priv_inverse(privset);

	/* Remove the set of unneeded privs from Permitted (and by
	 * implication from Effective) */
	if (setppriv(PRIV_OFF, PRIV_PERMITTED, privset) < 0) {
		SPINE_LOG_DEBUG(("WARNING: Dropping privileges from PRIV_PERMITTED failed: '%s'.", strerror(errno)));
	}

	/* Remove unneeded priv set from Limit to be safe */
	if (setppriv(PRIV_OFF, PRIV_LIMIT, privset) < 0) {
		SPINE_LOG_DEBUG(("WARNING: Dropping privileges from PRIV_LIMIT failed: '%s'.", strerror(errno)));
	}

	boolean_t pe = priv_ineffect(PRIV_NET_ICMPACCESS);
	SPINE_LOG_DEBUG(("DEBUG: Privilege PRIV_NET_ICMPACCESS is: '%s'.", pe != 0 ? "Enabled" : "Disabled"));

	set.icmp_avail = pe;

	/* Free the privset */
	priv_freeset(privset);
	free(p);
	#else
	if (hasCaps() != TRUE) {
		SPINE_LOG_DEBUG(("DEBUG: Spine running as %d UID, %d EUID", getuid(), geteuid()));
		int ret = seteuid(0);
		if (ret != 0) {
			SPINE_LOG_DEBUG(("WARNING: Spine NOT able to set effective UID to 0"));
		}

		if (geteuid() != 0) {
			SPINE_LOG_DEBUG(("WARNING: Spine NOT running as root.  This is required if using ICMP.  Please run \"chown root:root spine;chmod u+s spine\" to resolve."));
			set.icmp_avail = FALSE;
		} else {
			SPINE_LOG_DEBUG(("DEBUG: Spine is running as root."));
			set.icmp_avail = TRUE;

			if (seteuid(getuid()) == -1) {
				SPINE_LOG_DEBUG(("WARNING: Spine unable to drop from root to local user."));
			}
		}
	} else {
		SPINE_LOG_DEBUG(("DEBUG: Spine has cap_net_raw capability."));
		set.icmp_avail = TRUE;
	}
	SPINE_LOG_DEBUG(("DEBUG: Spine has %sgot ICMP", set.icmp_avail?"":"not "));
	#endif
	#endif
}

/*! \fn int get_cacti_version(MYSQL *psql, int mode, const char *setting)
 *  \brief Returns the version of Cacti as a decimal
 *
 *  Given a pointer to a database get the version of Cacti and convert
 *  to an integer.
 *
 *  \return the cacti version
 *
 */
int get_cacti_version(MYSQL *psql, int mode) {
	char      qstring[BUFSIZE];
	char      *retval;
	MYSQL_RES *result;
	MYSQL_ROW mysql_row;
	int major;
	int minor;
	int point;
	int       cacti_version;

	assert(psql != 0);

	spine_snprintf(qstring, sizeof(qstring), "SELECT cacti FROM version LIMIT 1");

	result = db_query(psql, mode, qstring);

	if (result != 0) {
		if (mysql_num_rows(result) > 0) {
			mysql_row = mysql_fetch_row(result);

			if (mysql_row != NULL) {
				retval = strdup(mysql_row[0]);
				db_free_result(result);

				if (STRIMATCH(retval, "new_install")) {
					SPINE_FREE(retval);

					return 0;
				} else {
					sscanf(retval, "%d.%d.%d", &major, &minor, &point);
					cacti_version = (major * 1000) + (minor * 100) + (point * 1);

					SPINE_FREE(retval);

					return cacti_version;
				}
			}else{
				return 0;
			}
		}else{
			db_free_result(result);
			return 0;
		}
	}else{
		return 0;
	}
}

/* pthread storage keeps the borrowed match valid without requiring C11 TLS.
 * Each polling thread owns its buffer, which is freed when the thread exits. */
static pthread_key_t regex_result_key;
static pthread_once_t regex_result_once = PTHREAD_ONCE_INIT;

static void initialize_regex_result_key(void) {
	if (pthread_key_create(&regex_result_key, free) != 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Unable to create thread-local regex storage");
	}
}

static char *regex_result_buffer(void) {
	if (pthread_once(&regex_result_once, initialize_regex_result_key) != 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Unable to initialize thread-local regex storage");
	}
	char *buffer = pthread_getspecific(regex_result_key);
	if (buffer == NULL) {
		buffer = malloc(RESULTS_BUFFER);
		if (buffer == NULL) die("ERROR: Fatal malloc error: regex result buffer!");
		if (pthread_setspecific(regex_result_key, buffer) != 0) {
			free(buffer);
			set.exit_code = EXIT_FAILURE;
			die("ERROR: Unable to retain thread-local regex storage");
		}
	}
	return buffer;
}

char *regex_replace(const char *exp, char *value) {
	regex_t regex;
	int reti;
	char *msgbuf = NULL;

	/* Compile regular expression */
	reti = regcomp(&regex, exp, 0);
	if (reti) {
		// regex failed, just return what we have
		return value;
	}

	/* Execute regular expression */
	regmatch_t matches[MAX_MATCHES];
	reti = regexec(&regex, value, MAX_MATCHES, matches, 0);
	if (!reti) {
		// regex matched
		size_t length = (size_t) (matches[0].rm_eo - matches[0].rm_so);
		if (length >= RESULTS_BUFFER) {
			regfree(&regex);
			return value;
		}
		msgbuf = regex_result_buffer();
		memcpy(msgbuf, value + matches[0].rm_so, length);
		msgbuf[length] = '\0';
	}

	/* Free memory allocated to the pattern buffer by regcomp() */
	regfree(&regex);

	return reti ? value : msgbuf;
}

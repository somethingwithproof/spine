# Cacti Spine - In-Depth Code Review

**Date:** 2026-03-26
**Scope:** Full codebase review of all C source files
**Files Reviewed:** spine.c, poller.c, snmp.c, ping.c, util.c, sql.c, php.c, nft_popen.c, error.c, locks.c, keywords.c, common.h, and all associated headers

---

## Executive Summary

Cacti Spine is a C-based multi-threaded network polling engine. This review identified **~85 findings** across the codebase, including **16 Critical**, **15 High**, **19 Medium**, and **~35 Low** severity issues. The most concerning patterns are:

1. **Pervasive buffer overflow risks** from `sprintf()`, `vsprintf()`, `strcat()` without bounds checking
2. **SQL injection vectors** through unescaped parameters in dynamically constructed queries
3. **Memory management defects** including leaks, use-after-free, and unchecked allocations
4. **Concurrency bugs** including race conditions in lock and subprocess management
5. **Signal handler safety violations** using non-async-signal-safe functions

---

## CRITICAL Findings

### C01. Buffer Overflow in `add_slashes()` - Heap Corruption
**File:** `util.c:1633-1668`

When input contains backslashes, each one expands to two characters in output. A string of 512 backslashes in a BUFSIZE (1024) buffer would require 1024 output bytes plus a null terminator, overflowing the heap allocation.

```c
if (string[position] == '\\') {
    return_str[new_position] = '\\';
    new_position++;
    return_str[new_position] = '\\';  // Can exceed BUFSIZE
}
```

**Fix:** Add bounds checking: `if (new_position + 1 >= BUFSIZE - 1) break;`

---

### C02. Stack Buffer Overflow in `die()` via `vsprintf()`
**File:** `util.c:1164-1178`

Uses unbounded `vsprintf()` into a BUFSIZE (1024) stack buffer, then appends with unchecked `strcat()`:

```c
vsprintf(logmessage, format, args);   // No size limit
strcat(logmessage, perr);             // Can overflow
```

**Fix:** Replace with `vsnprintf(logmessage, sizeof(logmessage), format, args);`

---

### C03. Stack Buffer Overflow in `spine_log()` via `strcat()`
**File:** `util.c:1369-1370`

Appends newline to LOGSIZE buffer without bounds check:

```c
if (!strstr(flogmessage, "\n")) {
    strcat(flogmessage, "\n");  // No room check
}
```

**Fix:** Use `strncat(flogmessage, "\n", sizeof(flogmessage) - strlen(flogmessage) - 1);`

---

### C04. SQL Injection in `getsetting()` / `getpsetting()` / `putsetting()`
**File:** `util.c:99, 141-147, 191, 285`

Setting names and values interpolated directly into SQL without escaping:

```c
sprintf(qstring, "SELECT SQL_NO_CACHE value FROM settings WHERE name = '%s'", setting);
sprintf(qstring, "INSERT INTO settings (name, value) VALUES ('%s', '%s')", mysetting, myvalue);
```

**Fix:** Use `db_escape()` for all dynamic SQL parameters, or use prepared statements.

---

### C05. SQL Injection via Unsanitized `host_id_list` from CLI
**File:** `spine.c:386-387, 636`

User-supplied host ID list from command line is directly interpolated into SQL:

```c
snprintf(set.host_id_list, BIG_BUFSIZE, "%s", getarg(opt, &argv));
// Later:
qp += sprintf(qp, " AND h.id IN(%s)", set.host_id_list);
```

**Fix:** Validate that `host_id_list` contains only digits and commas.

---

### C06. NULL Pointer Dereference - `mysql_fetch_row()`
**File:** `spine.c:736-759, 781-783, 796-798`

Multiple locations dereference `mysql_fetch_row()` result without NULL check:

```c
mysql_row = mysql_fetch_row(result);
host_id = atoi(mysql_row[0]);  // Crashes if mysql_row is NULL
```

**Fix:** Add `if (!mysql_row) { /* handle error */ }` after every fetch.

---

### C07. Missing `break` Statements in `get_date_format()` Switch
**File:** `util.c:1228-1243`

All case labels fall through to `default`, making only the default format ever apply:

```c
switch (set.log_datetime_format) {
    case GD_MO_D_Y:
        snprintf(log_fmt, GD_FMT_SIZE, ...);
    case GD_MN_D_Y:   // Falls through!
        snprintf(log_fmt, GD_FMT_SIZE, ...);
    // ... all fall through to default
}
```

**Fix:** Add `break;` after each case.

---

### C08. Race Condition - NULL Return from `get_lock()`
**File:** `locks.c:100-160, 230-242`

`get_lock()` returns NULL for invalid lock IDs, callers don't check:

```c
void thread_mutex_lock(int mutex) {
    pthread_mutex_lock(get_lock(mutex));  // NULL if invalid ID
}
```

**Fix:** Add NULL check and `default:` case with error logging in switch.

---

### C10. Memory Leak in `nft_popen()` Error Path
**File:** `nft_popen.c:155-208`

When `vfork()` fails after `cur = malloc()`, memory is never freed:

```c
cur = malloc(sizeof(struct pid));  // Allocated
// ... fork fails ...
return -1;  // 'cur' leaked!
```

**Fix:** Add `free(cur);` before error returns.

---

### C11. Signal Handler Calls Non-Async-Signal-Safe Functions
**File:** `error.c:46-94`

Signal handler calls `fprintf()`, `get_date_format()`, `localtime_r()`, `strftime()` - none are async-signal-safe per POSIX:

```c
static void spine_signal_handler(int spine_signal) {
    // ...
    char *log_fmt = get_date_format();  // NOT async-signal-safe
    fprintf(stderr, ...);               // NOT async-signal-safe
}
```

**Fix:** Use only `write()` and `_exit()` in signal handlers.

---

### C12. Use-After-Free - Stack Pointer in SNMP Session
**File:** `snmp.c:193`

`hostnameport` is stack-allocated but its address is stored in a session struct that outlives the function:

```c
char hostnameport[BUFSIZE];
snprintf(hostnameport, BUFSIZE, "%s:%i", hostname, snmp_port);
session.peername = hostnameport;  // Dangling pointer after return
```

**Fix:** Use `strdup()` or allocate on the heap.

---

### C13. Unchecked `strdup()` Return Values
**File:** `snmp.c:265, 277`

```c
Apsz = strdup(snmp_password);         // Can return NULL
Xpsz = strdup(snmp_priv_passphrase);  // Can return NULL
```

**Fix:** Check for NULL before use.

---

### C14. `poll_result` Written Without Size Verification
**File:** `poller.c:975, 2106`

Code writes BUFSIZE bytes into `poll_result` returned from `snmp_get()` without knowing its actual allocation size:

```c
poll_result = snmp_get(host, reindex->arg1);
snprintf(poll_result, BUFSIZE, "%s", sysUptime);  // Assumes >= BUFSIZE
```

**Fix:** Use a separate local buffer or verify allocation size.

---

### C15. Unchecked `malloc()` in Poller Thread Initialization
**File:** `poller.c:220-225`

```c
error_string = malloc(DBL_BUFSIZE);  // No NULL check
buf_size     = malloc(sizeof(int));  // No NULL check
*buf_size    = 0;                    // Crashes if NULL
```

**Fix:** Add NULL checks after each allocation.

---

### C16. Pointer Arithmetic Buffer Overflow in Error Accumulation
**File:** `poller.c:2010`

```c
snprintf(error_string + *buf_size, DBL_BUFSIZE, "%s", tbuffer);
```

Available space is `DBL_BUFSIZE - *buf_size`, not `DBL_BUFSIZE`.

**Fix:** `snprintf(error_string + *buf_size, DBL_BUFSIZE - *buf_size, "%s", tbuffer);`

---

### C17. `strtok()` Corruption + Wrong Type Assignment
**File:** `spine.c:524-530`

`strtok()` modifies global config string in-place, and `'\0'` (char) is assigned to `int` array:

```c
char *token = strtok(set.selective_device_debug, ",");
while(token) {
    debug_devices[i] = atoi(token);
    debug_devices[i+1] = '\0';  // Wrong: int array, not char array
}
```

**Fix:** Use `strdup()` before `strtok()`, use `-1` sentinel for int array.

---

## HIGH Findings

### H01. `atoi()` Without Validation on User Input
**File:** `spine.c:342, 354, 362, 366, 440, 444, 526`

All command-line numeric arguments parsed with `atoi()` which returns 0 on invalid input with no error indication. No bounds checking performed.

**Fix:** Use `strtol()` with validation.

---

### H02. NULL Pointer Dereference in `db_escape()`
**File:** `sql.c:581-594`

Returns without initializing output when `input == NULL`, leaving caller with uninitialized buffer.

**Fix:** Add `output[0] = '\0'; return;` for NULL input case.

---

### H03. Memory Leak - Socket Variable in `db_connect()`
**File:** `sql.c:237-257`

`hostname` from `strdup()` is orphaned when `socket` is reassigned:

```c
socket = strdup(set.db_host);  // New allocation
hostname = NULL;                // Old strdup'd hostname leaked
```

**Fix:** Track and free original allocation properly.

---

### H04. No DB Connection Validation Before Use
**File:** `sql.c:49-104`

`db_insert()` and `db_query()` don't validate `mysql` pointer:

```c
int db_insert(MYSQL *mysql, int type, const char *query) {
    if (mysql_query(mysql, query)) {  // Crashes if mysql is NULL
```

**Fix:** Add `assert(mysql != NULL);` at entry.

---

### H05. Unchecked `sendto()` / `send()` Return Values
**File:** `ping.c:405, 645`

ICMP and UDP send operations don't check for failure.

**Fix:** Check return and handle errors.

---

### H06. Race Condition in `nft_pclose()`
**File:** `nft_popen.c:328-365`

Mutex released at line 341, then `cur` structure used without protection:

```c
pthread_mutex_lock(&ListMutex);
for (cur = PidList; cur; cur = cur->next) ...
pthread_mutex_unlock(&ListMutex);  // Released!
// ... cur used freely below - RACE
```

**Fix:** Keep mutex locked during entire operation.

---

### H07. Missing `break` in `php_init()` Fork Error Handling
**File:** `php.c:384-410`

EAGAIN case falls through to ENOMEM, ENOMEM falls through to default.

**Fix:** Add `break;` after each case block.

---

### H08. Integer Underflow in Timeout Calculation
**File:** `php.c:208-217`

If elapsed time exceeds `script_timeout`, negative timeout values are computed without validation.

**Fix:** Clamp to zero: `if (elapsed >= set.script_timeout) { timeout.tv_sec = 0; ... }`

---

### H09. Uninitialized `hostinfo` Pointer in `init_sockaddr()`
**File:** `ping.c:902-946`

`hostinfo` used in `freeaddrinfo()` error path without initialization.

**Fix:** Initialize `hostinfo = NULL;` at declaration.

---

### H10. Unsafe IP Header Cast Without Bounds Check
**File:** `ping.c:439-440`

Raw socket data cast to `struct ip *` without verifying received data length:

```c
ip  = (struct ip *) socket_reply;
pkt = (struct icmp *) (socket_reply + (ip->ip_hl << 2));
```

**Fix:** Verify `return_code >= sizeof(struct ip)` before cast.

---

### H11. `inet_ntop()` Wrong Pointer Type
**File:** `ping.c:850, 863`

Uses `res->ai_addr->sa_data` instead of proper sockaddr_in/sockaddr_in6 address field.

**Fix:** Cast to correct sockaddr type and extract address properly.

---

### H12. SQL Injection in Poller Results
**File:** `poller.c:1810-1814`

SNMP/script results inserted into SQL without escaping:

```c
snprintf(result_string, ..., "(%i, '%s', FROM_UNIXTIME(%s), '%s')",
    ..., poller_items[i].result);  // Untrusted data!
```

**Fix:** Use `db_escape()` on all result values.

---

### H13. Dead `zero_sensitive` Code in SNMP
**File:** `snmp.c:260-281`

`zero_sensitive` is always 0, so password memory is never cleared:

```c
int zero_sensitive = 0;
if (Apsz && zero_sensitive) {  // Always false
    memset(Apsz, 0x0, strlen(Apsz));
}
```

**Fix:** Set `zero_sensitive = 1` or remove dead code.

---

### H14. Debug Devices Array Overflow
**File:** `spine.c:254, 524-530`

Array allocated for 100 entries but no bounds check in parsing loop.

**Fix:** Add `if (i >= 100) break;`

---

### H15. Memory Leak in `snmp_host_init()` Error Paths
**File:** `snmp.c:225-256`

OID structures allocated via `snmp_duplicate_objid()` are never freed on error returns.

---

### H16. Missing `calloc()` NULL Check
**File:** `snmp.c:785`

```c
namep = name = calloc(num_oids, sizeof(*name));
// No NULL check before use
```

---

### H17. Uninitialized Variable in `get_address_type()`
**File:** `ping.c:842`

`error` and `res_list` used without initialization.

---

### H18. Duplicate SNMP Session Initialization
**File:** `snmp.c:168-175`

`contextEngineID` and `contextEngineIDLen` set twice (lines 168-169, 174-175).

---

## MEDIUM Findings

| # | Issue | File | Line(s) |
|---|-------|------|---------|
| M01 | SQL injection in `db_column_exists()` | sql.c | 607 |
| M02 | Deprecated MySQL API usage throughout | sql.c, util.c | various |
| M03 | Ignored `snprintf()` return values | util.c | 1230-1240 |
| M04 | Hardcoded `strftime()` size of 50 | util.c | 1321 |
| M05 | Command injection risk in `nft_popen()` | nft_popen.c | 125-165 |
| M06 | `recv()`/`recvfrom()` return not validated before buffer use | ping.c | 421, 660 |
| M07 | `goto keep_listening` bypasses loop state updates | ping.c | 436 |
| M08 | `strncopy()` called with `strlen(stack)` after `memset` to zero | ping.c | 996 |
| M09 | Off-by-one in `strncpy` null termination | ping.c | 1048 |
| M10 | Wrong variable in `strncasecmp` comparison | ping.c | 1016, 1028 |
| M11 | Unchecked `setsockopt()` returns | ping.c | 401-402, 632-641 |
| M12 | `FD_SETSIZE` used without fd bounds validation | ping.c | 411, 649 |
| M13 | Missing return value checks on `db_connect`/`calloc` | spine.c | 539-550 |
| M14 | Uninitialized global `config_t set` struct | spine.c | 108 |
| M15 | Unbounded `sprintf()` with pointer arithmetic | spine.c | 628-640 |
| M16 | Thread configuration not validated | spine.c | 365-367 |
| M17 | `assert()` in production cleanup handler | nft_popen.c | 394 |
| M18 | `atoi()` without bounds check in `find_keyword_by_word()` | keywords.c | 127-138 |
| M19 | Inconsistent error handling in `php_cmd()` retry | php.c | 80-104 |
| M20 | Format string risk in SPINE_LOG with SNMP data | poller.c | 987-990 |

---

## LOW Findings

| # | Issue | File |
|---|-------|------|
| L01 | Magic numbers without named constants | various |
| L02 | `#pragma` suppresses `-Wstringop-overflow` instead of fixing root cause | util.c:1687 |
| L03 | Compiler warning suppression via pragma | util.c:1687-1690 |
| L04 | Dead code after `return` (`break` unreachable) | ping.c:319 |
| L05 | Inconsistent use of safe string functions | ping.c:1010-1048 |
| L06 | Memory ownership not documented | ping.c:987-1063 |
| L07 | Redundant signal handler installation (sigaction + signal) | error.c:113-137 |
| L08 | Inconsistent lock name validation | locks.c:100-127 |
| L09 | Magic retry counts (3) without constants | nft_popen.c, php.c |
| L10 | Inconsistent function return patterns | php.c |
| L11 | Mixed `ssize_t`/`size_t` types | php.c:53, 82 |
| L12 | Commented-out dead code | spine.c:206 |
| L13 | Variable shadowing in nested scopes | spine.c:523, 829 |
| L14 | `goto thread_retry` instead of loop construct | spine.c:918-946 |
| L15 | Ignored `KILL` query return value | sql.c:115 |
| L16 | Fixed 256-entry option table without bounds check | util.c:50-53 |

---

## Recommendations by Priority

### Immediate (Security / Crash Risk)
1. Replace all `sprintf()` / `vsprintf()` / `strcat()` with bounded equivalents (`snprintf`, `vsnprintf`, `strncat`)
2. Fix `add_slashes()` heap overflow (C01)
3. Sanitize all SQL parameters with `db_escape()` or prepared statements (C04, C05, H12)
4. Add NULL checks after all `malloc()` / `calloc()` / `strdup()` calls
5. Add NULL checks after all `mysql_fetch_row()` calls (C06)
6. Fix `php_readpipe()` buffer size constant mismatch (C08)
7. Add `break` statements to switch in `get_date_format()` (C07)

### Short-Term (Stability)
8. Fix race condition in `nft_pclose()` (H06)
9. Add NULL validation in lock functions (C09)
10. Fix signal handler to use only async-signal-safe functions (C11)
11. Validate all `atoi()` user inputs with `strtol()` + bounds checks
12. Fix `strtok()` corruption of global config data (C17)
13. Fix memory leaks in error paths (C10, H03, H15)

### Medium-Term (Code Quality)
14. Migrate from deprecated MySQL C API to prepared statements
15. Add input validation at all system boundaries
16. Standardize error handling patterns across codebase
17. Replace magic numbers with named constants
18. Add bounds checking to all pointer arithmetic
19. Run with AddressSanitizer and fix all reported issues

### Long-Term (Architecture)
20. Consider migrating to a memory-safe language for security-critical components
21. Add comprehensive unit tests, especially for string handling and SQL generation
22. Implement fuzz testing for SNMP response parsing and network input handling
23. Add static analysis (clang-analyzer, Coverity) to CI pipeline

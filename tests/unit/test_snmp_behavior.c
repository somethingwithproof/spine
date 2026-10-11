/* Real SNMP response classification and formatting, with no network session. */
#include "common.h"
#include "spine.h"
#include "test_platform_helpers.h"
static int reply_status = STAT_SUCCESS;
static int reply_type = ASN_INTEGER;
static int reply_error, reply_liberr, exchanges;
static int fake_response(void *session, netsnmp_pdu *request,
                         netsnmp_pdu **response) {
  (void)session;
  exchanges++;
  *response = NULL;
  if (reply_status == STAT_SUCCESS) {
    *response = snmp_pdu_create(SNMP_MSG_RESPONSE);
    (*response)->errstat = reply_error;
    (*response)->errindex = 0;
    for (netsnmp_variable_list *v = request->variables; v;
         v = v->next_variable) {
      long value = 42;
      snmp_pdu_add_variable(*response, v->name, v->name_length,
                            (u_char)reply_type,
                            reply_type == ASN_INTEGER ? (u_char *)&value : NULL,
                            reply_type == ASN_INTEGER ? sizeof(value) : 0);
    }
  }
  snmp_free_pdu(request);
  return reply_status;
}
static void fake_session_error(void *session, int *liberr, int *syserr,
                               char **message) {
  (void)session;
  *liberr = reply_liberr;
  *syserr = 0;
  *message = strdup("test error");
}
static oid *fake_parse(const char *input, oid *output, size_t *len) {
  const oid numeric[] = {1, 3, 6, 1, 2, 1, 1, 5, 0};
  if (input[0] != '.' || *len < sizeof(numeric) / sizeof(numeric[0]))
    return NULL;
  memcpy(output, numeric, sizeof(numeric));
  *len = sizeof(numeric) / sizeof(numeric[0]);
  return output;
}
#define snmp_parse_oid fake_parse
#define snmp_sess_synch_response fake_response
#define snmp_sess_error fake_session_error
#include "../../src/snmp.c"

int main(void) {
  int dummy_session = 0;
  const char *test_oid = ".1.3.6.1.2.1.1.5.0";
  host_t host = {.snmp_session = &dummy_session};
  netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICK_PRINT, 1);
  netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_PRINT_BARE_VALUE,
                         1);
  char *value = snmp_get_base(&host, test_oid, true);
  ASSERT_TRUE(strcmp(value, "42") == 0);
  free(value);
  ASSERT_INT_EQ(host.snmp_status, STAT_SUCCESS);
  ASSERT_INT_EQ(host.ignore_host, 0);
  value = snmp_getnext(&host, test_oid);
  ASSERT_TRUE(strcmp(value, "42") == 0);
  free(value);
  host.ignore_host = 1;
  int before = exchanges;
  value = snmp_get_base(&host, test_oid, true);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  ASSERT_INT_EQ(exchanges, before);
  host.ignore_host = 0;
  reply_status = STAT_TIMEOUT;
  value = snmp_get_base(&host, test_oid, false);
  ASSERT_TRUE(strcmp(value, "") == 0);
  free(value);
  ASSERT_INT_EQ(host.ignore_host, 0);
  ASSERT_INT_EQ(host.snmp_status, STAT_TIMEOUT);
  value = snmp_get_base(&host, test_oid, true);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  ASSERT_INT_EQ(host.ignore_host, 1);
  host.ignore_host = 0;
  reply_status = STAT_SUCCESS;
  reply_type = SNMP_NOSUCHOBJECT;
  value = snmp_get_base(&host, test_oid, true);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  ASSERT_INT_EQ(host.ignore_host, 1);
  host.ignore_host = 0;
  value = snmp_get_base(&host, ".1.3.6.1.2.1.1.1.0", true);
  ASSERT_TRUE(value[0] != '\0');
  free(value);
  ASSERT_INT_EQ(host.ignore_host, 0);
  ASSERT_INT_EQ(host.snmp_status, STAT_SUCCESS);
#if defined(SNMPERR_NOT_IN_TIME_WINDOW)
  reply_status = STAT_ERROR;
  reply_liberr = SNMPERR_NOT_IN_TIME_WINDOW;
#elif defined(SNMPERR_USM_NOTINTIMEWINDOW)
  reply_status = STAT_ERROR;
  reply_liberr = SNMPERR_USM_NOTINTIMEWINDOW;
#endif
  if (reply_status == STAT_ERROR) {
    value = snmp_get_base(&host, test_oid, true);
    ASSERT_TRUE(strcmp(value, "U") == 0);
    free(value);
    ASSERT_INT_EQ(host.ignore_host, 0);
    ASSERT_INT_EQ(host.snmp_status, STAT_SUCCESS);
  }
  snmp_oids_t oids[2] = {0};
  target_t items[2] = {0};
  for (int i = 0; i < 2; i++) {
    snprintf(oids[i].oid, sizeof(oids[i].oid), "%s", test_oid);
    oids[i].array_position = i;
  }
  reply_status = STAT_SUCCESS;
  reply_type = ASN_INTEGER;
  reply_error = 0;
  snmp_get_multi(&host, items, oids, 2);
  ASSERT_TRUE(strcmp(oids[0].result, "42") == 0 &&
              strcmp(oids[1].result, "42") == 0);
  reply_error = SNMP_ERR_GENERR;
  snmp_get_multi(&host, items, oids, 2);
  ASSERT_TRUE(strcmp(oids[0].result, "U") == 0 &&
              strcmp(oids[1].result, "U") == 0);
  reply_status = STAT_TIMEOUT;
  snmp_get_multi(&host, items, oids, 2);
  ASSERT_TRUE(strcmp(oids[0].result, "U") == 0 &&
              strcmp(oids[1].result, "U") == 0);
  ASSERT_INT_EQ(host.ignore_host, 1);
  return finish_tests("SNMP behavior");
}

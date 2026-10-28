// Provisioning console parser: command dispatch, validation, the write-only
// guarantees (token / wifi pass never in a reply) and the dev-only
// plain-http base URL. Drives provision_handle_line against the RAM port
// double; the NVS key names it asserts are exactly what sync_port_esp.c
// reads (url, token, wifi_ssid, wifi_pass).
#include "unity.h"

#include <stdio.h>
#include <string.h>

#include "provision/provision.h"
#include "provision_port_host.h"

static char reply[512];

static const char *send(const char *line)
{
    reply[0] = '\0';
    provision_handle_line(line, reply, sizeof(reply));
    return reply;
}

void setUp(void)
{
    provision_host_reset();
}

void tearDown(void) {}

void test_set_url_https_stored_under_exact_key(void)
{
    TEST_ASSERT_NOT_NULL(strstr(send("set url https://example.vn\r\n"), "ok"));
    TEST_ASSERT_EQUAL_STRING("https://example.vn", provision_host_get("url"));
}

void test_set_url_http_rejected_in_prod_build(void)
{
    provision_host_set_allow_http(false); // prod-like default
    TEST_ASSERT_NOT_NULL(strstr(send("set url http://192.168.1.10:3001"), "error"));
    TEST_ASSERT_NULL(provision_host_get("url"));
}

void test_set_url_http_accepted_in_dev_build(void)
{
    provision_host_set_allow_http(true); // dev bench (CONFIG_PROVISION_ALLOW_HTTP)
    TEST_ASSERT_NOT_NULL(strstr(send("set url http://192.168.1.10:3001"), "ok"));
    TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:3001", provision_host_get("url"));
}

void test_set_url_rejects_bad_values(void)
{
    provision_host_set_allow_http(true);
    const char *bad[] = { "", "example.vn", "https://", "https://host/",
                          "https://host with space", "ftp://host" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char line[320];
        snprintf(line, sizeof(line), "set url %s", bad[i]);
        TEST_ASSERT_NOT_NULL(strstr(send(line), "error"));
    }
    TEST_ASSERT_NULL(provision_host_get("url"));
    char longurl[200] = "https://";
    memset(longurl + 8, 'a', sizeof(longurl) - 9);
    longurl[sizeof(longurl) - 1] = '\0';
    char line[320];
    snprintf(line, sizeof(line), "set url %s", longurl);
    TEST_ASSERT_NOT_NULL(strstr(send(line), "error"));
}

void test_set_token_write_only_and_validated(void)
{
    TEST_ASSERT_NOT_NULL(strstr(send("set token s3cret-token-value"), "ok"));
    TEST_ASSERT_EQUAL_STRING("s3cret-token-value", provision_host_get("token"));
    // status never leaks the value
    TEST_ASSERT_NULL(strstr(send("status"), "s3cret-token-value"));
    // no blanks, non-empty
    TEST_ASSERT_NOT_NULL(strstr(send("set token two words"), "error"));
    TEST_ASSERT_NOT_NULL(strstr(send("set token"), "error"));
}

void test_set_wifi_keys_and_validation(void)
{
    TEST_ASSERT_NOT_NULL(strstr(send("set wifi ssid Bench Net 2"), "ok"));
    TEST_ASSERT_EQUAL_STRING("Bench Net 2", provision_host_get("wifi_ssid"));
    TEST_ASSERT_NOT_NULL(strstr(send("set wifi pass hunter2hunter2"), "ok"));
    TEST_ASSERT_EQUAL_STRING("hunter2hunter2", provision_host_get("wifi_pass"));
    // pass write-only
    TEST_ASSERT_NULL(strstr(send("status"), "hunter2hunter2"));
    // WPA lengths: 8..63
    TEST_ASSERT_NOT_NULL(strstr(send("set wifi pass short7"), "error"));
    char longpass[80];
    memset(longpass, 'x', 70);
    longpass[70] = '\0';
    char line[160];
    snprintf(line, sizeof(line), "set wifi pass %s", longpass);
    TEST_ASSERT_NOT_NULL(strstr(send(line), "error"));
    // ssid: non-empty, <= 32
    TEST_ASSERT_NOT_NULL(strstr(send("set wifi ssid "), "error"));
    char longssid[40];
    memset(longssid, 's', 33);
    longssid[33] = '\0';
    snprintf(line, sizeof(line), "set wifi ssid %s", longssid);
    TEST_ASSERT_NOT_NULL(strstr(send(line), "error"));
}

void test_status_reports_set_unset_and_provisioned(void)
{
    const char *r = send("status");
    TEST_ASSERT_NOT_NULL(strstr(r, "url: (unset)"));
    TEST_ASSERT_NOT_NULL(strstr(r, "token: (unset)"));
    TEST_ASSERT_NOT_NULL(strstr(r, "wifi ssid: (unset)"));
    TEST_ASSERT_NOT_NULL(strstr(r, "wifi pass: (unset)"));
    TEST_ASSERT_NOT_NULL(strstr(r, "provisioned: no"));

    send("set url https://example.vn");
    TEST_ASSERT_NOT_NULL(strstr(send("status"), "url: https://example.vn"));
    TEST_ASSERT_NOT_NULL(strstr(send("status"), "provisioned: no")); // token still missing

    send("set token the-token");
    r = send("status");
    TEST_ASSERT_NOT_NULL(strstr(r, "token: set"));
    TEST_ASSERT_NOT_NULL(strstr(r, "provisioned: yes"));
}

void test_provisioned_requires_url_and_token(void)
{
    TEST_ASSERT_FALSE(provisioned());
    send("set url https://example.vn");
    TEST_ASSERT_FALSE(provisioned());
    send("set token tok");
    TEST_ASSERT_TRUE(provisioned());
}

void test_unknown_and_forbidden_commands(void)
{
    // no unlock, card or test-output commands exist
    const char *forbidden[] = { "unlock", "open the door", "card add 12345678",
                                "test output", "wipe", "set url" };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        TEST_ASSERT_NOT_NULL(strstr(send(forbidden[i]), "error"));
    }
    // usage lists only the real commands
    const char *r = send("help");
    TEST_ASSERT_NULL(strstr(r, "unlock"));
    TEST_ASSERT_NOT_NULL(strstr(r, "set url"));
}

void test_empty_line_is_silent_and_crlf_tolerated(void)
{
    TEST_ASSERT_EQUAL_STRING("", send(""));
    TEST_ASSERT_EQUAL_STRING("", send("   \r\n"));
    TEST_ASSERT_NOT_NULL(strstr(send("  status  \r\n"), "url:"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_url_https_stored_under_exact_key);
    RUN_TEST(test_set_url_http_rejected_in_prod_build);
    RUN_TEST(test_set_url_http_accepted_in_dev_build);
    RUN_TEST(test_set_url_rejects_bad_values);
    RUN_TEST(test_set_token_write_only_and_validated);
    RUN_TEST(test_set_wifi_keys_and_validation);
    RUN_TEST(test_status_reports_set_unset_and_provisioned);
    RUN_TEST(test_provisioned_requires_url_and_token);
    RUN_TEST(test_unknown_and_forbidden_commands);
    RUN_TEST(test_empty_line_is_silent_and_crlf_tolerated);
    return UNITY_END();
}

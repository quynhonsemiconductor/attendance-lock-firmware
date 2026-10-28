// Provisioning console commands (§2.8): base URL, terminal token, Wi-Fi —
// set over UART0 inside the enclosure. Platform-free: the parser runs in
// the host tests; the line reader and NVS live in the port. The token and
// the Wi-Fi pass are write-only: set, never read, never printed, never
// logged.
#include "provision/provision.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "provision_port.h"

// NVS namespace "prov" keys — EXACTLY the keys sync_port_esp.c reads.
#define KEY_URL "url"
#define KEY_TOKEN "token"
#define KEY_SSID "wifi_ssid"
#define KEY_PASS "wifi_pass"

// sync_session_run reads url[128] / token[128]; Wi-Fi caps the SSID at 32
// bytes and the WPA passphrase at 8..63.
#define URL_MAX 127
#define TOKEN_MAX 127
#define SSID_MAX 32
#define PASS_MIN 8
#define PASS_MAX 63

#define USAGE \
    "usage: set url <url> | set token <token> | set wifi ssid <ssid> |\n" \
    "       set wifi pass <pass> | status\n"

static void appendf(char *out, size_t out_cap, size_t *n, const char *fmt, ...)
{
    if (*n >= out_cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *n, out_cap - *n, fmt, ap);
    va_end(ap);
    if (w > 0) {
        // w is what the reply WOULD need; clamp to what fit.
        *n += (size_t)w > out_cap - *n - 1 ? out_cap - *n - 1 : (size_t)w;
    }
}

// Strips the CR/LF a terminal appends plus surrounding blanks in place.
static void trim(char **s)
{
    char *p = *s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' ||
                     p[n - 1] == '\n')) {
        p[--n] = '\0';
    }
    *s = p;
}

// Splits at the first blank: returns the head token and points *s at the
// trimmed remainder ("" when there is none).
static char *split(char **s)
{
    char *head = *s;
    char *p = head;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') {
            p++;
        }
    }
    *s = p;
    return head;
}

// https://host[:port] (no trailing slash; http:// only in the dev build).
static bool valid_url(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > URL_MAX || v[n - 1] == '/' || strpbrk(v, " \t") != NULL) {
        return false;
    }
    bool https = strncmp(v, "https://", 8) == 0 && n > 8;
    bool http = strncmp(v, "http://", 7) == 0 && n > 7;
    return https || (http && provision_port_allow_http());
}

static void cmd_set(char *args, char *out, size_t out_cap, size_t *n)
{
    char *key = split(&args);
    if (key[0] == '\0' || args[0] == '\0') {
        appendf(out, out_cap, n, "error: set needs a key and a value\n" USAGE);
        return;
    }
    trim(&args); // the value is the rest of the line (Wi-Fi pass may contain blanks)

    if (strcmp(key, "url") == 0) {
        if (!valid_url(args)) {
            appendf(out, out_cap, n,
                    "error: url must be https://host[:port] without a trailing slash "
                    "(http:// is dev-build only)\n");
            return;
        }
        if (!provision_port_set_str(KEY_URL, args)) {
            appendf(out, out_cap, n, "error: could not store the url\n");
            return;
        }
        appendf(out, out_cap, n, "ok: url set\n");
        return;
    }
    if (strcmp(key, "token") == 0) {
        size_t len = strlen(args);
        if (len == 0 || len > TOKEN_MAX || strpbrk(args, " \t") != NULL) {
            appendf(out, out_cap, n, "error: token must be 1..127 characters without blanks\n");
            return;
        }
        if (!provision_port_set_str(KEY_TOKEN, args)) {
            appendf(out, out_cap, n, "error: could not store the token\n");
            return;
        }
        appendf(out, out_cap, n, "ok: token set\n");
        return;
    }
    if (strcmp(key, "wifi") == 0) {
        char *wkey = split(&args);
        trim(&args);
        if (strcmp(wkey, "ssid") == 0) {
            size_t len = strlen(args);
            if (len == 0 || len > SSID_MAX) {
                appendf(out, out_cap, n, "error: ssid must be 1..32 bytes\n");
                return;
            }
            if (!provision_port_set_str(KEY_SSID, args)) {
                appendf(out, out_cap, n, "error: could not store the wifi ssid\n");
                return;
            }
            appendf(out, out_cap, n, "ok: wifi ssid set\n");
            return;
        }
        if (strcmp(wkey, "pass") == 0) {
            size_t len = strlen(args);
            if (len < PASS_MIN || len > PASS_MAX) {
                appendf(out, out_cap, n, "error: wifi pass must be 8..63 bytes\n");
                return;
            }
            if (!provision_port_set_str(KEY_PASS, args)) {
                appendf(out, out_cap, n, "error: could not store the wifi pass\n");
                return;
            }
            appendf(out, out_cap, n, "ok: wifi pass set\n");
            return;
        }
    }
    appendf(out, out_cap, n, "error: unknown key\n" USAGE);
}

static void cmd_status(char *out, size_t out_cap, size_t *n)
{
    char url[URL_MAX + 1], ssid[SSID_MAX + 1];
    bool has_url = provision_port_get_str(KEY_URL, url, sizeof(url));
    bool has_ssid = provision_port_get_str(KEY_SSID, ssid, sizeof(ssid));
    // The token and the wifi pass are write-only: set/unset, never the value.
    appendf(out, out_cap, n, "url: %s\n", has_url ? url : "(unset)");
    appendf(out, out_cap, n, "token: %s\n", provision_port_has_str(KEY_TOKEN) ? "set" : "(unset)");
    appendf(out, out_cap, n, "wifi ssid: %s\n", has_ssid ? ssid : "(unset)");
    appendf(out, out_cap, n, "wifi pass: %s\n", provision_port_has_str(KEY_PASS) ? "set" : "(unset)");
    appendf(out, out_cap, n, "provisioned: %s\n", provisioned() ? "yes" : "no");
}

void provision_handle_line(const char *line, char *out, size_t out_cap)
{
    size_t n = 0;
    out[0] = '\0';
    char buf[256];
    if (strlen(line) >= sizeof(buf)) {
        appendf(out, out_cap, &n, "error: line too long\n");
        return;
    }
    snprintf(buf, sizeof(buf), "%s", line);
    char *s = buf;
    trim(&s);
    if (s[0] == '\0') {
        return; // empty line: just prompt again
    }
    char *cmd = split(&s);
    if (strcmp(cmd, "set") == 0) {
        cmd_set(s, out, out_cap, &n);
    } else if (strcmp(cmd, "status") == 0 && s[0] == '\0') {
        cmd_status(out, out_cap, &n);
    } else {
        appendf(out, out_cap, &n, "error: unknown command\n" USAGE);
    }
}

bool provisioned(void)
{
    return provision_port_has_str(KEY_URL) && provision_port_has_str(KEY_TOKEN);
}

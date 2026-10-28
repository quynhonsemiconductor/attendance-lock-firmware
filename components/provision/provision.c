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

// Dead-store-proof wipe (volatile): the line buffer held a secret value;
// memset can be elided, this cannot.
static void zeroize(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

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

// THE tokenizer — the handler and the echo-mask decision both use it, so a
// line runs of blanks cannot unmask: skips blank runs, points *out at the
// token start, returns its length (0 at end of line). Never mutates.
static size_t tok(const char *s, const char **out)
{
    while (s[0] == ' ' || s[0] == '\t') {
        s++;
    }
    const char *start = s;
    while (s[0] != '\0' && s[0] != ' ' && s[0] != '\t') {
        s++;
    }
    *out = start;
    return (size_t)(s - start);
}

// Token-equals for the non-mutating tokenizer.
static bool tok_is(const char *t, size_t len, const char *lit)
{
    return len == strlen(lit) && strncmp(t, lit, len) == 0;
}

// scheme://host[:port] EXACTLY (http:// only in the dev build): the sync
// client appends its own paths, so a base URL that carries a path, query,
// fragment or userinfo separator is a mistake, not a feature. The host is
// a hostname (letters, digits, '.', '-'): no IPv6 literals, no underscores.
static bool valid_url(const char *v)
{
    size_t n = strlen(v);
    if (n == 0 || n > URL_MAX || strpbrk(v, " \t") != NULL) {
        return false;
    }
    const char *host = NULL;
    if (strncmp(v, "https://", 8) == 0) {
        host = v + 8;
    } else if (provision_port_allow_http() && strncmp(v, "http://", 7) == 0) {
        host = v + 7;
    }
    if (host == NULL || host[0] == '\0') {
        return false;
    }
    if (strpbrk(host, "/?#@") != NULL) { // path / query / fragment / userinfo
        return false;
    }
    const char *colon = strchr(host, ':');
    const char *host_end = colon != NULL ? colon : host + strlen(host);
    if (host_end == host) {
        return false; // ":port" with no host
    }
    for (const char *p = host; p < host_end; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '.' || *p == '-';
        if (!ok) {
            return false;
        }
    }
    if (colon != NULL) {
        const char *port = colon + 1;
        if (port[0] == '\0' || strlen(port) > 5) {
            return false;
        }
        long pv = 0;
        for (const char *p = port; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            pv = pv * 10 + (*p - '0');
        }
        if (pv < 1 || pv > 65535) {
            return false;
        }
    }
    return true;
}

// value points at the rest of the line after the key token (leading blanks
// skipped; interior blanks kept — a Wi-Fi pass may contain them).
static void cmd_set(const char *value, size_t value_len, bool wifi_key, char *out,
                    size_t out_cap, size_t *n)
{
    if (!wifi_key) {
        if (value_len == 0 || value_len > TOKEN_MAX || strpbrk(value, " \t") != NULL) {
            appendf(out, out_cap, n,
                    "error: token must be 1..127 characters without blanks\n");
            return;
        }
        if (!provision_port_set_str(KEY_TOKEN, value)) {
            appendf(out, out_cap, n, "error: could not store the token\n");
            return;
        }
        appendf(out, out_cap, n, "ok: token set\n");
        return;
    }
    if (value_len == 0 || value_len > SSID_MAX) {
        appendf(out, out_cap, n, "error: ssid must be 1..32 bytes\n");
        return;
    }
    if (!provision_port_set_str(KEY_SSID, value)) {
        appendf(out, out_cap, n, "error: could not store the wifi ssid\n");
        return;
    }
    appendf(out, out_cap, n, "ok: wifi ssid set\n");
}

static void cmd_set_pass(const char *value, size_t value_len, char *out, size_t out_cap,
                         size_t *n)
{
    if (value_len < PASS_MIN || value_len > PASS_MAX) {
        appendf(out, out_cap, n, "error: wifi pass must be 8..63 bytes\n");
        return;
    }
    if (!provision_port_set_str(KEY_PASS, value)) {
        appendf(out, out_cap, n, "error: could not store the wifi pass\n");
        return;
    }
    appendf(out, out_cap, n, "ok: wifi pass set\n");
}

// Dispatch for "set …": [set] key tokens, then the value is the rest of the
// line. Runs of blanks separate tokens exactly like tok() (and the echo
// mask), so every accepted spelling of a secret command stays masked.
static void cmd_set_dispatch(const char *args, char *out, size_t out_cap, size_t *n)
{
    const char *t;
    size_t klen = tok(args, &t);
    if (klen == 0) {
        appendf(out, out_cap, n, "error: set needs a key and a value\n" USAGE);
        return;
    }
    const char *value = t + klen;
    while (value[0] == ' ' || value[0] == '\t') {
        value++;
    }
    size_t vlen = strlen(value);
    if (vlen == 0) {
        appendf(out, out_cap, n, "error: set needs a key and a value\n" USAGE);
        return;
    }

    if (tok_is(t, klen, "url")) {
        if (!valid_url(value)) {
            appendf(out, out_cap, n,
                    "error: url must be scheme://host[:port] with no path, query or "
                    "fragment (https only; http:// is dev-build only)\n");
            return;
        }
        if (!provision_port_set_str(KEY_URL, value)) {
            appendf(out, out_cap, n, "error: could not store the url\n");
            return;
        }
        appendf(out, out_cap, n, "ok: url set\n");
        return;
    }
    if (tok_is(t, klen, "token")) {
        cmd_set(value, vlen, false, out, out_cap, n);
        return;
    }
    if (tok_is(t, klen, "wifi")) {
        const char *wt;
        size_t wlen = tok(value, &wt);
        const char *wval = wt + wlen;
        while (wval[0] == ' ' || wval[0] == '\t') {
            wval++;
        }
        size_t wvlen = strlen(wval);
        if (tok_is(wt, wlen, "ssid")) {
            cmd_set(wval, wvlen, true, out, out_cap, n);
            return;
        }
        if (tok_is(wt, wlen, "pass")) {
            cmd_set_pass(wval, wvlen, out, out_cap, n);
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

// True when the line being typed has reached a write-only value: from
// "set token " / "set wifi pass " on (with ANY blank run as separator —
// tokenized exactly like the handler via tok()), the console echoes '*' so
// the monitor (and the logs BENCH-TEST asks to save) never shows the secret.
bool provision_input_masked(const char *line)
{
    const char *t;
    size_t len = tok(line, &t);
    if (!tok_is(t, len, "set")) {
        return false;
    }
    const char *rest = t + len;
    len = tok(rest, &t);
    if (tok_is(t, len, "token")) {
        return t[len] == ' ' || t[len] == '\t'; // a separator is typed: value follows
    }
    if (tok_is(t, len, "wifi")) {
        rest = t + len;
        len = tok(rest, &t);
        if (tok_is(t, len, "pass")) {
            return t[len] == ' ' || t[len] == '\t';
        }
    }
    return false;
}

void provision_handle_line(const char *line, char *out, size_t out_cap)
{
    size_t n = 0;
    out[0] = '\0';
    char buf[256];
    if (strlen(line) < sizeof(buf)) {
        snprintf(buf, sizeof(buf), "%s", line);
        char *s = buf;
        trim(&s); // the terminal's CR/LF and surrounding blanks
        const char *t;
        size_t len = tok(s, &t);
        if (len > 0) {
            const char *args = t + len;
            if (tok_is(t, len, "set")) {
                cmd_set_dispatch(args, out, out_cap, &n);
            } else if (tok_is(t, len, "status") && tok(args, &t) == 0) {
                cmd_status(out, out_cap, &n);
            } else {
                appendf(out, out_cap, &n, "error: unknown command\n" USAGE);
            }
        } // else: empty line — just prompt again
    } else {
        appendf(out, out_cap, &n, "error: line too long\n");
    }
    zeroize(buf, sizeof(buf)); // the line may have held a secret value
}

bool provisioned(void)
{
    return provision_port_has_str(KEY_URL) && provision_port_has_str(KEY_TOKEN);
}

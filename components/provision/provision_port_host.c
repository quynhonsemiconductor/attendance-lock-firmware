// Host double for the provision port: a small RAM map plus the profile
// flag. Test helpers live in provision_port_host.h.
#include "provision_port_host.h"

#include <stdio.h>
#include <string.h>

#include "provision_port.h"

#define HOST_MAX 8
#define HOST_KEY_MAX 16
#define HOST_VAL_MAX 256

typedef struct {
    char key[HOST_KEY_MAX];
    char val[HOST_VAL_MAX];
} ent_t;

static ent_t ents[HOST_MAX];
static int nents;
static bool allow_http;

bool provision_port_set_str(const char *key, const char *value)
{
    if (strlen(key) >= HOST_KEY_MAX || strlen(value) >= HOST_VAL_MAX) {
        return false;
    }
    ent_t *e = NULL;
    for (int i = 0; i < nents; i++) {
        if (strcmp(ents[i].key, key) == 0) {
            e = &ents[i];
            break;
        }
    }
    if (e == NULL) {
        if (nents >= HOST_MAX) {
            return false;
        }
        e = &ents[nents++];
        snprintf(e->key, HOST_KEY_MAX, "%s", key);
    }
    snprintf(e->val, HOST_VAL_MAX, "%s", value);
    return true;
}

bool provision_port_get_str(const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    const char *v = provision_host_get(key);
    if (v == NULL) {
        return false;
    }
    snprintf(out, cap, "%s", v);
    return out[0] != '\0';
}

bool provision_port_has_str(const char *key)
{
    return provision_host_get(key) != NULL;
}

bool provision_port_allow_http(void)
{
    return allow_http;
}

void provision_host_reset(void)
{
    nents = 0;
    allow_http = false; // prod-like by default; tests opt into the dev bench
}

void provision_host_set_allow_http(bool on)
{
    allow_http = on;
}

const char *provision_host_get(const char *key)
{
    for (int i = 0; i < nents; i++) {
        if (strcmp(ents[i].key, key) == 0) {
            return ents[i].val;
        }
    }
    return NULL;
}

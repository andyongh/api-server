#include "auth.h"
#include "log.h"
#include <string.h>
#include <stddef.h>

/*
 * STUB: Accept tokens with prefix "valid_<username>" (role=user)
 *       or "admin_<username>" (role=admin).
 *
 * Examples:
 *   "valid_alice"  → username="alice", role="user"
 *   "admin_bob"    → username="bob",   role="admin"
 *   anything else  → rejected
 *
 * Replace this entire function body with your real auth logic.
 */
bool auth_verify(const char *token, user_info_t *out) {
    if (!token || !out) return false;

    const char *username = NULL;
    const char *role     = NULL;

    if (strncmp(token, "valid_", 6) == 0 && token[6] != '\0') {
        username = token + 6;
        role     = "user";
    } else if (strncmp(token, "admin_", 6) == 0 && token[6] != '\0') {
        username = token + 6;
        role     = "admin";
    } else {
        LOGD("auth_verify: rejected token '%.32s...'", token);
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->session_token, sizeof(out->session_token), "%s", token);
    snprintf(out->username,      sizeof(out->username),      "%s", username);
    snprintf(out->role,          sizeof(out->role),          "%s", role);

    LOGD("auth_verify: accepted user='%s' role='%s'", out->username, out->role);
    return true;
}

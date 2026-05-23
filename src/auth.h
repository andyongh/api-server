#pragma once
#include <stdbool.h>
#include <stddef.h>

#define AUTH_TOKEN_MAX   256
#define AUTH_USER_MAX    64
#define AUTH_ROLE_MAX    32

typedef struct {
    char session_token[AUTH_TOKEN_MAX];
    char username[AUTH_USER_MAX];
    char role[AUTH_ROLE_MAX];  /* "user" | "admin" */
} user_info_t;

/**
 * auth_verify() — Stub authentication function.
 *
 * In production, replace this body with a real token validation
 * (e.g. JWT signature check, database lookup, Redis cache).
 *
 * @param token   Value of params.session from the JSONRPC request.
 * @param out     Filled on success with the authenticated user info.
 * @return        true if token is valid, false otherwise.
 */
bool auth_verify(const char *token, user_info_t *out);
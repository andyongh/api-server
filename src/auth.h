#pragma once
#include <stdbool.h>

#define AUTH_TOKEN_MAX 256
#define AUTH_USER_MAX   64
#define AUTH_ROLE_MAX   32

typedef struct {
    char session_token[AUTH_TOKEN_MAX];
    char username[AUTH_USER_MAX];
    char role[AUTH_ROLE_MAX];   /* "user" | "admin" */
} user_info_t;

/* Stub: replace body with real JWT / Redis / DB lookup */
bool auth_verify(const char *token, user_info_t *out);

#pragma once
/* Generate a RFC-4122 version-4 UUID string.
 * buf must be at least 37 bytes (36 chars + NUL). */
void uuid_v4(char *buf);

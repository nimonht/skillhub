// JSON-RPC 2.0 over lines (docs/protocol.md): cutting a byte stream into messages, reading one, and writing results,
// errors and notifications. No I/O and no threads: `skillhub serve` feeds it what arrives on its standard input, and the
// fuzzer feeds it anything at all.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "yyjson.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_RPC_MAX_LINE (64u * 1024 * 1024)

// JSON-RPC's own error codes, and Skill Hub's (docs/protocol.md, Errors)
enum {
    SH_RPC_PARSE_ERROR = -32700,
    SH_RPC_INVALID_REQUEST = -32600,
    SH_RPC_UNKNOWN_METHOD = -32601,
    SH_RPC_INVALID_PARAMS = -32602,
    SH_RPC_PROTOCOL_MISMATCH = -32002,
    SH_RPC_BUSY = 1001,
    SH_RPC_LOCKED = 1002,
    SH_RPC_REFUSED = 1003,
    SH_RPC_NOT_FOUND = 1004,
    SH_RPC_CONFLICT = 1005,
    SH_RPC_CANCELLED = 1006,
    SH_RPC_UNPARSABLE = 1007,
};

// ---- framing ----

typedef struct sh_rpc_framer sh_rpc_framer;
sh_rpc_framer *sh_rpc_framer_new(size_t max_line);
void sh_rpc_framer_free(sh_rpc_framer *f);
// Feeds bytes as they arrive. Each complete line goes to on_line without its "\n" (or "\r\n"). A line longer than
// max_line goes there once as NULL, and its bytes up to the next newline are dropped.
typedef void (*sh_rpc_line_fn)(void *user, const char *line, size_t len);
void sh_rpc_feed(sh_rpc_framer *f, const char *data, size_t n, sh_rpc_line_fn on_line, void *user);

// ---- reading a message ----

typedef enum { SH_RPC_REQUEST, SH_RPC_NOTIFICATION, SH_RPC_RESPONSE, SH_RPC_BAD } sh_rpc_kind;

typedef struct {
    sh_rpc_kind kind;
    yyjson_doc *doc;
    yyjson_val *id;      // requests and responses; a bad message keeps its id when it had a usable one
    const char *method;  // requests and notifications
    yyjson_val *params;  // an object or an array, or NULL when there is none
    int code;            // SH_RPC_BAD: SH_RPC_PARSE_ERROR or SH_RPC_INVALID_REQUEST
    const char *message; // SH_RPC_BAD: why
} sh_rpc_msg;

void sh_rpc_parse(const char *line, size_t len, sh_rpc_msg *out);
void sh_rpc_msg_free(sh_rpc_msg *m);

// ---- writing ----
// Each returns one message as a line ending in "\n" (malloc'd). The wrapper object is built inside `d`, the document that
// holds `result` or `params`, so a big result is never copied. `id` may be NULL (written as null).

char *sh_rpc_result(yyjson_mut_doc *d, yyjson_val *id, yyjson_mut_val *result);
char *sh_rpc_error(yyjson_val *id, int code, const char *message);
char *sh_rpc_notification(yyjson_mut_doc *d, const char *method, yyjson_mut_val *params);

#ifdef __cplusplus
}
#endif

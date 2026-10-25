#include "sh_rpc.h"

#include <stdlib.h>
#include <string.h>

#include "sh_str.h"

// ---- framing ----

struct sh_rpc_framer {
    size_t max;
    sh_buf line;    // the line so far
    bool skipping;  // inside a line that was too long
};

sh_rpc_framer *sh_rpc_framer_new(size_t max_line) {
    sh_rpc_framer *f = sh_xcalloc(1, sizeof *f);
    f->max = max_line;
    return f;
}

void sh_rpc_framer_free(sh_rpc_framer *f) {
    if (!f) return;
    sh_buf_free(&f->line);
    free(f);
}

static void deliver(sh_rpc_framer *f, sh_rpc_line_fn on_line, void *user) {
    size_t len = f->line.len;
    if (len > 0 && f->line.data[len - 1] == '\r') --len;
    on_line(user, len ? f->line.data : "", len);
    sh_buf_clear(&f->line);
}

void sh_rpc_feed(sh_rpc_framer *f, const char *data, size_t n, sh_rpc_line_fn on_line, void *user) {
    while (n > 0) {
        const char *nl = memchr(data, '\n', n);
        const size_t take = nl ? (size_t)(nl - data) : n;
        if (!f->skipping && f->line.len + take > f->max) {
            on_line(user, NULL, 0);  // too long: reported once, the rest of it is dropped
            sh_buf_clear(&f->line);
            f->skipping = true;
        }
        if (!f->skipping) sh_buf_append(&f->line, data, take);
        data += take;
        n -= take;
        if (!nl) break;
        ++data;
        --n;
        if (f->skipping) f->skipping = false;
        else deliver(f, on_line, user);
    }
}

// ---- reading a message ----

static void bad(sh_rpc_msg *m, int code, const char *message) {
    m->kind = SH_RPC_BAD;
    m->code = code;
    m->message = message;
}

// reads `out->doc`; returns false with out->code and out->message set when it is not a valid message
static bool parse_message(sh_rpc_msg *out) {
    yyjson_val *root = out->doc ? yyjson_doc_get_root(out->doc) : NULL;
    if (!root) {
        bad(out, SH_RPC_PARSE_ERROR, "not JSON");
        return false;
    }
    if (!yyjson_is_obj(root)) {
        bad(out, SH_RPC_INVALID_REQUEST, "expected one JSON object per line (batches are not supported)");
        return false;
    }
    yyjson_val *id = yyjson_obj_get(root, "id");
    if (id && (yyjson_is_str(id) || yyjson_is_num(id) || yyjson_is_null(id))) out->id = id;
    const char *why = NULL;
    yyjson_val *method = yyjson_obj_get(root, "method"), *params = yyjson_obj_get(root, "params");
    if (!yyjson_equals_str(yyjson_obj_get(root, "jsonrpc"), "2.0")) why = "\"jsonrpc\" must be \"2.0\"";
    else if (id && !out->id) why = "\"id\" must be a number or a string";
    else if (!method && out->id && (yyjson_obj_get(root, "result") || yyjson_obj_get(root, "error"))) out->kind = SH_RPC_RESPONSE;
    else if (!method) why = "\"method\" is missing";
    else if (!yyjson_is_str(method)) why = "\"method\" must be a string";
    else if (params && !yyjson_is_obj(params) && !yyjson_is_arr(params)) why = "\"params\" must be an object or an array";
    if (why) {
        bad(out, SH_RPC_INVALID_REQUEST, why);
        return false;
    }
    if (out->kind == SH_RPC_RESPONSE) return true;
    out->kind = id ? SH_RPC_REQUEST : SH_RPC_NOTIFICATION;
    out->method = yyjson_get_str(method);
    out->params = params;
    return true;
}

void sh_rpc_parse(const char *line, size_t len, sh_rpc_msg *out) {
    memset(out, 0, sizeof *out);
    out->doc = yyjson_read(line, len, YYJSON_READ_ALLOW_INVALID_UNICODE);
    parse_message(out);
}

void sh_rpc_msg_free(sh_rpc_msg *m) {
    yyjson_doc_free(m->doc);
    memset(m, 0, sizeof *m);
}

// ---- writing ----

// written compactly, so the message holds no raw newline; names on disk need not be valid UTF-8, and pass as they are
static char *line_of(yyjson_mut_val *root) {
    size_t len = 0;
    char *json = yyjson_mut_val_write(root, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &len);
    if (!json) return NULL;
    char *line = sh_xmalloc(len + 2);
    memcpy(line, json, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    free(json);
    return line;
}

static yyjson_mut_val *envelope(yyjson_mut_doc *d, yyjson_val *id) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, o, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(d, o, "id", id ? yyjson_val_mut_copy(d, id) : yyjson_mut_null(d));
    return o;
}

char *sh_rpc_result(yyjson_mut_doc *d, yyjson_val *id, yyjson_mut_val *result) {
    yyjson_mut_val *o = envelope(d, id);
    yyjson_mut_obj_add_val(d, o, "result", result ? result : yyjson_mut_obj(d));
    return line_of(o);
}

char *sh_rpc_error(yyjson_val *id, int code, const char *message) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = envelope(d, id);
    yyjson_mut_val *e = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, e, "code", code);
    yyjson_mut_obj_add_strcpy(d, e, "message", message ? message : "");
    yyjson_mut_obj_add_val(d, o, "error", e);
    char *line = line_of(o);
    yyjson_mut_doc_free(d);
    return line;
}

char *sh_rpc_notification(yyjson_mut_doc *d, const char *method, yyjson_mut_val *params) {
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, o, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(d, o, "method", method);
    if (params) yyjson_mut_obj_add_val(d, o, "params", params);
    return line_of(o);
}

// libFuzzer target: whatever arrives on `skillhub serve`'s standard input, cut into lines at arbitrary points.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "sh_rpc.h"

static void on_line(void *user, const char *line, size_t len) {
    (void)user;
    if (!line) return;  // too long
    sh_rpc_msg m;
    sh_rpc_parse(line, len, &m);
    char *reply = m.kind == SH_RPC_BAD ? sh_rpc_error(m.id, m.code, m.message) : sh_rpc_error(m.id, SH_RPC_UNKNOWN_METHOD, m.method);
    free(reply);
    sh_rpc_msg_free(&m);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sh_rpc_framer *f = sh_rpc_framer_new(4096);
    // the first byte says where the stream is split, so lines also arrive in pieces
    const size_t cut = size ? data[0] % (size + 1) : 0;
    sh_rpc_feed(f, (const char *)data, cut, on_line, NULL);
    sh_rpc_feed(f, (const char *)data + cut, size - cut, on_line, NULL);
    sh_rpc_framer_free(f);
    return 0;
}

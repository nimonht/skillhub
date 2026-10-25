// `skillhub serve --stdio`: the backend the window starts and talks to over JSON-RPC (docs/protocol.md). Linked into the
// command-line programs only; it is the one part of Skill Hub that needs libuv.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int sh_serve_stdio(void);  // returns the exit code once standard input has closed and the last job has stopped

#ifdef __cplusplus
}
#endif

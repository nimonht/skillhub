# yyjson 0.13.0

Fast JSON reader and writer in C, by YaoYuan. MIT licence (`LICENSE`). Unmodified copy of `src/yyjson.c` and `src/yyjson.h`
from <https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz>
(sha256 `34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae`).

Vendored because Debian 12 and Ubuntu 24.04 do not package it. Used by the C core for every JSON file (lock files, settings,
the agent catalog, exports) and for the backend protocol.

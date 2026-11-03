// libFuzzer target: the offline check reads every file of every skill, from any repository.
//   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DSKILLHUB_FUZZ=ON -DSKILLHUB_GUI=OFF && build-fuzz/fuzz_scan -max_total_time=600
#include <stddef.h>
#include <stdint.h>

#include "sh_scan.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sh_findings f = {0};
    sh_scan_text("SKILL.md", (const char *)data, size, &f);
    sh_findings_free(&f);
    return 0;
}

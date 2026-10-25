// libFuzzer target: the SKILL.md front matter parser reads files from any repository and any export.
//   cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DSKILLHUB_FUZZ=ON -DSKILLHUB_GUI=OFF && build-fuzz/fuzz_frontmatter -max_total_time=600
#include <stddef.h>
#include <stdint.h>

#include "sh_frontmatter.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    sh_frontmatter fm;
    sh_fm_parse((const char *)data, size, &fm);
    (void)sh_fm_get(&fm, "description");
    sh_fm_free(&fm);
    return 0;
}

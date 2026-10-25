# Turns a file into a C source with its bytes: `const char NAME[]` (NUL-terminated) and `const size_t NAME_len`.
#   cmake -DINPUT=data/providers.json -DOUTPUT=build/providers_json.c -DNAME=sh_providers_json -P cmake/EmbedFile.cmake
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR size "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x[0-9a-f][0-9a-f],){24}" "\\0\n    " bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
    "// Generated from ${INPUT} by cmake/EmbedFile.cmake. Do not edit.\n"
    "#include <stddef.h>\n"
    "const char ${NAME}[] = {\n    ${bytes}0x00};\n"
    "const size_t ${NAME}_len = ${size};\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")

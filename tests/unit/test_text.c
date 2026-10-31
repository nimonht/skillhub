// Strings, UTF-8 rules that must match Qt, hashes and base64.
#include "test.h"

#include "sh_hash.h"
#include "sh_str.h"
#include "sh_utf.h"

static char *hex_sha1(const char *s, size_t n) {
    sh_sha1 c;
    unsigned char d[20];
    char *out = malloc(41);
    sh_sha1_init(&c);
    sh_sha1_update(&c, s, n);
    sh_sha1_final(&c, d);
    sh_hex(d, 20, out);
    return out;
}

static char *hex_sha256(const char *s, size_t n) {
    sh_sha256 c;
    unsigned char d[32];
    char *out = malloc(65);
    sh_sha256_init(&c);
    sh_sha256_update(&c, s, n);
    sh_sha256_final(&c, d);
    sh_hex(d, 32, out);
    return out;
}

static void hashes(void) {
    CHECK_STR(hex_sha1("abc", 3), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_STR(hex_sha1("", 0), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK_STR(hex_sha256("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_STR(hex_sha256("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // git's object id of a blob is SHA-1 over "blob <size>\0<content>": `printf 'hello\n' | git hash-object --stdin`
    CHECK_STR(hex_sha1("blob 6\0hello\n", 13), "ce013625030ba8dba906f756967f9e9ca394464a");
    // more than one block, and a length on the 56-byte padding edge
    char block[200];
    memset(block, 'a', sizeof block);
    CHECK_STR(hex_sha256(block, 56), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK_STR(hex_sha1(block, 200), "e61cfffe0d9195a525fc6cf06ca2d77119c24a40");  // python3 hashlib
}

static void controls(void) {
    char text[] = "a\x1b[2Jb\tc\x07\x7f\xc2\x9b" "31md\xc3\xa9";  // ESC, BEL, DEL, the 8-bit CSI; tab and letters stay
    sh_strip_controls(text);
    CHECK_EQ_STR(text, "a[2Jb\tc31md\xc3\xa9");
}

static void base64(void) {
    sh_buf b = SH_BUF_INIT;
    sh_base64_encode("ab", 2, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "YWI=");
    sh_buf_clear(&b);
    sh_base64_encode("", 0, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "");
    sh_buf_clear(&b);
    // Qt's lenient decoder: characters outside the alphabet are skipped, padding is optional
    sh_base64_decode_lenient("QUJD*&^%REVG", 12, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "ABCDEF");
    sh_buf_clear(&b);
    sh_base64_decode_lenient("YWI", 3, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "ab");
    sh_buf_clear(&b);
    sh_base64_decode_lenient("YW\nI=\r\n", 7, &b);
    CHECK_EQ_STR(sh_buf_str(&b), "ab");
    sh_buf_free(&b);
}

static void trimming(void) {
    CHECK_STR(sh_trimmed("  x  ", 5), "x");
    CHECK_STR(sh_trimmed("\xC2\xA0 value\xC2\xA0\t", 11), "value");  // NBSP counts as a space, as in Qt
    CHECK_STR(sh_trimmed("", 0), "");
    CHECK_STR(sh_trimmed(" \t ", 3), "");
    CHECK_STR(sh_trimmed("\xEF\xBB\xBF---", 6), "\xEF\xBB\xBF---");  // a byte order mark is not a space
    CHECK_STR(sh_simplified("  first line  indented   more \n last  ", 38), "first line indented more last");
    CHECK(sh_is_blank(" \t\xE2\x80\x83"));  // EM SPACE
    CHECK(!sh_is_blank(" x "));
}

static void slugs(void) {
    CHECK_STR(sh_slugify("My Agent"), "my-agent");
    CHECK_STR(sh_slugify("  --Hello__World!!  "), "hello-world");
    CHECK_STR(sh_slugify("Caf\xC3\xA9 Agent"), "caf\xC3\xA9-agent");
    CHECK_STR(sh_slugify("A\xF0\x9F\xA7\xA9" "B"), "a-b");  // outside the BMP: a separator, as Qt's UTF-16 loop saw it
    CHECK_STR(sh_slugify("\xC3\x9C" "BER"), "\xC3\xBC" "ber");
    CHECK_STR(sh_slugify("!!!"), "");
}

static void utf16(void) {
    CHECK(sh_utf16_len("abc") == 3);
    CHECK(sh_utf16_len("\xF0\x9F\xA7\xA9") == 2);
    CHECK_STR(sh_left_utf16("AAA\xF0\x9F\xA7\xA9", 4), "AAA");  // never half a surrogate pair
    CHECK_STR(sh_left_utf16("AAA\xF0\x9F\xA7\xA9", 5), "AAA\xF0\x9F\xA7\xA9");
    CHECK_STR(sh_left_utf16("h\xC3\xA9llo", 2), "h\xC3\xA9");
    CHECK_STR(sh_utf8_clean("a\xFF" "b", 3), "a\xEF\xBF\xBD" "b");
}

static void compare(void) {
    CHECK(sh_casecmp("alpha", "Beta") < 0);
    CHECK(sh_casecmp("Alpha", "alpha") < 0);  // equal ignoring case: byte order decides
    CHECK(sh_casecmp("caf\xC3\xA9", "CAF\xC3\x89") > 0);
    CHECK(sh_casecmp("b", "a") > 0);
    CHECK(sh_casecmp("ab", "abc") < 0);
}

static void strings(void) {
    sh_strv v = SH_STRV_INIT;
    sh_split("a\n\nb\n", 5, '\n', false, &v);
    CHECK(v.len == 4);
    sh_strv_clear(&v);
    sh_split("a\n\nb\n", 5, '\n', true, &v);
    CHECK(v.len == 2 && strcmp(v.items[1], "b") == 0);
    CHECK_STR(sh_strv_join(&v, ", "), "a, b");
    sh_strv_free(&v);
    CHECK(sh_starts_with("skills/x/SKILL.md", "skills/"));
    CHECK(sh_ends_with("skills/x/SKILL.md", "/SKILL.md"));
    CHECK_STR(sh_strfmt("%s-%d", "x", 3), "x-3");
}

const sh_test_case sh_tests_text[] = {
    {"text.controls", controls},
    {"text.hashes", hashes},   {"text.base64", base64}, {"text.trimming", trimming}, {"text.slugs", slugs},
    {"text.utf16", utf16},     {"text.compare", compare}, {"text.strings", strings},  {NULL, NULL},
};

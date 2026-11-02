// Which version an install may move to, and what counts as signed: the rules that decide what runs on people's machines.
#include "test.h"

#include <stdlib.h>
#include <string.h>

#include "sh_selfupdate.h"

static void test_order(void) {
    static const char *const ascending[] = {"1.0.1", "2.0.0-alpha1", "2.0.0-beta1", "2.0.0-beta2", "2.0.0-beta10", "2.0.0-rc1", "2.0.0", "2.0.1", "2.1.0", "10.0.0"};
    const size_t n = sizeof ascending / sizeof ascending[0];
    for (size_t i = 0; i < n; ++i) {
        CHECK(sh_version_valid(ascending[i]));
        CHECK(sh_version_compare(ascending[i], ascending[i]) == 0);
        for (size_t k = i + 1; k < n; ++k) {
            CHECK(sh_version_compare(ascending[i], ascending[k]) < 0);
            CHECK(sh_version_compare(ascending[k], ascending[i]) > 0);
        }
    }
}

static void test_invalid_versions(void) {
    static const char *const bad[] = {"", "2", "2.0", "2.0.0.0", "v2.0.0", "2.0.0-", "2.0.0-Beta1", "2.0.0-beta1x", "2.0.0-beta-1", " 2.0.0", "2.0.0 ",
                                      "1234567890.0.0", "2.0.0-beta1234567890", "2.0.0-averyveryverylonglabel1", "2..0", "-1.0.0"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(!sh_version_valid(bad[i]));
    CHECK(sh_version_valid("2.0.0-dev"));
}

static char *candidates_of(const char *running, size_t max) {
    static const char *const raw[] = {"refs/tags/v1.0.0", "refs/tags/v1.0.1", "refs/tags/v2.0.0-beta1", "refs/tags/v2.0.0-beta2", "refs/tags/v2.0.0-beta10",
                                      "refs/tags/v2.0.0", "refs/tags/v2.1.0-beta1", "refs/tags/v2.2.0-dev", "refs/tags/archive-1", "refs/tags/vnext",
                                      "refs/tags/2.9.9", "v2.0.0"};
    sh_strv tags = SH_STRV_INIT, out = SH_STRV_INIT;
    for (size_t i = 0; i < sizeof raw / sizeof raw[0]; ++i) sh_strv_push(&tags, raw[i]);
    sh_update_candidates(&tags, running, max, &out);
    char *joined = sh_strv_join(&out, " ");
    sh_strv_free(&tags);
    sh_strv_free(&out);
    return joined;
}

static void test_channels(void) {
    CHECK_STR(candidates_of("2.0.0-beta2", 10), "2.1.0-beta1 2.0.0 2.0.0-beta10");  // a beta follows both; v2.0.0 twice counts once
    CHECK_STR(candidates_of("2.0.0-beta2", 1), "2.1.0-beta1");
    CHECK_STR(candidates_of("1.0.1", 10), "2.0.0");            // a final version never lands on a pre-release
    CHECK_STR(candidates_of("2.0.0", 10), "");                 // nothing newer that is final; and never back to a beta
    CHECK_STR(candidates_of("2.1.0-beta1", 10), "");           // the newest there is
    CHECK_STR(candidates_of("9.0.0", 10), "");                 // never a downgrade
    CHECK_STR(candidates_of("2.0.0-dev", 10), "");             // a build between releases follows nothing
    CHECK_STR(candidates_of("dev", 10), "");
}

static void test_sums(void) {
    static const char sums[] =
        "aaaa  short.exe\n"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  SkillHub-2.0.0-windows-x64.zip\n"
        "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *SkillHub-2.0.0-windows-x64-setup.exe\r\n"
        "1111111111111111111111111111111111111111111111111111111111111111  other/SkillHub-2.0.0.dmg\n";
    char got[65];
    CHECK(sh_update_sum_of(sums, "SkillHub-2.0.0-windows-x64.zip", got));
    CHECK(strcmp(got, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
    CHECK(sh_update_sum_of(sums, "SkillHub-2.0.0-windows-x64-setup.exe", got));
    CHECK(strcmp(got, "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") == 0);
    CHECK(!sh_update_sum_of(sums, "SkillHub-2.0.0.dmg", got));         // a name must match in full
    CHECK(!sh_update_sum_of(sums, "SkillHub-2.0.0-windows", got));
    CHECK(!sh_update_sum_of(sums, "short.exe", got));
    CHECK(!sh_update_sum_of("", "x", got));
}

// RFC 8032, section 7.1, TEST 2 and its key
static const char KEY[] = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const unsigned char MESSAGE[] = {0x72};
static const unsigned char SIGNATURE[64] = {
    0x92, 0xa0, 0x09, 0xa9, 0xf0, 0xd4, 0xca, 0xb8, 0x72, 0x0e, 0x82, 0x0b, 0x5f, 0x64, 0x25, 0x40, 0xa2, 0xb2, 0x7b, 0x54, 0x16, 0x50, 0x3f, 0x8f,
    0xb3, 0x76, 0x22, 0x23, 0xeb, 0xdb, 0x69, 0xda, 0x08, 0x5a, 0xc1, 0xe4, 0x3e, 0x15, 0x99, 0x6e, 0x45, 0x8f, 0x36, 0x13, 0xd0, 0xf1, 0x1d, 0x8c,
    0x38, 0x7b, 0x2e, 0xae, 0xb4, 0x30, 0x2a, 0xee, 0xb0, 0x0d, 0x29, 0x16, 0x12, 0xbb, 0x0c, 0x00};

static void test_signature(void) {
    CHECK(sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 64, KEY));
    const unsigned char other[] = {0x73};
    CHECK(!sh_update_signature_ok(other, 1, SIGNATURE, 64, KEY));      // another text
    unsigned char broken[64];
    memcpy(broken, SIGNATURE, 64);
    broken[10] ^= 1;
    CHECK(!sh_update_signature_ok(MESSAGE, 1, broken, 64, KEY));       // a changed signature
    CHECK(!sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 63, KEY));    // a short one
    CHECK(!sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 64, ""));     // a build without a key trusts nothing
    CHECK(!sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 64, "3d40"));
    CHECK(!sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 64, "zz4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"));
    CHECK(!sh_update_signature_ok(MESSAGE, 1, SIGNATURE, 64, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));  // another key
}

static void test_detect(void) {
    const sh_update_build plain = {"2.0.0", "", false};
#if defined(__linux__)
    CHECK(sh_update_detect(&plain, "/usr/bin/skillhub") == SH_INSTALL_PACKAGE);
    CHECK(sh_update_detect(&plain, "/usr/local/bin/skillhub") == SH_INSTALL_SOURCE);
    CHECK(sh_update_detect(&plain, "/home/me/.local/bin/skillhub") == SH_INSTALL_SOURCE);
#else
    CHECK(sh_update_detect(&plain, "C:/somewhere/skillhub-cli.exe") == SH_INSTALL_SOURCE);
#endif
}

const sh_test_case sh_tests_selfupdate[] = {
    {"selfupdate order", test_order},
    {"selfupdate invalid versions", test_invalid_versions},
    {"selfupdate channels", test_channels},
    {"selfupdate sums", test_sums},
    {"selfupdate signature", test_signature},
    {"selfupdate detect", test_detect},
    {NULL, NULL},
};

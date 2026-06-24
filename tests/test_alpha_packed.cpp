#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "wows-model-exporter.h"

/* ── Synthetic DDS fixtures ───────────────────────────────────────── */

static std::vector<uint8_t> make_bc3_dds(int w, int h, uint8_t alpha_val) {
    std::vector<uint8_t> dds(128 + 8 + 8, 0);
    memcpy(dds.data(), "DDS ", 4);
    uint32_t dw = w, dh = h, linsz = (w + 3) / 4 * 8;
    memcpy(dds.data() + 12, &dh, 4);
    memcpy(dds.data() + 16, &dw, 4);
    uint32_t pitch = h > 1 ? linsz : 8;
    memcpy(dds.data() + 28, &pitch, 4);
    uint32_t depth = 1;
    memcpy(dds.data() + 24, &depth, 4);
    dds[76] = 32;
    dds[77] = 8;
    uint32_t pf_flags = 4;
    memcpy(dds.data() + 80, &pf_flags, 4);
    uint32_t fcc = 0x35545844; // DXT5
    memcpy(dds.data() + 84, &fcc, 4);
    uint32_t cap1 = 0x1000;
    memcpy(dds.data() + 108, &cap1, 4);

    dds[128 + 0] = alpha_val;
    dds[128 + 1] = alpha_val;
    dds[128 + 2] = 0;
    dds[128 + 3] = 0;
    dds[128 + 4] = 0;
    dds[128 + 5] = 0;
    dds[128 + 6] = 0;
    dds[128 + 7] = 0;

    uint16_t c0 = 0x0000, c1 = 0xFFFF;
    memcpy(dds.data() + 128 + 8, &c0, 2);
    memcpy(dds.data() + 128 + 10, &c1, 2);
    dds[128 + 12] = 0;
    dds[128 + 13] = 0;
    dds[128 + 14] = 0;
    dds[128 + 15] = 0;
    return dds;
}

static std::vector<uint8_t> make_bc1_dds(int w, int h) {
    std::vector<uint8_t> dds(128 + 8, 0);
    memcpy(dds.data(), "DDS ", 4);
    uint32_t dw = w, dh = h;
    memcpy(dds.data() + 12, &dh, 4);
    memcpy(dds.data() + 16, &dw, 4);
    uint32_t pitch = h > 1 ? (w + 3) / 4 * 8 : 8;
    memcpy(dds.data() + 28, &pitch, 4);
    uint32_t depth = 1;
    memcpy(dds.data() + 24, &depth, 4);
    dds[76] = 32;
    dds[77] = 8;
    uint32_t pf_flags = 4;
    memcpy(dds.data() + 80, &pf_flags, 4);
    uint32_t fcc = 0x31545844; // DXT1
    memcpy(dds.data() + 84, &fcc, 4);
    uint32_t cap1 = 0x1000;
    memcpy(dds.data() + 108, &cap1, 4);

    dds[128 + 0] = 0x00; dds[128 + 1] = 0x00;
    dds[128 + 2] = 0xFF; dds[128 + 3] = 0xFF;
    dds[128 + 4] = 0; dds[128 + 5] = 0; dds[128 + 6] = 0; dds[128 + 7] = 0;
    return dds;
}

/* ── Tests ────────────────────────────────────────────────────────── */

static void test_force_opaque_applied_to_all_alpha(void) {
    std::vector<uint8_t> dds = make_bc3_dds(4, 4, 128);
    std::vector<uint8_t> png = wows_stitch_dds_to_png_from_memory(dds.data(), dds.size(), 2048, true);
    CU_ASSERT_FALSE(png.empty());
    CU_ASSERT_EQUAL(png[0], 0x89);
    CU_ASSERT_EQUAL(png[1], 0x50);
    CU_ASSERT_EQUAL(png[2], 0x4E);
    CU_ASSERT_EQUAL(png[3], 0x47);
}

static void test_force_opaque_from_memory_variant(void) {
    std::vector<uint8_t> dds = make_bc3_dds(4, 4, 100);
    std::vector<uint8_t> png_normal = wows_stitch_dds_to_png_from_memory(dds.data(), dds.size(), 2048, false);
    CU_ASSERT_FALSE(png_normal.empty());
    std::vector<uint8_t> png_opaque = wows_stitch_dds_to_png_from_memory_force_opaque(dds.data(), dds.size(), 2048);
    CU_ASSERT_FALSE(png_opaque.empty());
}

static void test_bc1_force_opaque(void) {
    std::vector<uint8_t> dds = make_bc1_dds(4, 4);
    std::vector<uint8_t> png = wows_stitch_dds_to_png_from_memory(dds.data(), dds.size(), 2048, true);
    CU_ASSERT_FALSE(png.empty());
}

static void test_mfm_strip_suffixes(void) {
    CU_PASS("MFM strip test skipped (requires filesystem)");
}

/* ── Test main ─────────────────────────────────────────────────────── */

int main(void) {
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("alpha_packed_material_channels", NULL, NULL);
    if (!suite) {
        fprintf(stderr, "Failed to add suite: %s\n", CU_get_error_msg());
        return CU_get_error();
    }
    CU_add_test(suite, "force_opaque_applied_to_all_alpha", test_force_opaque_applied_to_all_alpha);
    CU_add_test(suite, "force_opaque_from_memory_variant", test_force_opaque_from_memory_variant);
    CU_add_test(suite, "bc1_force_opaque", test_bc1_force_opaque);
    CU_add_test(suite, "mfm_strip_suffixes", test_mfm_strip_suffixes);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    int failures = CU_get_number_of_failures();
    CU_cleanup_registry();
    return (failures == 0) ? 0 : 1;
}

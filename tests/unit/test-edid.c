/*
 * EDID generator tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/display/edid.h"

#define EDID_SIZE 256

/* The tag of an 18-byte descriptor, or 0 for a detailed timing. */
static uint8_t desc_tag(const uint8_t *desc)
{
    return desc[0] || desc[1] ? 0 : desc[3];
}

static const uint8_t *base_desc(const uint8_t *edid, int i)
{
    return edid + 54 + 18 * i;
}

/* A descriptor with @tag in the base block, or NULL. */
static const uint8_t *find_base_desc(const uint8_t *edid, uint8_t tag)
{
    for (int i = 0; i < 4; i++) {
        if (desc_tag(base_desc(edid, i)) == tag) {
            return base_desc(edid, i);
        }
    }
    return NULL;
}

/* A descriptor with @tag in the CTA extension, or NULL. */
static const uint8_t *find_cta_desc(const uint8_t *edid, uint8_t tag)
{
    const uint8_t *dta = edid + 128;

    for (int off = dta[2]; off && off + 18 <= 127; off += 18) {
        if (desc_tag(dta + off) == tag) {
            return dta + off;
        }
    }
    return NULL;
}

static void check_checksums(const uint8_t *edid, size_t size)
{
    for (size_t b = 0; b < size / 128 && b <= edid[126]; b++) {
        uint8_t sum = 0;

        for (int i = 0; i < 128; i++) {
            sum += edid[b * 128 + i];
        }
        g_assert_cmpuint(sum, ==, 0);
    }
}

static void generate(uint8_t *edid, size_t size, qemu_edid_info *info)
{
    memset(edid, 0, size);
    qemu_edid_generate(edid, size, info);
    check_checksums(edid, size);
}

/* The 13 data bytes of a text descriptor. */
static void desc_text(const uint8_t *desc, char *text)
{
    g_assert_nonnull(desc);
    memcpy(text, desc + 5, 13);
    text[13] = 0;
}

/* A text descriptor holds 13 characters, a shorter one ends in a line feed. */
static void test_edid_text(void)
{
    uint8_t edid[EDID_SIZE];
    char text[14];
    qemu_edid_info full = { .name = "PiKVM V4 Plus" };
    qemu_edid_info shorter = { .name = "QEMU Monitor" };
    qemu_edid_info longer = { .name = "ABCDEFGHIJKLMN" };

    generate(edid, sizeof(edid), &full);
    desc_text(find_base_desc(edid, 0xfc), text);
    g_assert_cmpstr(text, ==, "PiKVM V4 Plus");

    generate(edid, sizeof(edid), &shorter);
    desc_text(find_base_desc(edid, 0xfc), text);
    g_assert_cmpstr(text, ==, "QEMU Monitor\n");

    generate(edid, sizeof(edid), &longer);
    desc_text(find_base_desc(edid, 0xfc), text);
    g_assert_cmpstr(text, ==, "ABCDEFGHIJKLM");
}

/* Whether a standard timing of the base block is @xres at 60 Hz, 4:3. */
static bool has_std_4_3(const uint8_t *edid, uint32_t xres)
{
    for (int i = 38; i < 54; i += 2) {
        if (edid[i] == xres / 8 - 31 && edid[i + 1] == 0x40) {
            return true;
        }
    }
    return false;
}

/* A mode is offered when it fits the maximum, and one that does not, not. */
static void test_edid_max(void)
{
    uint8_t edid[EDID_SIZE];
    qemu_edid_info info = { .maxx = 1400, .maxy = 1050 };

    generate(edid, sizeof(edid), &info);
    g_assert_true(has_std_4_3(edid, 1400));
    g_assert_true(has_std_4_3(edid, 1280));
    g_assert_false(has_std_4_3(edid, 1600));
}

static void check_base_tags(const uint8_t *edid, const uint8_t tags[4])
{
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(desc_tag(base_desc(edid, i)), ==, tags[i]);
    }
}

/*
 * The monitor's name and serial number stay in the base block; the
 * established timings III (0xf7) give way, unchanged.
 */
static void test_edid_placement(void)
{
    uint8_t edid[384], f7[18];
    const uint8_t *ext_f7;
    qemu_edid_info fhd_serial = {
        .name = "Monitor", .serial_string = "SN1", .prefx = 1920, .prefy = 1080,
    };
    qemu_edid_info fhd = { .name = "Monitor", .prefx = 1920, .prefy = 1080 };
    qemu_edid_info uhd_serial = {
        .name = "Monitor", .serial_string = "SN1", .prefx = 3840, .prefy = 2160,
    };

    /* No serial number: everything fits, the timings III last. */
    generate(edid, EDID_SIZE, &fhd);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xf7 });
    memcpy(f7, base_desc(edid, 3), sizeof(f7));
    /* Revision 10, with the modes the standard timings have no room for */
    g_assert_cmpuint(f7[5], ==, 10);
    g_assert_cmphex(f7[6], ==, 0);
    g_assert_cmphex(f7[7], ==, 0x40);          /* 1280x768 */
    g_assert_cmphex(f7[8], ==, 0x82);          /* 1360x768, 1400x1050 */
    g_assert_cmphex(f7[9], ==, 0);
    g_assert_cmphex(f7[10], ==, 0x28);         /* 1792x1344, 1856x1392 */
    g_assert_cmphex(f7[11], ==, 0x20);         /* 1920x1440 */

    /* With one: the base block is full, the timings III move on. */
    generate(edid, EDID_SIZE, &fhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xff });
    ext_f7 = find_cta_desc(edid, 0xf7);
    g_assert_nonnull(ext_f7);
    g_assert_cmpmem(ext_f7, 18, f7, sizeof(f7));

    /* No extension to move to: the timings III are left out. */
    generate(edid, 128, &fhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xff });

    /* A large screen has no detailed timing in the base block. */
    generate(edid, sizeof(edid), &uhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0xfd, 0xfc, 0xff, 0xf7 });
}

/* Manufacturer, product code and serial number, as given or QEMU's own. */
static void test_edid_id(void)
{
    uint8_t edid[EDID_SIZE];
    qemu_edid_info given = {
        .vendor = "LNX", .product = 0x7773, .serial_number = 0xcafebabe,
    };
    qemu_edid_info own = { .serial_string = "12345" };

    generate(edid, sizeof(edid), &given);
    g_assert_cmphex(lduw_be_p(edid + 8), ==, 0x31d8);          /* LNX */
    g_assert_cmphex(lduw_le_p(edid + 10), ==, 0x7773);
    g_assert_cmphex((uint32_t)ldl_le_p(edid + 12), ==, 0xcafebabe);

    /* The serial number text is not the 32-bit serial number. */
    generate(edid, sizeof(edid), &own);
    g_assert_cmphex(lduw_be_p(edid + 8), ==, 0x4914);          /* RHT */
    g_assert_cmphex(lduw_le_p(edid + 10), ==, 0x1234);
    g_assert_cmphex(ldl_le_p(edid + 12), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/edid/text", test_edid_text);
    g_test_add_func("/edid/max", test_edid_max);
    g_test_add_func("/edid/placement", test_edid_placement);
    g_test_add_func("/edid/id", test_edid_id);

    return g_test_run();
}

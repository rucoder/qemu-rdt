/*
 * EDID generator tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
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
    qemu_edid_info fhd_serial = {
        .name = "Monitor", .serial = "SN1", .prefx = 1920, .prefy = 1080,
    };
    qemu_edid_info fhd = { .name = "Monitor", .prefx = 1920, .prefy = 1080 };
    qemu_edid_info uhd_serial = {
        .name = "Monitor", .serial = "SN1", .prefx = 3840, .prefy = 2160,
    };

    /* No serial number: everything fits, the timings III last. */
    generate(edid, EDID_SIZE, &fhd);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xf7 });
    memcpy(f7, base_desc(edid, 3), sizeof(f7));

    /* With one: the base block is full, the timings III move on. */
    generate(edid, EDID_SIZE, &fhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xff });
    g_assert_nonnull(find_cta_desc(edid, 0xf7));
    g_assert_cmpmem(find_cta_desc(edid, 0xf7), 18, f7, sizeof(f7));

    /* No extension to move to: the timings III are left out. */
    fhd_serial = (qemu_edid_info) {
        .name = "Monitor", .serial = "SN1", .prefx = 1920, .prefy = 1080,
    };
    generate(edid, 128, &fhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0, 0xfd, 0xfc, 0xff });

    /* A large screen has no detailed timing in the base block. */
    generate(edid, sizeof(edid), &uhd_serial);
    check_base_tags(edid, (const uint8_t[]){ 0xfd, 0xfc, 0xff, 0xf7 });
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/edid/text", test_edid_text);
    g_test_add_func("/edid/placement", test_edid_placement);

    return g_test_run();
}

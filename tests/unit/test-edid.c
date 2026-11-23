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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/edid/text", test_edid_text);
    g_test_add_func("/edid/max", test_edid_max);

    return g_test_run();
}

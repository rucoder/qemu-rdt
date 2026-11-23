#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "qemu/dbus.h"
#include "qemu/sockets.h"
#include "glib.h"
#include "glibconfig.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "libqtest.h"
#include "libqos/malloc-pc.h"
#include "libqos/pci-pc.h"
#include "libqos/virtio-pci.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_gpu.h"
#include "ui/dbus-display1.h"

static GDBusConnection*
test_dbus_p2p_from_fd(int fd)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GSocketConnection) socketc = NULL;
    GDBusConnection *conn;

#ifdef WIN32
    socket = g_socket_new_from_fd(_get_osfhandle(fd), &err);
#else
    socket = g_socket_new_from_fd(fd, &err);
#endif
    g_assert_no_error(err);

    socketc = g_socket_connection_factory_create_connection(socket);
    g_assert(socketc != NULL);

    conn = g_dbus_connection_new_sync(
        G_IO_STREAM(socketc), NULL,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
        NULL, NULL, &err);
    g_assert_no_error(err);

    return conn;
}

static void
test_setup_args(QTestState **qts, GDBusConnection **conn, const char *args)
{
    int pair[2];

    *qts = qtest_init(args);

    g_assert_cmpint(qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);

    qtest_qmp_add_client(*qts, "@dbus-display", pair[1]);

    *conn = test_dbus_p2p_from_fd(pair[0]);
    g_dbus_connection_start_message_processing(*conn);
}

static void
test_setup(QTestState **qts, GDBusConnection **conn)
{
    test_setup_args(qts, conn, "-display dbus,p2p=yes -name dbus-test");
}

static void
test_dbus_display_vm(void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) conn = NULL;
    g_autoptr(QemuDBusDisplay1VMProxy) vm = NULL;
    QTestState *qts = NULL;

    test_setup(&qts, &conn);

    vm = QEMU_DBUS_DISPLAY1_VM_PROXY(
        qemu_dbus_display1_vm_proxy_new_sync(
            conn,
            G_DBUS_PROXY_FLAGS_NONE,
            NULL,
            DBUS_DISPLAY1_ROOT "/VM",
            NULL,
            &err));
    g_assert_no_error(err);

    g_assert_cmpstr(
        qemu_dbus_display1_vm_get_name(QEMU_DBUS_DISPLAY1_VM(vm)),
        ==,
        "dbus-test");
    g_clear_object(&conn);
    qtest_quit(qts);
}

typedef struct TestDBusConsoleRegister {
    GMainLoop *loop;
    GThread *thread;
    GDBusConnection *listener_conn;
    GDBusObjectManagerServer *server;
    bool with_map;
    /* Scanout and Update calls received, and the first one after @watch */
    guint messages;
    bool watch;
    const char *first;
    guint first_width;
    guint first_height;
} TestDBusConsoleRegister;

static void test_dbus_console_seen(TestDBusConsoleRegister *test,
                                   const char *method,
                                   guint width, guint height)
{
    test->messages++;
    if (test->watch && !test->first) {
        test->first = method;
        test->first_width = width;
        test->first_height = height;
    }
}

static gboolean listener_handle_scanout(
    QemuDBusDisplay1Listener *object,
    GDBusMethodInvocation *invocation,
    guint arg_width,
    guint arg_height,
    guint arg_stride,
    guint arg_pixman_format,
    GVariant *arg_data,
    TestDBusConsoleRegister *test)
{
    test_dbus_console_seen(test, "Scanout", arg_width, arg_height);
    qemu_dbus_display1_listener_complete_scanout(object, invocation);

    if (!test->with_map) {
        g_main_loop_quit(test->loop);
    }

    return DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean listener_handle_update(
    QemuDBusDisplay1Listener *object,
    GDBusMethodInvocation *invocation,
    gint arg_x,
    gint arg_y,
    gint arg_width,
    gint arg_height,
    guint arg_stride,
    guint arg_pixman_format,
    GVariant *arg_data,
    TestDBusConsoleRegister *test)
{
    test_dbus_console_seen(test, "Update", arg_width, arg_height);
    qemu_dbus_display1_listener_complete_update(object, invocation);

    return DBUS_METHOD_INVOCATION_HANDLED;
}

#ifndef WIN32
static gboolean listener_handle_scanout_map(
    QemuDBusDisplay1ListenerUnixMap *object,
    GDBusMethodInvocation *invocation,
    GUnixFDList *fd_list,
    GVariant *arg_handle,
    guint arg_offset,
    guint arg_width,
    guint arg_height,
    guint arg_stride,
    guint arg_pixman_format,
    TestDBusConsoleRegister *test)
{
    int fd = -1;
    gint32 handle = g_variant_get_handle(arg_handle);
    g_autoptr(GError) error = NULL;
    void *addr = NULL;
    size_t len = arg_height * arg_stride;

    g_assert_cmpuint(g_unix_fd_list_get_length(fd_list), ==, 1);
    fd = g_unix_fd_list_get(fd_list, handle, &error);
    g_assert_no_error(error);

    addr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, arg_offset);
    g_assert_no_errno(addr == MAP_FAILED ? -1 : 0);
    g_assert_no_errno(munmap(addr, len));

    qemu_dbus_display1_listener_unix_map_complete_scanout_map(object, invocation,
                                                              NULL);

    g_main_loop_quit(test->loop);

    close(fd);
    return DBUS_METHOD_INVOCATION_HANDLED;
}
#endif

static void
test_dbus_console_setup_listener(TestDBusConsoleRegister *test, bool with_map)
{
    g_autoptr(GDBusObjectSkeleton) listener = NULL;
    g_autoptr(QemuDBusDisplay1ListenerSkeleton) iface = NULL;

    test->server = g_dbus_object_manager_server_new(DBUS_DISPLAY1_ROOT);
    listener = g_dbus_object_skeleton_new(DBUS_DISPLAY1_ROOT "/Listener");
    iface = QEMU_DBUS_DISPLAY1_LISTENER_SKELETON(
        qemu_dbus_display1_listener_skeleton_new());
    g_object_connect(iface,
                     "signal::handle-scanout", listener_handle_scanout, test,
                     "signal::handle-update", listener_handle_update, test,
                     NULL);
    g_dbus_object_skeleton_add_interface(listener,
                                         G_DBUS_INTERFACE_SKELETON(iface));
    if (with_map) {
#ifdef WIN32
        g_test_skip("map test lacking on win32");
        return;
#else
        g_autoptr(QemuDBusDisplay1ListenerUnixMapSkeleton) iface_map =
            QEMU_DBUS_DISPLAY1_LISTENER_UNIX_MAP_SKELETON(
                qemu_dbus_display1_listener_unix_map_skeleton_new());

        g_object_connect(iface_map,
                         "signal::handle-scanout-map", listener_handle_scanout_map, test,
                         NULL);
        g_dbus_object_skeleton_add_interface(listener,
                                             G_DBUS_INTERFACE_SKELETON(iface_map));
        g_object_set(iface, "interfaces",
            (const gchar *[]) { "org.qemu.Display1.Listener.Unix.Map", NULL },
            NULL);
#endif
    }
    g_dbus_object_manager_server_export(test->server, listener);
    g_dbus_object_manager_server_set_connection(test->server,
                                                test->listener_conn);

    g_dbus_connection_start_message_processing(test->listener_conn);
}

static void
test_dbus_console_registered(GObject *source_object,
                             GAsyncResult *res,
                             gpointer user_data)
{
    TestDBusConsoleRegister *test = user_data;
    g_autoptr(GError) err = NULL;

    qemu_dbus_display1_console_call_register_listener_finish(
        QEMU_DBUS_DISPLAY1_CONSOLE(source_object),
#ifndef WIN32
        NULL,
#endif
        res, &err);

    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
        g_test_skip("The VM doesn't have a console!");
        g_main_loop_quit(test->loop);
        return;
    }

    g_assert_no_error(err);

    test->listener_conn = g_thread_join(test->thread);
    test_dbus_console_setup_listener(test, test->with_map);
}

static gpointer
test_dbus_p2p_server_setup_thread(gpointer data)
{
    return test_dbus_p2p_from_fd(GPOINTER_TO_INT(data));
}

static void
test_dbus_console_register(QTestState *qts, GDBusConnection *conn,
                           TestDBusConsoleRegister *test)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(QemuDBusDisplay1ConsoleProxy) console = NULL;
    int pair[2];
#ifdef WIN32
    WSAPROTOCOL_INFOW info;
    g_autoptr(GVariant) listener = NULL;
#else
    g_autoptr(GUnixFDList) fd_list = NULL;
    int idx;
#endif

    g_assert_cmpint(qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
#ifndef WIN32
    fd_list = g_unix_fd_list_new();
    idx = g_unix_fd_list_append(fd_list, pair[1], NULL);
#endif

    console = QEMU_DBUS_DISPLAY1_CONSOLE_PROXY(
        qemu_dbus_display1_console_proxy_new_sync(
            conn,
            G_DBUS_PROXY_FLAGS_NONE,
            NULL,
            "/org/qemu/Display1/Console_0",
            NULL,
            &err));
    g_assert_no_error(err);

    test->loop = g_main_loop_new(NULL, FALSE);
    test->thread = g_thread_new(NULL, test_dbus_p2p_server_setup_thread,
                                GINT_TO_POINTER(pair[0]));

#ifdef WIN32
    if (WSADuplicateSocketW(_get_osfhandle(pair[1]),
                            GetProcessId((HANDLE) qtest_pid(qts)),
                            &info) == SOCKET_ERROR)
    {
        g_autofree char *emsg = g_win32_error_message(WSAGetLastError());
        g_error("WSADuplicateSocket failed: %s", emsg);
    }
    close(pair[1]);
    listener = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                         &info,
                                         sizeof(info),
                                         1);
#endif

    qemu_dbus_display1_console_call_register_listener(
        QEMU_DBUS_DISPLAY1_CONSOLE(console),
#ifdef WIN32
        listener,
#else
        g_variant_new_handle(idx),
#endif
        G_DBUS_CALL_FLAGS_NONE,
        -1,
#ifndef WIN32
        fd_list,
#endif
        NULL,
        test_dbus_console_registered,
        test);

    g_main_loop_run(test->loop);
}

static void
test_dbus_console_cleanup(TestDBusConsoleRegister *test)
{
    g_clear_object(&test->server);
    g_clear_object(&test->listener_conn);
    g_clear_pointer(&test->loop, g_main_loop_unref);
}

static void
test_dbus_display_console(const void *data)
{
    g_autoptr(GDBusConnection) conn = NULL;
    QTestState *qts = NULL;
    TestDBusConsoleRegister test = { 0, .with_map = GPOINTER_TO_INT(data) };

    test_setup(&qts, &conn);
    test_dbus_console_register(qts, conn, &test);

    test_dbus_console_cleanup(&test);
    g_clear_object(&conn);
    qtest_quit(qts);
}

/* Handle whatever reaches the listener during the next @ms milliseconds. */
static void
test_dbus_console_turn(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * G_TIME_SPAN_MILLISECOND;

    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {
        }
        g_usleep(G_TIME_SPAN_MILLISECOND);
    }
}

/* Wait until the picture is still: no Scanout or Update for 200 ms. */
static void
test_dbus_console_settle(TestDBusConsoleRegister *test)
{
    guint seen;
    int i;

    for (i = 0; i < 25; i++) {
        seen = test->messages;
        test_dbus_console_turn(200);
        if (test->messages == seen) {
            return;
        }
    }
    g_assert_not_reached();
}

static void
vga_crtc_write(QTestState *qts, uint8_t index, uint8_t value)
{
    qtest_outb(qts, 0x3d4, index);
    qtest_outb(qts, 0x3d5, value);
}

static void
test_dbus_display_text_resize(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    QTestState *qts = NULL;
    TestDBusConsoleRegister test = { 0 };
    gint64 end;

    test_setup(&qts, &conn);
    test_dbus_console_register(qts, conn, &test);

    /* VGA text mode, 80x25 cells of 9x16 pixels: 720x400 */
    qtest_outb(qts, 0x3c2, 0x67);       /* CRTC at 0x3d4 */
    vga_crtc_write(qts, 0x01, 0x4f);    /* 80 columns */
    vga_crtc_write(qts, 0x07, 0x02);    /* bit 8 of the vertical display end */
    vga_crtc_write(qts, 0x09, 0x0f);    /* 16 scan lines per cell */
    vga_crtc_write(qts, 0x0e, 0x3f);    /* cursor outside the screen */
    vga_crtc_write(qts, 0x0f, 0xff);
    vga_crtc_write(qts, 0x12, 0x8f);    /* 400 scan lines */
    vga_crtc_write(qts, 0x13, 0x28);    /* 80 characters per row */
    qtest_inb(qts, 0x3da);              /* attribute controller to index */
    qtest_outb(qts, 0x3c0, 0x20);       /* display on */
    test_dbus_console_settle(&test);

    /*
     * Cells of 9x12 pixels, as the Windows NT 4.0 setup sets: 720x396.
     * VGA redraws a text mode one row of cells at a time, and the client
     * has to know the new size before the first row arrives.
     */
    test.watch = true;
    vga_crtc_write(qts, 0x09, 0x0b);
    end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!test.first && g_get_monotonic_time() < end) {
        test_dbus_console_turn(10);
    }
    g_assert_cmpstr(test.first, ==, "Scanout");
    g_assert_cmpuint(test.first_width, ==, 720);
    g_assert_cmpuint(test.first_height, ==, 396);

    test_dbus_console_cleanup(&test);
    g_clear_object(&conn);
    qtest_quit(qts);
}

static void
test_dbus_display_keyboard(void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) conn = NULL;
    g_autoptr(QemuDBusDisplay1KeyboardProxy) keyboard = NULL;
    QTestState *qts = NULL;

    test_setup(&qts, &conn);

    keyboard = QEMU_DBUS_DISPLAY1_KEYBOARD_PROXY(
        qemu_dbus_display1_keyboard_proxy_new_sync(
            conn,
            G_DBUS_PROXY_FLAGS_NONE,
            NULL,
            "/org/qemu/Display1/Console_0",
            NULL,
            &err));
    g_assert_no_error(err);

    g_assert_cmpint(qtest_inb(qts, 0x64) & 0x1, ==, 0);
    g_assert_cmpint(qtest_inb(qts, 0x60), ==, 0);

    qemu_dbus_display1_keyboard_call_press_sync(
        QEMU_DBUS_DISPLAY1_KEYBOARD(keyboard),
        0x1C, /* qnum enter */
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &err);
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
        g_test_skip("The VM doesn't have a console!");
        g_clear_object(&conn);
        qtest_quit(qts);
        return;
    }

    g_assert_no_error(err);

    /* may be should wait for interrupt? */
    g_assert_cmpint(qtest_inb(qts, 0x64) & 0x1, ==, 1);
    g_assert_cmpint(qtest_inb(qts, 0x60), ==, 0x5A); /* scan code 2 enter */

    qemu_dbus_display1_keyboard_call_release_sync(
        QEMU_DBUS_DISPLAY1_KEYBOARD(keyboard),
        0x1C, /* qnum enter */
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &err);
    g_assert_no_error(err);

    g_assert_cmpint(qtest_inb(qts, 0x64) & 0x1, ==, 1);
    g_assert_cmpint(qtest_inb(qts, 0x60), ==, 0xF0); /* scan code 2 release */
    g_assert_cmpint(qtest_inb(qts, 0x60), ==, 0x5A); /* scan code 2 enter */

    g_assert_cmpint(qemu_dbus_display1_keyboard_get_modifiers(
                        QEMU_DBUS_DISPLAY1_KEYBOARD(keyboard)), ==, 0);

    g_clear_object(&conn);
    qtest_quit(qts);
}

/* A machine with @device, a display device, on the D-Bus display. */
static void
test_setup_device(QTestState **qts, GDBusConnection **conn, const char *device)
{
    g_autofree char *args = g_strdup_printf(
        "-vga none -device %s -display dbus,p2p=yes -name dbus-test", device);

    test_setup_args(qts, conn, args);
}

/* The UIInfo of console @index. */
static QemuDBusDisplay1UIInfo *
test_uiinfo_proxy(GDBusConnection *conn, int index)
{
    g_autoptr(GError) err = NULL;
    g_autofree char *path = g_strdup_printf(DBUS_DISPLAY1_ROOT "/Console_%d",
                                            index);
    QemuDBusDisplay1UIInfo *ui_info;

    ui_info = QEMU_DBUS_DISPLAY1_UIINFO(
        qemu_dbus_display1_uiinfo_proxy_new_sync(
            conn, G_DBUS_PROXY_FLAGS_NONE, NULL, path, NULL, &err));
    g_assert_no_error(err);
    return ui_info;
}

static GVariant *
test_uiinfo_get(QemuDBusDisplay1UIInfo *ui_info)
{
    g_autoptr(GError) err = NULL;
    GVariant *info = NULL;

    qemu_dbus_display1_uiinfo_call_get_sync(ui_info, G_DBUS_CALL_FLAGS_NONE,
                                            -1, &info, NULL, &err);
    g_assert_no_error(err);
    return info;
}

static guint32
test_uiinfo_get_u(QemuDBusDisplay1UIInfo *ui_info, const char *key)
{
    g_autoptr(GVariant) info = test_uiinfo_get(ui_info);
    guint32 value = 0;

    g_assert_true(g_variant_lookup(info, key, "u", &value));
    return value;
}

/*
 * A device's output is not off until a UI says so: UIInfo starts from the
 * size the virtio-gpu output is created with, not from a zero size.
 */
static void
test_dbus_display_uiinfo_initial(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    QemuDBusDisplay1UIInfo *ui_info;
    QTestState *qts = NULL;

    test_setup_device(&qts, &conn, "virtio-gpu-pci,xres=1024,yres=768");
    ui_info = test_uiinfo_proxy(conn, 0);

    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 1024);
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "height"), ==, 768);

    g_clear_object(&ui_info);
    g_clear_object(&conn);
    qtest_quit(qts);
}

/* UIInfo.Apply: NULL, or the name of the D-Bus error it failed with. */
static char *
test_uiinfo_apply(QemuDBusDisplay1UIInfo *ui_info, GVariant *dict)
{
    g_autoptr(GError) err = NULL;

    qemu_dbus_display1_uiinfo_call_apply_sync(ui_info, dict,
                                              G_DBUS_CALL_FLAGS_NONE, -1,
                                              NULL, &err);
    return err ? g_dbus_error_get_remote_error(err) : NULL;
}

static void
test_uiinfo_apply_ok(QemuDBusDisplay1UIInfo *ui_info, GVariant *dict)
{
    g_autofree char *error = test_uiinfo_apply(ui_info, dict);

    g_assert_cmpstr(error, ==, NULL);
}

static void
test_uiinfo_apply_invalid(QemuDBusDisplay1UIInfo *ui_info, GVariant *dict)
{
    g_autofree char *error = test_uiinfo_apply(ui_info, dict);

    g_assert_cmpstr(error, ==, "org.qemu.Display1.Error.Invalid");
}

static void
test_uiinfo_assert_s(QemuDBusDisplay1UIInfo *ui_info, const char *key,
                     const char *expected)
{
    g_autoptr(GVariant) info = test_uiinfo_get(ui_info);
    const char *s = NULL;

    g_assert_true(g_variant_lookup(info, key, "&s", &s));
    g_assert_cmpstr(s, ==, expected);
}

/* Whether a monitor shows the console, as a client tells from UIInfo.Get. */
static gboolean
test_uiinfo_get_connected(QemuDBusDisplay1UIInfo *ui_info)
{
    g_autoptr(GVariant) info = test_uiinfo_get(ui_info);
    guint32 width = 0, height = 0;
    gboolean connected;

    if (g_variant_lookup(info, "connected", "b", &connected)) {
        /* Given only where the size does not say it */
        g_assert_false(connected);
        return false;
    }
    g_assert_true(g_variant_lookup(info, "width", "u", &width));
    g_assert_true(g_variant_lookup(info, "height", "u", &height));
    return width && height;
}
#if defined(CONFIG_VNC) && !defined(WIN32)
static void
test_vnc_read(int fd, void *buf, size_t len)
{
    while (len) {
        ssize_t n = read(fd, buf, len);

        g_assert_cmpint(n, >, 0);
        buf = (char *)buf + n;
        len -= n;
    }
}

static void
test_vnc_write(int fd, const void *buf, size_t len)
{
    g_assert_cmpint(qemu_write_full(fd, buf, len), ==, len);
}

/* A VNC client of @qts, past the handshake, without authentication. */
static int
test_vnc_connect(QTestState *qts)
{
    char version[12];
    uint8_t types[256], n, shared = 1, server_init[24];
    uint32_t result;
    g_autofree char *name = NULL;
    int pair[2];

    g_assert_cmpint(qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    g_assert_cmpint(setsockopt(pair[0], SOL_SOCKET, SO_RCVTIMEO,
                               &(struct timeval) { .tv_sec = 10 },
                               sizeof(struct timeval)), ==, 0);
    qtest_qmp_add_client(qts, "vnc", pair[1]);
    close(pair[1]);

    test_vnc_read(pair[0], version, sizeof(version));
    g_assert_cmpmem(version, sizeof(version), "RFB 003.008\n", 12);
    test_vnc_write(pair[0], version, sizeof(version));
    test_vnc_read(pair[0], &n, 1);
    test_vnc_read(pair[0], types, n);
    g_assert_nonnull(memchr(types, 1, n));          /* None */
    test_vnc_write(pair[0], (uint8_t[]){ 1 }, 1);
    test_vnc_read(pair[0], &result, sizeof(result));
    g_assert_cmpuint(result, ==, 0);
    test_vnc_write(pair[0], &shared, 1);
    test_vnc_read(pair[0], server_init, sizeof(server_init));
    name = g_malloc(ldl_be_p(server_init + 20));
    test_vnc_read(pair[0], name, ldl_be_p(server_init + 20));
    return pair[0];
}

/* SetDesktopSize: one screen of @width x @height. */
static void
test_vnc_set_desktop_size(int fd, uint16_t width, uint16_t height)
{
    uint8_t msg[24] = { 251 };

    stw_be_p(msg + 2, width);
    stw_be_p(msg + 4, height);
    msg[6] = 1;                     /* one screen, at 0,0 */
    stw_be_p(msg + 8 + 8, width);
    stw_be_p(msg + 8 + 10, height);
    test_vnc_write(fd, msg, sizeof(msg));
}

/*
 * A VNC client that resizes the console changes the size, and leaves what
 * the D-Bus client gave as it was.
 */
static void
test_dbus_display_uiinfo_vnc(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    QemuDBusDisplay1UIInfo *ui_info;
    QTestState *qts = NULL;
    gint64 end;
    int fd;

    test_setup_args(&qts, &conn, "-vga none -device virtio-gpu-pci -vnc none"
                    " -display dbus,p2p=yes -name dbus-test");
    ui_info = test_uiinfo_proxy(conn, 0);
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'refresh_rate': <uint32 60000>, 'name': <'Name'>}"));

    fd = test_vnc_connect(qts);
    test_vnc_set_desktop_size(fd, 1600, 900);
    end = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (test_uiinfo_get_u(ui_info, "width") != 1600) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "height"), ==, 900);
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "refresh_rate"), ==, 60000);
    test_uiinfo_assert_s(ui_info, "name", "Name");

    close(fd);
    g_clear_object(&ui_info);
    g_clear_object(&conn);
    qtest_quit(qts);
}
#endif

static void
test_console_set_ui_info(QemuDBusDisplay1Console *console,
                         guint width, guint height)
{
    g_autoptr(GError) err = NULL;

    qemu_dbus_display1_console_call_set_uiinfo_sync(
        console, 300, 170, 0, 0, width, height,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    g_assert_no_error(err);
}

/* Console.SetUIInfo leaves what it does not take as UIInfo.Apply set it. */
static void
test_dbus_display_uiinfo_legacy(void)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(GDBusConnection) conn = NULL;
    QemuDBusDisplay1UIInfo *ui_info;
    QemuDBusDisplay1Console *console;
    QTestState *qts = NULL;

    test_setup_device(&qts, &conn, "virtio-gpu-pci");
    ui_info = test_uiinfo_proxy(conn, 0);
    console = QEMU_DBUS_DISPLAY1_CONSOLE(
        qemu_dbus_display1_console_proxy_new_sync(
            conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
            DBUS_DISPLAY1_ROOT "/Console_0", NULL, &err));
    g_assert_no_error(err);

    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'refresh_rate': <uint32 60000>, 'name': <'Name'>,"
        " 'serial_number': <uint32 7>}"));
    test_console_set_ui_info(console, 1600, 900);
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 1600);
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "refresh_rate"), ==, 60000);
    test_uiinfo_assert_s(ui_info, "name", "Name");
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "serial_number"), ==, 7);

    /*
     * A zero size takes the monitor away, and a size brings it back, also
     * after UIInfo.Apply took it away.
     */
    test_console_set_ui_info(console, 0, 0);
    g_assert_false(test_uiinfo_get_connected(ui_info));
    test_console_set_ui_info(console, 1600, 900);
    g_assert_true(test_uiinfo_get_connected(ui_info));
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'connected': <false>}"));
    test_console_set_ui_info(console, 1280, 720);
    g_assert_true(test_uiinfo_get_connected(ui_info));

    g_clear_object(&ui_info);
    g_clear_object(&console);
    g_clear_object(&conn);
    qtest_quit(qts);
}

/* @key set to @value, with a new size, is refused, and changes nothing. */
static void
test_uiinfo_refused(QemuDBusDisplay1UIInfo *ui_info,
                    const char *key, GVariant *value)
{
    g_autoptr(GVariant) before = test_uiinfo_get(ui_info);
    g_autoptr(GVariant) after = NULL;

    test_uiinfo_apply_invalid(ui_info, g_variant_new_parsed(
        "{'width': <uint32 640>, 'height': <uint32 480>, %s: %v}",
        key, value));
    after = test_uiinfo_get(ui_info);
    g_assert_true(g_variant_equal(before, after));
}

/* The monitor's identity: given, read back, checked, and cleared. */
static void
test_dbus_display_uiinfo_identity(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    g_autoptr(GVariant) info = NULL;
    QemuDBusDisplay1UIInfo *ui_info;
    QTestState *qts = NULL;
    guint16 product = 0;

    test_setup_device(&qts, &conn, "virtio-gpu-pci");
    ui_info = test_uiinfo_proxy(conn, 0);

    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'vendor': <'LNX'>, 'product': <%q>, 'serial_number': <%u>,"
        " 'name': <'PiKVM V4 Plus'>, 'serial_string': <'CAFEBABE'>}",
        0x7773, 0xcafebabe));
    info = test_uiinfo_get(ui_info);
    g_assert_true(g_variant_lookup(info, "product", "q", &product));
    g_assert_cmphex(product, ==, 0x7773);
    g_assert_cmphex(test_uiinfo_get_u(ui_info, "serial_number"), ==,
                    0xcafebabe);
    test_uiinfo_assert_s(ui_info, "vendor", "LNX");
    test_uiinfo_assert_s(ui_info, "name", "PiKVM V4 Plus");
    test_uiinfo_assert_s(ui_info, "serial_string", "CAFEBABE");

    test_uiinfo_refused(ui_info, "vendor", g_variant_new_string("lnx"));
    test_uiinfo_refused(ui_info, "vendor", g_variant_new_string("LN"));
    test_uiinfo_refused(ui_info, "vendor", g_variant_new_string("LNXX"));
    test_uiinfo_refused(ui_info, "vendor", g_variant_new_string("L1X"));
    test_uiinfo_refused(ui_info, "name",
                        g_variant_new_string("ABCDEFGHIJKLMN"));
    test_uiinfo_refused(ui_info, "name", g_variant_new_string("Tab\there"));
    test_uiinfo_refused(ui_info, "name",
                        g_variant_new_string("M\xc3\xb6nitor"));
    test_uiinfo_refused(ui_info, "serial_string",
                        g_variant_new_string("12345678901234"));
    test_uiinfo_refused(ui_info, "name", g_variant_new_uint32(1));
    test_uiinfo_refused(ui_info, "product", g_variant_new_string("1"));

    /* 13 characters fit. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'serial_string': <'1234567890123'>}"));
    test_uiinfo_assert_s(ui_info, "serial_string", "1234567890123");

    /* An empty string clears a text, and a key of no use is ignored. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'name': <''>, 'vendor': <''>, 'no-such-key': <uint32 1>}"));
    test_uiinfo_assert_s(ui_info, "name", "");
    test_uiinfo_assert_s(ui_info, "vendor", "");

    g_clear_object(&ui_info);
    g_clear_object(&conn);
    qtest_quit(qts);
}

/* Whether a monitor shows the console, said or taken from the size. */
static void
test_dbus_display_uiinfo_connected(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    g_autoptr(GVariant) info = NULL;
    g_autoptr(GVariant) after = NULL;
    QemuDBusDisplay1UIInfo *ui_info;
    QTestState *qts = NULL;
    GVariantDict dict;

    test_setup_device(&qts, &conn, "virtio-gpu-pci,xres=1024,yres=768");
    ui_info = test_uiinfo_proxy(conn, 0);
    g_assert_true(test_uiinfo_get_connected(ui_info));

    /* Disconnected, the size stays for the monitor to come back with. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'connected': <false>}"));
    g_assert_false(test_uiinfo_get_connected(ui_info));
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 1024);

    /* Neither the key nor a size: still disconnected. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'name': <'Name'>}"));
    g_assert_false(test_uiinfo_get_connected(ui_info));

    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'connected': <true>}"));
    g_assert_true(test_uiinfo_get_connected(ui_info));
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 1024);

    /* No key: a zero size is no monitor, as for older clients. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 0>, 'height': <uint32 1>}"));
    g_assert_false(test_uiinfo_get_connected(ui_info));
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 800>, 'height': <uint32 600>}"));
    g_assert_true(test_uiinfo_get_connected(ui_info));

    /* One size key alone: the other keeps its value. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 0>}"));
    g_assert_false(test_uiinfo_get_connected(ui_info));
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 640>}"));
    g_assert_true(test_uiinfo_get_connected(ui_info));
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "height"), ==, 600);

    /* A connected monitor has a size, and "connected" is a boolean. */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 0>}"));
    test_uiinfo_apply_invalid(ui_info, g_variant_new_parsed(
        "{'connected': <true>}"));
    test_uiinfo_apply_invalid(ui_info, g_variant_new_parsed(
        "{'connected': <true>, 'width': <uint32 0>, 'height': <uint32 1>}"));
    test_uiinfo_apply_invalid(ui_info, g_variant_new_parsed(
        "{'connected': <uint32 1>, 'width': <uint32 800>}"));
    g_assert_false(test_uiinfo_get_connected(ui_info));
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 0);

    /*
     * What UIInfo.Get returns, given back to UIInfo.Apply, changes nothing,
     * and with a new size means what that size means, unless a client took
     * the monitor away with "connected".
     */
    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'width': <uint32 1024>, 'height': <uint32 768>}"));
    info = test_uiinfo_get(ui_info);
    test_uiinfo_apply_ok(ui_info, info);
    after = test_uiinfo_get(ui_info);
    g_assert_true(g_variant_equal(info, after));
    g_assert_true(test_uiinfo_get_connected(ui_info));

    g_variant_dict_init(&dict, info);
    g_variant_dict_insert(&dict, "width", "u", 0);
    g_variant_dict_insert(&dict, "height", "u", 0);
    test_uiinfo_apply_ok(ui_info, g_variant_dict_end(&dict));
    g_assert_false(test_uiinfo_get_connected(ui_info));

    g_clear_pointer(&info, g_variant_unref);
    info = test_uiinfo_get(ui_info);
    g_variant_dict_init(&dict, info);
    g_variant_dict_insert(&dict, "width", "u", 800);
    g_variant_dict_insert(&dict, "height", "u", 600);
    test_uiinfo_apply_ok(ui_info, g_variant_dict_end(&dict));
    g_assert_true(test_uiinfo_get_connected(ui_info));

    test_uiinfo_apply_ok(ui_info, g_variant_new_parsed(
        "{'connected': <false>}"));
    g_clear_pointer(&info, g_variant_unref);
    info = test_uiinfo_get(ui_info);
    test_uiinfo_apply_ok(ui_info, info);
    g_clear_pointer(&after, g_variant_unref);
    after = test_uiinfo_get(ui_info);
    g_assert_true(g_variant_equal(info, after));
    g_assert_false(test_uiinfo_get_connected(ui_info));

    /*
     * Such a dictionary, given back with a new size, keeps the monitor
     * away: "connected" in it says so.
     */
    g_variant_dict_init(&dict, info);
    g_variant_dict_insert(&dict, "width", "u", 640);
    test_uiinfo_apply_ok(ui_info, g_variant_dict_end(&dict));
    g_assert_false(test_uiinfo_get_connected(ui_info));
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "width"), ==, 640);

    g_clear_object(&ui_info);
    g_clear_object(&conn);
    qtest_quit(qts);
}

/*
 * A virtio-gpu driven from the test, as a guest would: what it reports for
 * its outputs, and when, after a UI changes a monitor through D-Bus.
 */

#define GPU_TIMEOUT_US (10 * G_TIME_SPAN_SECOND)
#define GPU_OUTPUTS 2

typedef struct TestGPU {
    QTestState *qts;
    GDBusConnection *conn;
    QemuDBusDisplay1UIInfo *ui_info[GPU_OUTPUTS];
    int outputs;
    QGuestAllocator alloc;
    QPCIBus *pcibus;
    QVirtioPCIDevice *dev;
    QVirtQueue *ctrlq;
} TestGPU;

typedef struct TestGPUOutput {
    uint32_t width, height;
} TestGPUOutput;

/*
 * What a driver does to start the device. The config change has an MSI-X
 * vector of its own, so that the test sees every interrupt for it, as a
 * guest does.
 */
static void
test_gpu_start_driver(TestGPU *t)
{
    uint64_t features;

    qvirtio_start_device(&t->dev->vdev);
    qvirtio_pci_set_msix_configuration_vector(t->dev, &t->alloc, 0);
    features = qvirtio_get_features(&t->dev->vdev) &
               ((1ull << VIRTIO_F_VERSION_1) | (1ull << VIRTIO_GPU_F_EDID));
    qvirtio_set_features(&t->dev->vdev, features);
    t->ctrlq = qvirtqueue_setup(&t->dev->vdev, &t->alloc, 0);
    qvirtqueue_pci_msix_setup(t->dev, (QVirtQueuePCI *)t->ctrlq, &t->alloc, 1);
    qvirtio_set_driver_ok(&t->dev->vdev);
}

/* Start a guest with @device, a virtio-gpu at 04.0 with @outputs outputs. */
static void
test_gpu_start_device(TestGPU *t, const char *device, int outputs)
{
    QPCIAddress addr = { .devfn = QPCI_DEVFN(4, 0) };

    test_setup_device(&t->qts, &t->conn, device);
    t->outputs = outputs;
    for (int i = 0; i < outputs; i++) {
        t->ui_info[i] = test_uiinfo_proxy(t->conn, i);
    }

    pc_alloc_init(&t->alloc, t->qts, 0);
    t->pcibus = qpci_new_pc(t->qts, &t->alloc);
    t->dev = virtio_pci_new(t->pcibus, &addr);
    g_assert_nonnull(t->dev);
    qvirtio_pci_device_enable(t->dev);
    qpci_msix_enable(t->dev->pdev);
    test_gpu_start_driver(t);
}

static void
test_gpu_start(TestGPU *t)
{
    test_gpu_start_device(t, "virtio-gpu-pci,addr=04.0", 1);
}

/* A driver that stops and starts again, resetting the device. */
static void
test_gpu_restart_driver(TestGPU *t)
{
    qvirtqueue_cleanup(t->dev->vdev.bus, t->ctrlq, &t->alloc);
    test_gpu_start_driver(t);
}

static void
test_gpu_stop(TestGPU *t)
{
    qvirtqueue_cleanup(t->dev->vdev.bus, t->ctrlq, &t->alloc);
    qpci_msix_disable(t->dev->pdev);
    qos_object_destroy((QOSGraphObject *)t->dev);
    qpci_free_pc(t->pcibus);
    alloc_destroy(&t->alloc);
    for (int i = 0; i < t->outputs; i++) {
        g_clear_object(&t->ui_info[i]);
    }
    g_clear_object(&t->conn);
    qtest_quit(t->qts);
}

static void
test_gpu_cmd(TestGPU *t, const void *req, size_t req_len,
             void *resp, size_t resp_len)
{
    uint64_t req_addr = guest_alloc(&t->alloc, req_len);
    uint64_t resp_addr = guest_alloc(&t->alloc, resp_len);
    uint32_t head;

    qtest_memwrite(t->qts, req_addr, req, req_len);
    head = qvirtqueue_add(t->qts, t->ctrlq, req_addr, req_len, false, true);
    qvirtqueue_add(t->qts, t->ctrlq, resp_addr, resp_len, true, false);
    qvirtqueue_kick(t->qts, &t->dev->vdev, t->ctrlq, head);
    qvirtio_wait_used_elem(t->qts, &t->dev->vdev, t->ctrlq, head, NULL,
                           GPU_TIMEOUT_US);
    qtest_memread(t->qts, resp_addr, resp, resp_len);
    guest_free(&t->alloc, req_addr);
    guest_free(&t->alloc, resp_addr);
    /* The command is done, and none is in flight: reuse its descriptors. */
    qvirtqueue_reset_pool(t->ctrlq);
}

/* GET_DISPLAY_INFO: the size of each output, 0x0 for one not enabled. */
static void
test_gpu_display_info(TestGPU *t, TestGPUOutput out[GPU_OUTPUTS])
{
    struct virtio_gpu_ctrl_hdr req = {
        .type = cpu_to_le32(VIRTIO_GPU_CMD_GET_DISPLAY_INFO),
    };
    struct virtio_gpu_resp_display_info resp = {};

    test_gpu_cmd(t, &req, sizeof(req), &resp, sizeof(resp));
    g_assert_cmphex(le32_to_cpu(resp.hdr.type), ==,
                    VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    for (int i = 0; i < GPU_OUTPUTS; i++) {
        bool enabled = le32_to_cpu(resp.pmodes[i].enabled);

        out[i].width = enabled ? le32_to_cpu(resp.pmodes[i].r.width) : 0;
        out[i].height = enabled ? le32_to_cpu(resp.pmodes[i].r.height) : 0;
    }
}

/* GET_DISPLAY_INFO: output 0's width, or 0 when it is not enabled. */
static uint32_t
test_gpu_width(TestGPU *t)
{
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_display_info(t, out);
    return out[0].width;
}

/*
 * GET_EDID for output @scanout. Unlike the display info, the EDID follows
 * every UI change, whether the output is enabled or held back, and reading
 * it is not what tells the device the guest has seen the display info.
 */
static void
test_gpu_edid(TestGPU *t, int scanout, struct virtio_gpu_resp_edid *resp)
{
    struct virtio_gpu_cmd_get_edid req = {
        .hdr.type = cpu_to_le32(VIRTIO_GPU_CMD_GET_EDID),
        .scanout = cpu_to_le32(scanout),
    };

    test_gpu_cmd(t, &req, sizeof(req), resp, sizeof(*resp));
    g_assert_cmphex(le32_to_cpu(resp->hdr.type), ==, VIRTIO_GPU_RESP_OK_EDID);
}

/* The width of the preferred mode in the EDID of output @scanout. */
static uint32_t
test_gpu_edid_width(TestGPU *t, int scanout)
{
    struct virtio_gpu_resp_edid resp = {};

    test_gpu_edid(t, scanout, &resp);
    /* The first detailed timing: horizontal active pixels. */
    g_assert_true(resp.edid[54] || resp.edid[55]);
    return resp.edid[56] | ((resp.edid[58] & 0xf0) << 4);
}

static bool
test_gpu_display_event(TestGPU *t)
{
    return qvirtio_config_readl(&t->dev->vdev,
               offsetof(struct virtio_gpu_config, events_read)) &
           VIRTIO_GPU_EVENT_DISPLAY;
}

/* Whether the guest was interrupted for a config change since last asked. */
static bool
test_gpu_config_irq(TestGPU *t)
{
    return qpci_msix_test_interrupt(t->dev->pdev, t->dev->config_msix_entry,
                                    t->dev->config_msix_addr,
                                    t->dev->config_msix_data);
}

/* A display event is raised, and the guest was interrupted for it. */
static void
test_gpu_assert_raised(TestGPU *t)
{
    g_assert_true(test_gpu_display_event(t));
    g_assert_true(test_gpu_config_irq(t));
}

/* No display event, and no interrupt for one. */
static void
test_gpu_assert_quiet(TestGPU *t)
{
    g_assert_false(test_gpu_display_event(t));
    g_assert_false(test_gpu_config_irq(t));
}

/* virtio-gpu-pci is modern only: its config space is the device's own. */
static void
test_gpu_clear_display_event(TestGPU *t)
{
    qpci_io_writel(t->dev->pdev, t->dev->bar, t->dev->device_cfg_offset +
                   offsetof(struct virtio_gpu_config, events_clear),
                   VIRTIO_GPU_EVENT_DISPLAY);
}

static void
test_gpu_apply(TestGPU *t, int out, uint32_t width, uint32_t height)
{
    test_uiinfo_apply_ok(t->ui_info[out], g_variant_new_parsed(
        "{'width': <%u>, 'height': <%u>}", width, height));
}

/*
 * The console passes UI info on from a timer: wait for the device to act,
 * which it tells the guest with an interrupt.
 */
static void
test_gpu_wait_display_event(TestGPU *t)
{
    gint64 end = g_get_monotonic_time() + GPU_TIMEOUT_US;

    while (!test_gpu_config_irq(t)) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_assert_true(test_gpu_display_event(t));
}

static void
test_gpu_wait_edid_width(TestGPU *t, int scanout, uint32_t width)
{
    gint64 end = g_get_monotonic_time() + GPU_TIMEOUT_US;

    while (test_gpu_edid_width(t, scanout) != width) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
}

/* An output configured with no size is refused. */
static void
test_dbus_display_gpu_no_size(void)
{
    if (g_test_subprocess()) {
        qtest_quit(qtest_init("-vga none -device virtio-gpu-pci,xres=0"));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*output 0 has a zero xres or yres*");
}

/* A UI's first request, for no monitor, reaches the device. */
static void
test_dbus_display_gpu_first_off(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    test_gpu_assert_quiet(&t);
    test_gpu_apply(&t, 0, 0, 0);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 0);

    test_gpu_stop(&t);
}

/*
 * A change between the guest's read of the display info and its clear of
 * the event is not cleared with it, and an event cleared twice without a
 * read in between is raised again only once.
 */
static void
test_dbus_display_gpu_event_clear(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    test_gpu_width(&t);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1600);

    /* A change after the read, before the clear. */
    test_gpu_apply(&t, 0, 1280, 720);
    test_gpu_wait_edid_width(&t, 0, 1280);
    g_assert_true(test_gpu_config_irq(&t));
    test_gpu_clear_display_event(&t);
    test_gpu_assert_raised(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    /* Cleared twice without a read: raised again once, not twice. */
    test_gpu_apply(&t, 0, 1024, 768);
    test_gpu_wait_display_event(&t);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_raised(&t);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_gpu_stop(&t);
}

/*
 * Output 0, which the guest has seen, was given a 1600x900 monitor after
 * a disconnect, both before the guest read the display info again: the
 * guest sees the output go, then come back with an event of its own,
 * whether it reads the display info before clearing the event (viogpudo,
 * Linux) or after (older Linux).
 */
static void
test_gpu_swapped(TestGPU *t, bool clear_first)
{
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_wait_edid_width(t, 0, 1600);
    test_gpu_assert_raised(t);

    if (clear_first) {
        test_gpu_clear_display_event(t);
        test_gpu_assert_raised(t);
        g_assert_cmpuint(test_gpu_width(t), ==, 0);
        /* The read gives the output back, with an event of its own. */
        test_gpu_assert_raised(t);
    } else {
        g_assert_cmpuint(test_gpu_width(t), ==, 0);
        g_assert_true(test_gpu_config_irq(t));
        test_gpu_clear_display_event(t);
        test_gpu_assert_raised(t);
    }

    test_gpu_display_info(t, out);
    g_assert_cmpuint(out[0].width, ==, 1600);
    g_assert_cmpuint(out[0].height, ==, 900);
    test_gpu_clear_display_event(t);
    test_gpu_assert_quiet(t);
}

/* A monitor swapped before the guest looked, one request at a time. */
static void
test_dbus_display_gpu_swap(const void *data)
{
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply(&t, 0, 0, 0);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_swapped(&t, GPOINTER_TO_INT(data));

    test_gpu_stop(&t);
}

#ifndef WIN32
static void
test_gpu_applied(GObject *source, GAsyncResult *res, gpointer data)
{
    g_autoptr(GError) err = NULL;
    int *pending = data;

    qemu_dbus_display1_uiinfo_call_apply_finish(
        QEMU_DBUS_DISPLAY1_UIINFO(source), res, &err);
    g_assert_no_error(err);
    (*pending)--;
}

/*
 * UIInfo.Apply of @first and @second on output 0, reaching the console in
 * one turn of QEMU's main loop, before its timer can pass the first on.
 * Requests sent one after the other come in two turns. QEMU sets up the
 * connection of a new listener in its main loop, after answering the
 * registration, and blocks there until the test's side is up: the
 * requests sent meanwhile are handled together once it is.
 */
static void
test_gpu_apply_merged(TestGPU *t, GVariant *first, GVariant *second)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(QemuDBusDisplay1ConsoleProxy) console = NULL;
    g_autoptr(GUnixFDList) fd_list = NULL;
    TestDBusConsoleRegister listener = { 0 };
    int pair[2], idx, pending = 2;

    g_assert_cmpint(qemu_socketpair(AF_UNIX, SOCK_STREAM, 0, pair), ==, 0);
    fd_list = g_unix_fd_list_new();
    idx = g_unix_fd_list_append(fd_list, pair[1], NULL);
    close(pair[1]);
    console = QEMU_DBUS_DISPLAY1_CONSOLE_PROXY(
        qemu_dbus_display1_console_proxy_new_sync(
            t->conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
            DBUS_DISPLAY1_ROOT "/Console_0", NULL, &err));
    g_assert_no_error(err);
    qemu_dbus_display1_console_call_register_listener_sync(
        QEMU_DBUS_DISPLAY1_CONSOLE(console), g_variant_new_handle(idx),
        G_DBUS_CALL_FLAGS_NONE, -1, fd_list, NULL, NULL, &err);
    g_assert_no_error(err);

    qemu_dbus_display1_uiinfo_call_apply(t->ui_info[0], first,
                                         G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                         test_gpu_applied, &pending);
    qemu_dbus_display1_uiinfo_call_apply(t->ui_info[0], second,
                                         G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                         test_gpu_applied, &pending);
    g_dbus_connection_flush_sync(t->conn, NULL, &err);
    g_assert_no_error(err);

    listener.loop = g_main_loop_new(NULL, FALSE);
    listener.listener_conn = test_dbus_p2p_from_fd(pair[0]);
    test_dbus_console_setup_listener(&listener, false);
    while (pending) {
        g_main_context_iteration(NULL, TRUE);
    }
    g_dbus_connection_close_sync(listener.listener_conn, NULL, NULL);
    test_dbus_console_cleanup(&listener);
}

/* The same, with both requests reaching the console at once. */
static void
test_dbus_display_gpu_swap_merged(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply_merged(&t,
        g_variant_new_parsed("{'width': <uint32 0>, 'height': <uint32 0>}"),
        g_variant_new_parsed("{'width': <uint32 1600>,"
                             " 'height': <uint32 900>}"));
    test_gpu_swapped(&t, false);

    test_gpu_stop(&t);
}
#endif

/*
 * A monitor swapped and then unplugged before the guest looked: the guest
 * sees the output go, and nothing come back.
 */
static void
test_dbus_display_gpu_swap_unplug(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply(&t, 0, 0, 0);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_wait_edid_width(&t, 0, 1600);
    test_gpu_apply(&t, 0, 0, 0);
    /* With no size, the EDID gives QEMU's default. */
    test_gpu_wait_edid_width(&t, 0, 1280);
    test_gpu_assert_raised(&t);

    g_assert_cmpuint(test_gpu_width(&t), ==, 0);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 0);

    test_gpu_stop(&t);
}

/*
 * A monitor swapped after the guest's last read of the display info, as
 * by firmware that reads it once: the driver that starts next, resetting
 * the device, sees the new monitor.
 */
static void
test_dbus_display_gpu_swap_reset(void)
{
    TestGPU t = {};
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply(&t, 0, 0, 0);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_wait_edid_width(&t, 0, 1600);

    test_gpu_restart_driver(&t);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1600);
    g_assert_cmpuint(out[0].height, ==, 900);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_gpu_stop(&t);
}

/*
 * Each output on its own: a second output starts with no monitor, and a
 * monitor swapped on it leaves the first output as it was.
 */
static void
test_dbus_display_gpu_outputs(void)
{
    TestGPU t = {};
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_start_device(&t, "virtio-gpu-pci,addr=04.0,max_outputs=2", 2);
    g_assert_cmpuint(test_uiinfo_get_u(t.ui_info[1], "width"), ==, 0);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1280);
    g_assert_cmpuint(out[1].width, ==, 0);

    test_gpu_apply(&t, 1, 1024, 768);
    test_gpu_wait_display_event(&t);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1280);
    g_assert_cmpuint(out[1].width, ==, 1024);
    test_gpu_clear_display_event(&t);

    test_gpu_apply(&t, 1, 0, 0);
    test_gpu_apply(&t, 1, 1600, 900);
    test_gpu_wait_edid_width(&t, 1, 1600);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1280);
    g_assert_cmpuint(out[0].height, ==, 800);
    g_assert_cmpuint(out[1].width, ==, 0);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1280);
    g_assert_cmpuint(out[0].height, ==, 800);
    g_assert_cmpuint(out[1].width, ==, 1600);
    g_assert_cmpuint(test_gpu_edid_width(&t, 0), ==, 1280);

    test_gpu_stop(&t);
}

/*
 * A second output configured with a size starts with a monitor of that
 * size, and a UI's first request for it, for no monitor, reaches the guest.
 */
static void
test_dbus_display_gpu_outputs_configured(void)
{
    TestGPU t = {};
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_start_device(&t, "'{\"driver\": \"virtio-gpu-pci\","
                              " \"addr\": \"04.0\", \"max_outputs\": 2,"
                              " \"outputs\": [{\"name\": \"A\"},"
                              " {\"name\": \"B\", \"xres\": 800,"
                              " \"yres\": 600}]}'", 2);
    g_assert_cmpuint(test_uiinfo_get_u(t.ui_info[1], "width"), ==, 800);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[1].width, ==, 800);
    g_assert_cmpuint(out[1].height, ==, 600);

    test_gpu_apply(&t, 1, 0, 0);
    test_gpu_wait_display_event(&t);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1280);
    g_assert_cmpuint(out[1].width, ==, 0);

    test_gpu_stop(&t);
}

/*
 * A request that changes nothing does not reach the guest; one that
 * changes only the refresh rate, or only the physical size, does.
 */
static void
test_dbus_display_gpu_unchanged(void)
{
    g_autoptr(GVariant) info = NULL;
    struct virtio_gpu_resp_edid resp = {};
    uint16_t clock;
    TestGPU t = {};

    test_gpu_start(&t);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1600);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    /*
     * The console passes a request on in the turn of its main loop that
     * takes it, before it takes the next command from the test.
     */
    info = test_uiinfo_get(t.ui_info[0]);
    test_uiinfo_apply_ok(t.ui_info[0], info);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_assert_quiet(&t);

    test_gpu_edid(&t, 0, &resp);
    clock = lduw_le_p(resp.edid + 54);
    test_uiinfo_apply_ok(t.ui_info[0], g_variant_new_parsed(
        "{'refresh_rate': <uint32 30000>}"));
    test_gpu_assert_raised(&t);
    test_gpu_edid(&t, 0, &resp);
    g_assert_cmpuint(lduw_le_p(resp.edid + 54), <, clock);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1600);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_uiinfo_apply_ok(t.ui_info[0], g_variant_new_parsed(
        "{'width_mm': <uint16 600>, 'height_mm': <uint16 340>}"));
    test_gpu_assert_raised(&t);
    test_gpu_edid(&t, 0, &resp);
    g_assert_cmpuint(resp.edid[21], ==, 60);
    g_assert_cmpuint(resp.edid[22], ==, 34);

    test_gpu_stop(&t);
}

/* The 13 characters of the base-block text descriptor with @tag, or "". */
static void
test_edid_text(const uint8_t *edid, uint8_t tag, char text[14])
{
    text[0] = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *desc = edid + 54 + 18 * i;

        if (!desc[0] && !desc[1] && desc[3] == tag) {
            memcpy(text, desc + 5, 13);
            text[13] = 0;
            return;
        }
    }
}

/* An output name of 13 characters is taken whole, and a longer one refused. */
static void
test_dbus_display_gpu_output_name(void)
{
    struct virtio_gpu_resp_edid resp = {};
    TestGPU t = {};
    char text[14];

    if (g_test_subprocess()) {
        qtest_quit(qtest_init("-vga none -device '{\"driver\":"
                              " \"virtio-gpu-pci\", \"outputs\":"
                              " [{\"name\": \"ABCDEFGHIJKLMN\"}]}'"));
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*invalid output name*");

    test_gpu_start_device(&t, "'{\"driver\": \"virtio-gpu-pci\","
                              " \"addr\": \"04.0\","
                              " \"outputs\": [{\"name\": \"ABCDEFGHIJKLM\"}]}'",
                          1);
    test_gpu_edid(&t, 0, &resp);
    test_edid_text(resp.edid, 0xfc, text);
    g_assert_cmpstr(text, ==, "ABCDEFGHIJKLM");

    test_gpu_stop(&t);
}

/* A monitor's identity, as its EDID gives it. */
typedef struct TestMonitor {
    const char *vendor;
    uint16_t product;
    uint32_t serial_number;
    const char *name;
    const char *serial_string;
} TestMonitor;

/* Whether a text descriptor's 13 characters are @s, as an EDID pads it. */
static bool
test_edid_text_is(const char text[14], const char *s)
{
    size_t len = strlen(s);

    return !strncmp(text, s, len) &&
           (len == 13 ||
            (text[len] == '\n' && strspn(text + len + 1, " ") == 12 - len));
}

/* Whether the EDID of output 0 names @m; no serial number text, none. */
static bool
test_gpu_edid_names(TestGPU *t, const TestMonitor *m)
{
    struct virtio_gpu_resp_edid resp = {};
    uint16_t pnp_id = (m->vendor[0] - '@') << 10 |
                      (m->vendor[1] - '@') << 5 |
                      (m->vendor[2] - '@');
    char text[14];

    test_gpu_edid(t, 0, &resp);
    if (lduw_be_p(resp.edid + 8) != pnp_id ||
        lduw_le_p(resp.edid + 10) != m->product ||
        (uint32_t)ldl_le_p(resp.edid + 12) != m->serial_number) {
        return false;
    }
    test_edid_text(resp.edid, 0xfc, text);
    if (!test_edid_text_is(text, m->name)) {
        return false;
    }
    test_edid_text(resp.edid, 0xff, text);
    return m->serial_string[0] ? test_edid_text_is(text, m->serial_string)
                               : !text[0];
}

/* Without a monitor's identity, the guest's EDID names QEMU's own. */
static void
test_dbus_display_gpu_identity_default(void)
{
    static const TestMonitor qemu = { "RHT", 0x1234, 0, "QEMU Monitor", "" };
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_true(test_gpu_edid_names(&t, &qemu));

    test_gpu_stop(&t);
}

static const TestMonitor test_monitor = {
    "LNX", 0x7773, 0xcafebabe, "PiKVM V4 Plus", "CAFEBABE",
};

/* UIInfo.Apply of @m at 1920x1080 on output 0. */
static void
test_gpu_apply_monitor(TestGPU *t, const TestMonitor *m)
{
    test_uiinfo_apply_ok(t->ui_info[0], g_variant_new_parsed(
        "{'width': <uint32 1920>, 'height': <uint32 1080>, 'vendor': <%s>,"
        " 'product': <%q>, 'serial_number': <%u>, 'name': <%s>,"
        " 'serial_string': <%s>}", m->vendor, m->product, m->serial_number,
        m->name, m->serial_string));
}

static void
test_gpu_wait_edid_names(TestGPU *t, const TestMonitor *m)
{
    gint64 end = g_get_monotonic_time() + GPU_TIMEOUT_US;

    while (!test_gpu_edid_names(t, m)) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
}

/* The EDID the guest reads names the monitor the client gave. */
static void
test_dbus_display_gpu_identity(void)
{
    TestGPU t = {};
    TestMonitor m = test_monitor;

    test_gpu_start(&t);
    test_gpu_apply_monitor(&t, &m);
    test_gpu_wait_edid_names(&t, &m);

    /* No name, no serial number text: QEMU's own name, and no text. */
    m.name = "";
    m.serial_string = "";
    test_gpu_apply_monitor(&t, &m);
    m.name = "QEMU Monitor";
    test_gpu_wait_edid_names(&t, &m);
    test_gpu_stop(&t);

    /* An output named in the configuration keeps that name. */
    test_gpu_start_device(&t, "'{\"driver\": \"virtio-gpu-pci\","
                              " \"addr\": \"04.0\","
                              " \"outputs\": [{\"name\": \"CFG\"}]}'", 1);
    m = test_monitor;
    test_gpu_apply_monitor(&t, &m);
    m.name = "CFG";
    test_gpu_wait_edid_names(&t, &m);
    test_gpu_stop(&t);
}

static const TestMonitor test_monitor_name = {
    "LNX", 0x7773, 0xcafebabe, "Second", "CAFEBABE",
};
static const TestMonitor test_monitor_serial_number = {
    "LNX", 0x7773, 0xcafebabf, "PiKVM V4 Plus", "CAFEBABE",
};
static const TestMonitor test_monitor_serial_string = {
    "LNX", 0x7773, 0xcafebabe, "PiKVM V4 Plus", "CAFEBABF",
};
static const TestMonitor test_monitor_product = {
    "LNX", 0x7774, 0xcafebabe, "PiKVM V4 Plus", "CAFEBABE",
};
static const TestMonitor test_monitor_vendor = {
    "SAM", 0x7773, 0xcafebabe, "PiKVM V4 Plus", "CAFEBABE",
};

/*
 * Another monitor of the same size, @data, on an output the guest has
 * seen: the guest sees the first one go and the other come.
 */
static void
test_dbus_display_gpu_identity_swap(const void *data)
{
    const TestMonitor *other = data;
    TestGPU t = {};

    test_gpu_start(&t);
    test_gpu_apply_monitor(&t, &test_monitor);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1920);
    test_gpu_clear_display_event(&t);

    test_gpu_apply_monitor(&t, other);
    test_gpu_wait_edid_names(&t, other);
    test_gpu_assert_raised(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 0);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_raised(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1920);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_gpu_stop(&t);
}

/*
 * A monitor's identity given only after the guest has seen the output
 * with QEMU's own: that one goes, and this one comes.
 */
static void
test_dbus_display_gpu_identity_first(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply_monitor(&t, &test_monitor);
    test_gpu_wait_edid_names(&t, &test_monitor);
    test_gpu_assert_raised(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 0);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_raised(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1920);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_gpu_stop(&t);
}

/* A guest that reads no EDID sees another identity as the same monitor. */
static void
test_dbus_display_gpu_identity_no_edid(void)
{
    TestGPU t = {};

    test_gpu_start_device(&t, "virtio-gpu-pci,addr=04.0,edid=off", 1);
    test_gpu_apply_monitor(&t, &test_monitor);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1920);
    test_gpu_clear_display_event(&t);

    test_gpu_apply_monitor(&t, &test_monitor_name);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1920);

    test_gpu_stop(&t);
}

#ifndef WIN32
/*
 * Two requests that reach the console at once reach the device as one:
 * another monitor's identity, given and taken back together, is no other
 * monitor to the guest. Passed on one at a time, they would take the
 * output away.
 */
static void
test_dbus_display_gpu_identity_merged(void)
{
    TestGPU t = {};

    test_gpu_start(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);
    test_gpu_apply_merged(&t, g_variant_new_parsed("{'name': <'Other'>}"),
                          g_variant_new_parsed("{'name': <''>}"));
    g_assert_cmpuint(test_gpu_width(&t), ==, 1280);

    test_gpu_stop(&t);
}
#endif

/* "connected" takes the output away and gives it back, at the size it had. */
static void
test_dbus_display_gpu_connected(void)
{
    TestGPU t = {};
    TestGPUOutput out[GPU_OUTPUTS];

    test_gpu_start(&t);
    test_gpu_apply(&t, 0, 1600, 900);
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 1600);
    test_gpu_clear_display_event(&t);

    test_uiinfo_apply_ok(t.ui_info[0], g_variant_new_parsed(
        "{'connected': <false>}"));
    test_gpu_wait_display_event(&t);
    g_assert_cmpuint(test_gpu_width(&t), ==, 0);
    test_gpu_clear_display_event(&t);

    test_uiinfo_apply_ok(t.ui_info[0], g_variant_new_parsed(
        "{'connected': <true>}"));
    test_gpu_wait_display_event(&t);
    test_gpu_display_info(&t, out);
    g_assert_cmpuint(out[0].width, ==, 1600);
    g_assert_cmpuint(out[0].height, ==, 900);
    test_gpu_clear_display_event(&t);
    test_gpu_assert_quiet(&t);

    test_gpu_stop(&t);
}

static gsize
get_console_ids_count(GDBusConnection *conn)
{
    g_autoptr(GError) err = NULL;
    g_autoptr(QemuDBusDisplay1VMProxy) vm = NULL;
    GVariant *console_ids;
    gsize n_ids = 0;

    vm = QEMU_DBUS_DISPLAY1_VM_PROXY(
        qemu_dbus_display1_vm_proxy_new_sync(
            conn,
            G_DBUS_PROXY_FLAGS_NONE,
            NULL,
            DBUS_DISPLAY1_ROOT "/VM",
            NULL,
            &err));
    g_assert_no_error(err);

    console_ids = qemu_dbus_display1_vm_get_console_ids(
        QEMU_DBUS_DISPLAY1_VM(vm));
    if (console_ids) {
        n_ids = g_variant_n_children(console_ids);
    }
    return n_ids;
}

static void
wait_device_event(QTestState *qts, const char *event_name, const char *id)
{
    QDict *resp, *data;
    QString *qstr;

    for (;;) {
        resp = qtest_qmp_eventwait_ref(qts, event_name);
        data = qdict_get_qdict(resp, "data");
        if (!data || !qdict_get(data, "device")) {
            qobject_unref(resp);
            continue;
        }
        qstr = qobject_to(QString, qdict_get(data, "device"));
        if (!strcmp(qstring_get_str(qstr), id)) {
            qobject_unref(resp);
            break;
        }
        qobject_unref(resp);
    }
}

static void
test_dbus_display_hotplug(void)
{
    g_autoptr(GDBusConnection) conn = NULL;
    QTestState *qts = NULL;
    gsize n;

    test_setup_args(&qts, &conn,
        "-machine q35"
        " -device pcie-root-port,id=rp0"
        " -display dbus,p2p=yes"
        " -name dbus-test");

    n = get_console_ids_count(conn);
    g_assert_cmpuint(n, ==, 1);

    qtest_qmp_device_add(qts, "bochs-display", "bochs0",
                         "{'bus': 'rp0'}");

    n = get_console_ids_count(conn);
    g_assert_cmpuint(n, ==, 2);

    /*
     * On q35, PCI device removal is ACPI-based and requires guest
     * acknowledgement. Since qtest has no guest OS, issue the delete
     * request and force removal via system reset.
     */
    qtest_qmp_device_del_send(qts, "bochs0");
    qtest_system_reset_nowait(qts);
    wait_device_event(qts, "DEVICE_DELETED", "bochs0");

    n = get_console_ids_count(conn);
    g_assert_cmpuint(n, ==, 1);

    g_clear_object(&conn);
    qtest_quit(qts);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/dbus-display/vm", test_dbus_display_vm);
    qtest_add_data_func("/dbus-display/console", GINT_TO_POINTER(false), test_dbus_display_console);
    qtest_add_data_func("/dbus-display/console/map", GINT_TO_POINTER(true), test_dbus_display_console);
    qtest_add_func("/dbus-display/console/text-resize",
                   test_dbus_display_text_resize);
    qtest_add_func("/dbus-display/keyboard", test_dbus_display_keyboard);
    if (qtest_has_device("virtio-gpu-pci")) {
        qtest_add_func("/dbus-display/uiinfo/initial",
                       test_dbus_display_uiinfo_initial);
#if defined(CONFIG_VNC) && !defined(WIN32)
        qtest_add_func("/dbus-display/uiinfo/vnc",
                       test_dbus_display_uiinfo_vnc);
#endif
        qtest_add_func("/dbus-display/uiinfo/legacy",
                       test_dbus_display_uiinfo_legacy);
        qtest_add_func("/dbus-display/uiinfo/identity",
                       test_dbus_display_uiinfo_identity);
        qtest_add_func("/dbus-display/uiinfo/connected",
                       test_dbus_display_uiinfo_connected);
        qtest_add_func("/dbus-display/gpu/no-size",
                       test_dbus_display_gpu_no_size);
        qtest_add_func("/dbus-display/gpu/first-off",
                       test_dbus_display_gpu_first_off);
        qtest_add_func("/dbus-display/gpu/event-clear",
                       test_dbus_display_gpu_event_clear);
        qtest_add_data_func("/dbus-display/gpu/swap/read-first",
                            GINT_TO_POINTER(false),
                            test_dbus_display_gpu_swap);
        qtest_add_data_func("/dbus-display/gpu/swap/clear-first",
                            GINT_TO_POINTER(true),
                            test_dbus_display_gpu_swap);
#ifndef WIN32
        qtest_add_func("/dbus-display/gpu/swap/merged",
                       test_dbus_display_gpu_swap_merged);
#endif
        qtest_add_func("/dbus-display/gpu/swap/unplug",
                       test_dbus_display_gpu_swap_unplug);
        qtest_add_func("/dbus-display/gpu/swap/reset",
                       test_dbus_display_gpu_swap_reset);
        qtest_add_func("/dbus-display/gpu/outputs",
                       test_dbus_display_gpu_outputs);
        qtest_add_func("/dbus-display/gpu/outputs/configured",
                       test_dbus_display_gpu_outputs_configured);
        qtest_add_func("/dbus-display/gpu/unchanged",
                       test_dbus_display_gpu_unchanged);
        qtest_add_func("/dbus-display/gpu/output-name",
                       test_dbus_display_gpu_output_name);
        qtest_add_func("/dbus-display/gpu/identity/default",
                       test_dbus_display_gpu_identity_default);
        qtest_add_func("/dbus-display/gpu/identity",
                       test_dbus_display_gpu_identity);
        qtest_add_func("/dbus-display/gpu/identity/first",
                       test_dbus_display_gpu_identity_first);
        qtest_add_func("/dbus-display/gpu/identity/no-edid",
                       test_dbus_display_gpu_identity_no_edid);
#ifndef WIN32
        qtest_add_func("/dbus-display/gpu/identity/merged",
                       test_dbus_display_gpu_identity_merged);
#endif
        qtest_add_data_func("/dbus-display/gpu/identity/swap/name",
                            &test_monitor_name,
                            test_dbus_display_gpu_identity_swap);
        qtest_add_data_func("/dbus-display/gpu/identity/swap/serial-number",
                            &test_monitor_serial_number,
                            test_dbus_display_gpu_identity_swap);
        qtest_add_data_func("/dbus-display/gpu/identity/swap/serial-string",
                            &test_monitor_serial_string,
                            test_dbus_display_gpu_identity_swap);
        qtest_add_data_func("/dbus-display/gpu/identity/swap/product",
                            &test_monitor_product,
                            test_dbus_display_gpu_identity_swap);
        qtest_add_data_func("/dbus-display/gpu/identity/swap/vendor",
                            &test_monitor_vendor,
                            test_dbus_display_gpu_identity_swap);
        qtest_add_func("/dbus-display/gpu/connected",
                       test_dbus_display_gpu_connected);
    }
    if (qtest_has_machine("q35") && qtest_has_device("bochs-display")) {
        qtest_add_func("/dbus-display/hotplug", test_dbus_display_hotplug);
    }

    return g_test_run();
}

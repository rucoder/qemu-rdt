#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "qemu/dbus.h"
#include "qemu/sockets.h"
#include "glib.h"
#include "glibconfig.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
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

#if defined(CONFIG_VNC) && !defined(WIN32)
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
        "{'refresh_rate': <uint32 60000>}"));

    fd = test_vnc_connect(qts);
    test_vnc_set_desktop_size(fd, 1600, 900);
    end = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (test_uiinfo_get_u(ui_info, "width") != 1600) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "height"), ==, 900);
    g_assert_cmpuint(test_uiinfo_get_u(ui_info, "refresh_rate"), ==, 60000);

    close(fd);
    g_clear_object(&ui_info);
    g_clear_object(&conn);
    qtest_quit(qts);
}
#endif

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
    }
    if (qtest_has_machine("q35") && qtest_has_device("bochs-display")) {
        qtest_add_func("/dbus-display/hotplug", test_dbus_display_hotplug);
    }

    return g_test_run();
}

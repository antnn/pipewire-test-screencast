#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>
#include <pipewire/pipewire.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

static GDBusConnection *connection = NULL;
static GDBusProxy *screencast_proxy = NULL;

#define WIDTH 1920
#define HEIGHT 1080

#define MAX_BUFFERS 64

#include "sdl.h"

uint32_t pw_stream_node_id;
int pw_fd;

void start_request_response_signal_handler(GDBusConnection *connection,
                                           const char *sender_name,
                                           const char *object_path,
                                           const char *interface_name,
                                           const char *signal_name,
                                           GVariant *parameters,
                                           gpointer user_data);

gchar *session_handle_ = "";

GCancellable *cancellable = NULL;

const char *kDesktopRequestObjectPath = "/org/freedesktop/portal/desktop/request";
const gchar *prepare_signal_handle(const gchar *token,
                                   GDBusConnection *connection)
{
    gchar *sender =
        g_strdup(g_dbus_connection_get_unique_name(connection) + 1); // cut ":" from string
    for (int i = 0; sender[i]; ++i)
    {
        if (sender[i] == '.')
        {
            sender[i] = '_'; // replace "." in string
        }
    }
    const gchar *handle = g_strconcat(kDesktopRequestObjectPath, "/", sender,
                                      "/", token, /*end of varargs*/ NULL);
    return handle;
}

uint32_t sources_request_signal_id_;
const char *kDesktopBusName = "org.freedesktop.portal.Desktop";
const char *kRequestInterfaceName = "org.freedesktop.portal.Request";
uint32_t setup_request_response_signal(const char *object_path,
                                       const GDBusSignalCallback callback,
                                       gpointer user_data,
                                       GDBusConnection *connection)
{
    return g_dbus_connection_signal_subscribe(
        connection, kDesktopBusName, kRequestInterfaceName, "Response",
        object_path, /*arg0=*/NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
        callback, user_data, /*user_data_free_func=*/NULL);
}

void process_pipewire(int, uint32_t);
void on_portal_done()
{
    process_pipewire(pw_fd, pw_stream_node_id);
}

uint32_t start_request_signal_id;
gchar *start_handle = "";

void start_request()
{
    GVariantBuilder builder;
    gchar *variant_string;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    // token for handle
    variant_string =
        g_strdup_printf("%s%d", "pythonMss", g_random_int_range(0, G_MAXINT));
    g_variant_builder_add(&builder, "{sv}", "handle_token",
                          g_variant_new_string(variant_string));
    //"/org/freedesktop/portal/desktop/request"
    start_handle = prepare_signal_handle(variant_string, connection);
    start_request_signal_id = setup_request_response_signal(
        start_handle, start_request_response_signal_handler, NULL, connection);

    // "Identifier for the application window", this is Wayland, so not "x11:...".
    const char parent_window[] = "";

    printf("Starting the portal session.\n");
    g_autoptr(GError) error = NULL;
    g_dbus_proxy_call_sync(
        screencast_proxy, "Start",
        g_variant_new("(osa{sv})", session_handle_, parent_window,
                      &builder),
        G_DBUS_CALL_FLAGS_NONE, /*timeout=*/-1, cancellable, &error);
    if (error)
    {
        on_portal_done();
    }
}

void sources_request_response_signal_handler(GDBusConnection *connection,
                                             const char *sender_name,
                                             const char *object_path,
                                             const char *interface_name,
                                             const char *signal_name,
                                             GVariant *parameters,
                                             gpointer user_data)
{

    uint32_t portal_response;
    g_variant_get(parameters, "(u@a{sv})", &portal_response, NULL);
    if (portal_response)
    {
        printf("Failed to select sources for the screen cast session.");
        on_portal_done();
        return;
    }
    start_request();
}

gchar *restore_token = "";
uint32_t capture_source_type;

void cleanup()
{
}

void open_pipewire_remote()
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

    printf("Opening the PipeWire remote.\n");
    GUnixFDList *outlist = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) variant = g_dbus_proxy_call_with_unix_fd_list_sync(
        screencast_proxy, "OpenPipeWireRemote",
        g_variant_new("(oa{sv})", session_handle_, &builder),
        G_DBUS_CALL_FLAGS_NONE, /*timeout=*/-1, /*outlist=*/NULL, &outlist, cancellable,
        &error);
    if (error)
    {
        cleanup();
    }
    error = NULL;
    int32_t index;
    g_variant_get(variant, "(h)", &index);
    pw_fd = g_unix_fd_list_get(outlist, index, &error);

    if (pw_fd == -1)
    {
        cleanup();
        return;
    }

    on_portal_done();
}

bool StartScreenCastStream()
{
}

void start_request_response_signal_handler(GDBusConnection *connection,
                                           const char *sender_name,
                                           const char *object_path,
                                           const char *interface_name,
                                           const char *signal_name,
                                           GVariant *parameters,
                                           gpointer user_data)
{
    printf("Start signal received.\n");
    uint32_t portal_response;
    g_autoptr(GVariant) response_data;
    g_autoptr(GVariantIter) iter;
    gchar *restore_token_ = "";
    g_variant_get(parameters, "(u@a{sv})", &portal_response,
                  &response_data);
    if (portal_response || !response_data)
    {
        on_portal_done();
    }

    // Array of PipeWire streams. See
    // https://github.com/flatpak/xdg-desktop-portal/blob/master/data/org.freedesktop.portal.ScreenCast.xml
    // documentation for <method name="Start">.
    if (g_variant_lookup(response_data, "streams", "a(ua{sv})",
                         &iter))
    {
        g_autoptr(GVariant) variant;

        while (g_variant_iter_next(iter, "@(ua{sv})", &variant))
        {
            uint32_t stream_id;
            uint32_t type;
            g_autoptr(GVariant) options;

            g_variant_get(variant, "(u@a{sv})", &stream_id, &options);
            if (g_variant_lookup(options, "source_type", "u", &type))
            {
                capture_source_type = (uint32_t)(type);
            }

            pw_stream_node_id = stream_id;

            break;
        }
    }

    if (g_variant_lookup(response_data, "restore_token", "s",
                         restore_token_))
    {
        restore_token = restore_token_;
    }

    open_pipewire_remote();
}

const char *kDesktopSessionObjectPath = "/org/freedesktop/portal/desktop/session";
gchar *new_session_path(int token)
{
    return g_strdup_printf("%s/%d", kDesktopSessionObjectPath, token);
}

const char *portal_prefix = "pythonMss";
gchar *portal_handle = "";
int session_request_signal_id = 0;

void sources_request();
void on_session_closed_signal(GDBusConnection *connection,
                              const char *sender_name,
                              const char *object_path,
                              const char *interface_name,
                              const char *signal_name,
                              GVariant *parameters,
                              gpointer user_data);
int session_closed_signal_id_ = 0;
char *kSessionInterfaceName = "org.freedesktop.portal.Session";
void request_session_response_signale_handler(
    GDBusConnection *connection,
    const char *sender_name,
    const char *object_path,
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gpointer user_data)
{

    uint32_t portal_response;
    g_autoptr(GVariant) response_data = NULL;
    g_variant_get(parameters, /*format_string=*/"(u@a{sv})", &portal_response,
                  &response_data);
    g_autoptr(GVariant) g_session_handle =
        g_variant_lookup_value(response_data, /*key=*/"session_handle",
                               /*expected_type=*/NULL);
    session_handle_ = g_variant_dup_string(
        /*value=*/g_session_handle, /*length=*/NULL);

    if (session_handle_ == "" || !session_handle_ || portal_response)
    {
        printf("Failed to request the session subscription.\n");
        // OnPortalDone(RequestResponse::kError);
        return;
    }

    session_closed_signal_id_ = g_dbus_connection_signal_subscribe(
        connection, kDesktopBusName, kSessionInterfaceName, /*member=*/"Closed",
        session_handle_, /*arg0=*/NULL, G_DBUS_SIGNAL_FLAGS_NONE,
        on_session_closed_signal, NULL, /*user_data_free_func=*/NULL);
    sources_request();
}

void setup_session_request_handlers()
{
    GVariantBuilder builder;
    gchar *variant_string;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    variant_string = g_strdup_printf("%s_session%d", portal_prefix,
                                     g_random_int_range(0, G_MAXINT));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token",
                          g_variant_new_string(variant_string));

    variant_string = g_strdup_printf("%s_%d", portal_prefix,
                                     g_random_int_range(0, G_MAXINT));
    g_variant_builder_add(&builder, "{sv}", "handle_token",
                          g_variant_new_string(variant_string));

    portal_handle = prepare_signal_handle(variant_string, connection);

    session_request_signal_id = setup_request_response_signal(
        portal_handle, request_session_response_signale_handler, NULL,
        connection);

    "Desktop session requested.";
    g_autoptr(GError) error;
    g_dbus_proxy_call_sync(
        screencast_proxy, "CreateSession", g_variant_new("(a{sv})", &builder),
        G_DBUS_CALL_FLAGS_NONE, /*timeout=*/-1, cancellable,
        &error);
    if (error)
    {
        cleanup();
        exit(-1);
    }
}

void on_session_closed_signal(GDBusConnection *connection,
                              const char *sender_name,
                              const char *object_path,
                              const char *interface_name,
                              const char *signal_name,
                              GVariant *parameters,
                              gpointer user_data)
{
    // OnScreenCastSessionClosed();

    // Unsubscribe from the signal and free the session handle to avoid calling
    // Session::Close from the destructor since it's already closed
    g_dbus_connection_signal_unsubscribe(connection,
                                         session_closed_signal_id_);
}

void sources_request()
{
    g_autoptr(GError) error = NULL;
    GVariantBuilder builder;
    gchar *token_string;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    // We want to record monitor content.
    g_variant_builder_add(
        &builder, "{sv}", "types",
        g_variant_new_uint32((uint32_t)(1U)));
    // We don't want to allow selection of multiple sources.
    g_variant_builder_add(&builder, "{sv}", "multiple",
                          g_variant_new_boolean(false));

    g_autoptr(GVariant) cursorModesVariant =
        g_dbus_proxy_get_cached_property(screencast_proxy, "AvailableCursorModes");
    if (cursorModesVariant)
    {
        uint32_t modes = 0;
        g_variant_get(cursorModesVariant, "u", &modes);
        // Make request only if this mode is advertised by the portal
        // implementation.
        uint32_t cursor_mode_ = 4U;
        if (modes & (uint32_t)(cursor_mode_))
        {
            g_variant_builder_add(
                &builder, "{sv}", "cursor_mode",
                g_variant_new_uint32((uint32_t)(cursor_mode_)));
        }
    }

    g_autoptr(GVariant) versionVariant =
        g_dbus_proxy_get_cached_property(screencast_proxy, "version");
    if (versionVariant)
    {
        uint32_t version = 0;
        g_variant_get(versionVariant, "u", &version);
        // Make request only if xdg-desktop-portal has required API version
        if (version >= 4)
        {
            uint32_t persist_mode_ = 0U;
            g_variant_builder_add(
                &builder, "{sv}", "persist_mode",
                g_variant_new_uint32((uint32_t)(persist_mode_)));
            /*if (!restore_token_.empty())
            {
                g_variant_builder_add(&builder, "{sv}", "restore_token",
                                      g_variant_new_string(restore_token_.c_str()));
            }*/
        }
    }
    token_string = g_strdup_printf("pythonMss%d", g_random_int_range(0, G_MAXINT));
    g_variant_builder_add(&builder, "{sv}", "handle_token",
                          g_variant_new_string(token_string));
    /// request token path
    const gchar *sources_handle = prepare_signal_handle(token_string, connection);
    sources_request_signal_id_ = setup_request_response_signal(
        sources_handle, sources_request_response_signal_handler,
        NULL, connection);

    printf("Requesting sources from the screen cast session.\n");

    error = NULL;
    g_autoptr(GVariant) result = g_dbus_proxy_call_sync(
        screencast_proxy, "SelectSources",
        g_variant_new("(oa{sv})", session_handle_, &builder),
        G_DBUS_CALL_FLAGS_NONE, /*timeout=*/-1, cancellable, &error);
    if (error)
    {
        return -1;
    }
}

int main(int argc, char *argv[])
{
    g_autoptr(GError) error = NULL;
    if (!connection)
    {
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

        if (error)
        {
            printf(
                "Error retrieving D-Bus connection: %s \n",
                error->message);
            return -1;
        }
    }
    if (!screencast_proxy)
    {
        error = NULL;
        screencast_proxy = g_dbus_proxy_new_sync(
            connection, G_DBUS_PROXY_FLAGS_NONE,
            NULL, "org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.ScreenCast", NULL, &error);

        if (error)
        {
            printf("[portals] Error retrieving D-Bus proxy: %s\n",
                   error->message);
            return -1;
        }
        cancellable = g_cancellable_new();
        setup_session_request_handlers();
        GMainLoop *mainloop = g_main_loop_new(NULL, TRUE);
        g_main_loop_run(mainloop);
    }
    return 0;
}

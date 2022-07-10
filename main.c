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

void start_request_response_signal_handler(GDBusConnection *connection,
										   const char *sender_name,
										   const char *object_path,
										   const char *interface_name,
										   const char *signal_name,
										   GVariant *parameters,
										   gpointer user_data);

gchar *session_handle_ = "";

struct pixel
{
	float r, g, b, a;
};

struct data
{
	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;
	SDL_Texture *cursor;

	struct pw_main_loop *loop;
	struct spa_source *timer;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info format;
	int32_t stride;
	struct spa_rectangle size;

	int counter;
};

static void handle_events(struct data *data)
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_QUIT:
			pw_main_loop_quit(data->loop);
			break;
		}
	}
}

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  b = pw_stream_dequeue_buffer(stream);
 *
 *  .. do stuff with buffer ...
 *
 *  pw_stream_queue_buffer(stream, b);
 */
static void
on_process(void *_data)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	struct pw_buffer *b;
	struct spa_buffer *buf;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	uint32_t i;
	uint8_t *src, *dst;

	b = NULL;
	/* dequeue and queue old buffers, use the last available
	 * buffer */
	while (true)
	{
		struct pw_buffer *t;
		if ((t = pw_stream_dequeue_buffer(stream)) == NULL)
			break;
		if (b)
			pw_stream_queue_buffer(stream, b);
		b = t;
	}
	if (b == NULL)
	{
		pw_log_warn("out of buffers: %m");
		return;
	}

	buf = b->buffer;

	pw_log_info("new buffer %p", buf);

	handle_events(data);

	if ((sdata = buf->datas[0].data) == NULL)
		goto done;

	if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0)
	{
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		goto done;
	}

	/* copy video image in texture */
	sstride = buf->datas[0].chunk->stride;
	ostride = SPA_MIN(sstride, dstride);

	src = sdata;
	dst = ddata;

	for (i = 0; i < data->size.height; i++)
	{
		memcpy(dst, src, ostride);
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(data->texture);

	SDL_RenderClear(data->renderer);
	/* now render the video */
	SDL_RenderCopy(data->renderer, data->texture, NULL, NULL);
	SDL_RenderPresent(data->renderer);

done:
	pw_stream_queue_buffer(stream, b);
}

static void on_stream_state_changed(void *_data, enum pw_stream_state old,
									enum pw_stream_state state, const char *error)
{
	struct data *data = _data;
	fprintf(stderr, "stream state: \"%s\"\n", pw_stream_state_as_string(state));
	switch (state)
	{
	case PW_STREAM_STATE_UNCONNECTED:
		pw_main_loop_quit(data->loop);
		break;
	case PW_STREAM_STATE_PAUSED:
		pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
							 data->timer, NULL, NULL, false);
		break;
	case PW_STREAM_STATE_STREAMING:
	{
		struct timespec timeout, interval;

		timeout.tv_sec = 1;
		timeout.tv_nsec = 0;
		interval.tv_sec = 1;
		interval.tv_nsec = 0;

		pw_loop_update_timer(pw_main_loop_get_loop(data->loop),
							 data->timer, &timeout, &interval, false);
		break;
	}
	default:
		break;
	}
}

/* Be notified when the stream param changes. We're only looking at the
 * format changes.
 *
 * We are now supposed to call pw_stream_finish_format() with success or
 * failure, depending on if we can support the format. Because we gave
 * a list of supported formats, this should be ok.
 *
 * As part of pw_stream_finish_format() we can provide parameters that
 * will control the buffer memory allocation. This includes the metadata
 * that we would like on our buffer, the size, alignment, etc.
 */
static void
on_stream_param_changed(void *_data, uint32_t id, const struct spa_pod *param)
{
	struct data *data = _data;
	struct pw_stream *stream = data->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[1];
	Uint32 sdl_format;
	void *d;

	/* NULL means to clear the format */
	if (param == NULL || id != SPA_PARAM_Format)
		return;

	fprintf(stderr, "got format:\n");
	spa_debug_format(2, NULL, param);

	if (spa_format_parse(param, &data->format.media_type, &data->format.media_subtype) < 0)
		return;

	if (data->format.media_type != SPA_MEDIA_TYPE_video ||
		data->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	/* call a helper function to parse the format for us. */
	spa_format_video_raw_parse(param, &data->format.info.raw);
	sdl_format = id_to_sdl_format(data->format.info.raw.format);
	data->size = data->format.info.raw.size;

	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN)
	{
		pw_stream_set_error(stream, -EINVAL, "unknown pixel format");
		return;
	}

	data->texture = SDL_CreateTexture(data->renderer,
									  sdl_format,
									  SDL_TEXTUREACCESS_STREAMING,
									  data->size.width,
									  data->size.height);
	SDL_LockTexture(data->texture, NULL, &d, &data->stride);
	SDL_UnlockTexture(data->texture);

	/* a SPA_TYPE_OBJECT_ParamBuffers object defines the acceptable size,
	 * number, stride etc of the buffers */
	params[0] = spa_pod_builder_add_object(&b,
										   SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
										   SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
										   SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
										   SPA_PARAM_BUFFERS_size, SPA_POD_Int(data->stride * data->size.height),
										   SPA_PARAM_BUFFERS_stride, SPA_POD_Int(data->stride),
										   SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr)));

	/* we are done */
	pw_stream_update_params(stream, params, 1);
}

/* these are the stream events we listen for */
static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.param_changed = on_stream_param_changed,
	.process = on_process,
};

static int build_format(struct data *data, struct spa_pod_builder *b, const struct spa_pod **params)
{
	SDL_RendererInfo info;

	SDL_GetRendererInfo(data->renderer, &info);
	params[0] = sdl_build_formats(&info, b);

	fprintf(stderr, "supported SDL formats:\n");
	spa_debug_format(2, NULL, params[0]);

	return 1;
}

static int reneg_format(struct data *data)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];
	int32_t width, height;

	if (data->format.info.raw.format == 0)
		return -EBUSY;

	width = data->counter & 1 ? 320 : 640;
	height = data->counter & 1 ? 240 : 480;

	fprintf(stderr, "renegotiate to %dx%d:\n", width, height);
	params[0] = spa_pod_builder_add_object(&b,
										   SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
										   SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
										   SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
										   SPA_FORMAT_VIDEO_format, SPA_POD_Id(data->format.info.raw.format),
										   SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
										   SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&data->format.info.raw.framerate));

	pw_stream_update_params(data->stream, params, 1);

	data->counter++;
	return 0;
}

static int reneg_buffers(struct data *data)
{
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[2];

	fprintf(stderr, "renegotiate buffers\n");
	params[0] = spa_pod_builder_add_object(&b,
										   SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
										   SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, MAX_BUFFERS),
										   SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
										   SPA_PARAM_BUFFERS_size, SPA_POD_Int(data->stride * data->size.height),
										   SPA_PARAM_BUFFERS_stride, SPA_POD_Int(data->stride));

	pw_stream_update_params(data->stream, params, 1);

	data->counter++;
	return 0;
}

static void on_timeout(void *userdata, uint64_t expirations)
{
	struct data *data = userdata;
	if (1)
		reneg_format(data);
	else
		reneg_buffers(data);
}

static void do_quit(void *userdata, int signal_number)
{
	struct data *data = userdata;
	pw_main_loop_quit(data->loop);
}

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

void on_portal_done()
{
	
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

uint32_t pw_stream_node_id;
int pw_fd;
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

void process_pipewire(int argc, char *argv[])
{
	struct data data = {
		0,
	};
	const struct spa_pod *params[2];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	int res, n_params;

	//pw_init(&argc, &argv);

	/* create a main loop */
	data.loop = pw_main_loop_new(NULL);

	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
	pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

	/* create a simple stream, the simple stream manages to core and remote
	 * objects for you if you don't need to deal with them
	 *
	 * If you plan to autoconnect your stream, you need to provide at least
	 * media, category and role properties
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the stream state. The most important event
	 * you need to listen to is the process event where you need to consume
	 * the data provided to you.
	 */
	data.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(data.loop),
		"video-play-reneg",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Camera",
			NULL),
		&stream_events,
		&data);

	data.path = argc > 1 ? argv[1] : NULL;

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		fprintf(stderr, "can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer(WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer))
	{
		fprintf(stderr, "can't create window: %s\n", SDL_GetError());
		return -1;
	}

	/* build the extra parameters to connect with. To connect, we can provide
	 * a list of supported formats.  We use a builder that writes the param
	 * object to the stack. */
	n_params = build_format(&data, &b, params);

	/* now connect the stream, we need a direction (input/output),
	 * an optional target node to connect to, some flags and parameters
	 */
	if ((res = pw_stream_connect(data.stream,
								 PW_DIRECTION_INPUT,
								 data.path ? (uint32_t)atoi(data.path) : PW_ID_ANY,
								 PW_STREAM_FLAG_AUTOCONNECT |	 /* try to automatically connect this stream */
									 PW_STREAM_FLAG_MAP_BUFFERS, /* mmap the buffer data for us */
								 params, n_params))				 /* extra parameters, see above */
		< 0)
	{
		fprintf(stderr, "can't connect: %s\n", spa_strerror(res));
		return -1;
	}

	data.timer = pw_loop_add_timer(pw_main_loop_get_loop(data.loop), on_timeout, &data);

	/* do things until we quit the mainloop */
	pw_main_loop_run(data.loop);

	pw_stream_destroy(data.stream);
	pw_main_loop_destroy(data.loop);

	SDL_DestroyTexture(data.texture);
	if (data.cursor)
		SDL_DestroyTexture(data.cursor);
	SDL_DestroyRenderer(data.renderer);
	SDL_DestroyWindow(data.window);
	pw_deinit();
}
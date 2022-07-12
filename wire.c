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

struct pw_core_events;
struct pw_thread_loop;
struct pw_context;

struct pw_core_events pw_core_events_ = {};
struct pw_thread_loop *pw_main_loop_ = NULL;
struct pw_context *pw_context_ = NULL;

uint32_t pw_stream_node_id_ = 0;
int pw_fd_ = 0;
char *pw_client_version_ = "";
struct pw_core *pw_core_ = NULL;
struct pw_stream_events pw_stream_events_;
struct spa_hook spa_core_listener_;
struct spa_source *renegotiate_ = NULL;

struct DATA
{
};
struct DATA userdata;

static void on_renegotiate_format(void *data, uint64_t foo)
{
    printf("renegonitiating\n");
    return;
}

static void on_core_info(void *data, const struct pw_core_info *info)
{
    return;
}
static void on_core_done(void *object, uint32_t id, int seq)
{
    return;
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    return;
}

static void on_stream_state_changed(void *data, enum pw_stream_state old_state,
                                    enum pw_stream_state state, const char *error_message)
{
}

static void on_streamParam_changed(void *data, uint32_t id, const struct spa_pod *format)
{
    return;
}
static void on_stream_process(void *data)
{
    return;
}
// unwrap macros
struct spa_source *__pw_loop_add_event(struct pw_loop *loop,
                                       spa_source_event_func_t usermethod,
                                       void *userdata)
{
    struct spa_source *res = NULL;
    struct spa_loop_utils *utils = loop->utils;
    struct spa_interface *iface = &utils->iface;
    struct spa_callbacks *callbacks = &iface->cb;
    const struct spa_loop_utils_methods *_f = (const struct spa_loop_utils_methods *)callbacks->funcs;
    res = _f->add_event(callbacks->data, usermethod, userdata);
    return res;
}

int __pw_loop_signal_event(struct pw_loop *loop, struct spa_source *source)
{
    int res = -ENOTSUP;
    struct spa_loop_utils *utils = loop->utils;
    struct spa_interface *iface = &utils->iface;
    struct spa_callbacks *callbacks = &iface->cb;
    const struct spa_loop_utils_methods *_f = (const struct spa_loop_utils_methods *)callbacks->funcs;
    res = _f->signal_event(callbacks->data, source);
    return res;
}
// unwrap macros

void process_pipewire(int pw_fd, uint32_t pw_stream_node_id)
{
    uint32_t width = 1920;
    uint32_t height = 1080;

    pw_stream_node_id_ = pw_stream_node_id;
    pw_fd_ = pw_fd;

    pw_init(/*argc=*/NULL, /*argc=*/NULL);

    pw_main_loop_ = pw_thread_loop_new("pipewire-main-loop", NULL);

    pw_context_ =
        pw_context_new(pw_thread_loop_get_loop(pw_main_loop_), NULL, 0);
    if (!pw_context_)
    {
        "Failed to create PipeWire context";
        return false;
    }

    if (pw_thread_loop_start(pw_main_loop_) < 0)
    {
        printf("Failed to start main PipeWire loop");
        return false;
    }

    pw_client_version_ = pw_get_library_version();

    // Initialize event handlers, remote end and stream-related.
    pw_core_events_.version = PW_VERSION_CORE_EVENTS;
    pw_core_events_.info = &on_core_info;
    pw_core_events_.done = &on_core_done;
    pw_core_events_.error = &on_core_error;

    pw_stream_events_.version = PW_VERSION_STREAM_EVENTS;
    pw_stream_events_.state_changed = &on_stream_state_changed;
    pw_stream_events_.param_changed = &on_streamParam_changed;
    pw_stream_events_.process = &on_stream_process;

    {
        pw_thread_loop_lock(pw_main_loop_);

        if (!pw_fd_)
        {
            pw_core_ = pw_context_connect(pw_context_, NULL, 0);
        }
        else
        {
            pw_core_ = pw_context_connect_fd(pw_context_, pw_fd_, NULL, 0);
        }

        if (!pw_core_)
        {
            printf("Failed to connect PipeWire context");
            return false;
        }

        // pw_core_add_listener(pw_core_, &spa_core_listener_, &pw_core_events_, &userdata);
        //  core_method_marshal_add_listener
        pw_proxy_add_listener(pw_core_, &spa_core_listener_, &pw_core_events_, &userdata);
        //  Add an event that can be later invoked by pw_loop_signal_event()
        struct pw_loop *loop__ = pw_thread_loop_get_loop(pw_main_loop_);
        renegotiate_ = __pw_loop_add_event(loop__,
                                           &on_renegotiate_format, &userdata);
        __pw_loop_signal_event(pw_thread_loop_get_loop(pw_main_loop_), renegotiate_);

        pw_main_loop_run(pw_main_loop_);
        /*
        server_version_sync_ =
            pw_core_sync(pw_core_, PW_ID_CORE, server_version_sync_);

        pw_thread_loop_wait(pw_main_loop_);

        pw_properties *reuseProps =
            pw_properties_new_string("pipewire.client.reuse=1");
        pw_stream_ = pw_stream_new(pw_core_, "webrtc-consume-stream", reuseProps);

        if (!pw_stream_)
        {
            printf("Failed to create PipeWire stream";
            return false;
        }
        struct spa_hook spa_stream_listener_;
        pw_stream_add_listener(pw_stream_, &spa_stream_listener_,
                               &pw_stream_events_, this);
        uint8_t buffer[2048] = {};

        spa_pod_builder builder = spa_pod_builder{buffer, sizeof(buffer)};

        std::vector<const spa_pod *> params;
        const bool has_required_pw_client_version =
            pw_client_version_ >= kDmaBufModifierMinVersion;
        const bool has_required_pw_server_version =
            pw_server_version_ >= kDmaBufModifierMinVersion;
        struct spa_rectangle resolution;
        bool set_resolution = false;
        if (width && height)
        {
            resolution = SPA_RECTANGLE(width, height);
            set_resolution = true;
        }
        for (uint32_t format : {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
                                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx})
        {
            // Modifiers can be used with PipeWire >= 0.3.33
            if (has_required_pw_client_version && has_required_pw_server_version)
            {
                modifiers_ = egl_dmabuf_->QueryDmaBufModifiers(format);

                if (!modifiers_.empty())
                {
                    params.push_back(BuildFormat(&builder, format, modifiers_,
                                                 set_resolution ? &resolution : NULL));
                }
            }

            params.push_back(BuildFormat(&builder, format, /*modifiers=*/
        /*{},
set_resolution ? &resolution : NULL));
}

if (pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, pw_stream_node_id_,
PW_STREAM_FLAG_AUTOCONNECT, params.data(),
params.size()) != 0)
{
printf("Could not connect receiving stream.";
return false;
}

std::cout << "PipeWire remote opened.";*/
    }
}

//   struct pw_loop * loop__ = pw_thread_loop_get_loop(pw_main_loop_);
//   renegotiate_ = pw_loop_add_event(loop__,
//                          &on_renegotiate_format, &userdata);
/*struct spa_source *  pw_add_event(loop, usermethod, userdata){
    int vers = 0;
    void * utils = loop->utils;
    char* method = "add_event";

    struct spa_source *_res = NULL;
    struct spa_loop_utils *_o = utils;
    typedef struct spa_loop_utils_methods method_type;
    void * iface = &_o->iface;

    void *callbacks = &(iface)->cb;

    const method_type *_f = (const method_type *) (callbacks)->funcs;
    //res = _f->method((callbacks)->data, userdata);
    res = _f->add_event((callbacks)->data, usermethod, userdata);
    res;
}
*/

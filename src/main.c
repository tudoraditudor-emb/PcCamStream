/*
 * pc-cam-stream — real-time webcam processing (C + GStreamer, Linux)
 *
 * Input   : laptop camera via v4l2src, default device format (unconstrained)
 * Process : scale -> flip -> invert colors (inversion done in C, appsink->appsrc)
 * Output  : forced to H264 (x264enc + video/x-h264 caps), decoded, shown on screen
 *
 * Two pipelines (a single pipeline with two disconnected chains would deadlock
 * in preroll, because the appsrc branch produces no buffer until it is fed):
 *
 *   pipe_in : v4l2src ! decodebin ! videoconvert ! videoscale
 *             ! video/x-raw,format=RGB,WxH ! videoflip ! appsink(name=proc)
 *                                                          |
 *                              callback in C: invert every byte (255 - v)
 *                                                          v
 *   pipe_out: appsrc(name=out) ! videoconvert ! x264enc ! video/x-h264 (forced)
 *             ! avdec_h264 ! videoconvert ! autovideosink
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  GMainLoop *loop;
  GstElement *pipe_in;
  GstElement *pipe_out;
  GstElement *appsrc;     /* in pipe_out */
  GstElement *proc;       /* appsink in pipe_in */
  gint64 frames;          /* frames pushed through the invert stage */
  gint64 max_frames;      /* 0 = run forever */
  gint64 t0;
} App;

static const gchar *
flip_method(const gchar *name)
{
  if (g_strcmp0(name, "horizontal") == 0) return "horizontal-flip";
  if (g_strcmp0(name, "vertical") == 0)   return "vertical-flip";
  if (g_strcmp0(name, "180") == 0)        return "rotate-180";
  if (g_strcmp0(name, "none") == 0)       return "none";
  return NULL;
}

/* Called for every camera frame. The capsfilter upstream forces RGB
 * (3 bytes/pixel, no row padding), so inverting every byte inverts the
 * color. The inverted frame is pushed into the encoder pipeline. */
static GstFlowReturn
on_new_sample(GstAppSink *sink, gpointer user_data)
{
  App *app = user_data;
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_EOS;

  /* the incoming buffer may be shared/read-only (pool subbuffer) — get a
   * writable copy before mapping it for writing */
  GstBuffer *buf =
      gst_buffer_make_writable(gst_buffer_ref(gst_sample_get_buffer(sample)));
  GstMapInfo map;
  if (gst_buffer_map(buf, &map, GST_MAP_READWRITE)) {
    for (gsize i = 0; i < map.size; i++)
      map.data[i] = (guint8)(255 - map.data[i]);
    gst_buffer_unmap(buf, &map);
  }

  app->frames++;
  if (app->max_frames > 0 && app->frames >= app->max_frames)
    g_main_loop_quit(app->loop);

  /* gst_app_src_push_buffer takes ownership of buf */
  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(app->appsrc), buf);
  gst_sample_unref(sample);
  return ret;
}

static gboolean
on_bus(GstBus *bus, GstMessage *msg, gpointer user_data)
{
  App *app = user_data;
  (void)bus;
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error(msg, &err, &dbg);
      g_printerr("ERROR: %s\n  debug: %s\n",
                 err ? err->message : "?", dbg ? dbg : "-");
      g_clear_error(&err);
      g_free(dbg);
      g_main_loop_quit(app->loop);
      break;
    }
    case GST_MESSAGE_EOS:
      g_main_loop_quit(app->loop);
      break;
    default:
      break;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_sigint(gpointer user_data)
{
  g_main_loop_quit(((App *)user_data)->loop);
  return G_SOURCE_REMOVE;
}

static void
watch(GstElement *pipe, App *app)
{
  GstBus *bus = gst_element_get_bus(pipe);
  gst_bus_add_watch(bus, on_bus, app);
  gst_object_unref(bus);
}

int
main(int argc, char **argv)
{
  gst_init(&argc, &argv);

  gchar *device = g_strdup("/dev/video0");
  gchar *sink   = g_strdup("autovideosink");
  gchar *flip   = g_strdup("horizontal");
  gint width = 640, height = 480, bitrate = 800;
  gint64 max_frames = 0;
  gboolean test = FALSE;

  GOptionEntry entries[] = {
    { "device",  0, 0, G_OPTION_ARG_STRING, &device,  "camera device (default /dev/video0)", "DEV" },
    { "width",   0, 0, G_OPTION_ARG_INT,    &width,   "output width in px (default 640)", "PX" },
    { "height",  0, 0, G_OPTION_ARG_INT,    &height,  "output height in px (default 480)", "PX" },
    { "flip",    0, 0, G_OPTION_ARG_STRING, &flip,    "flip: none|horizontal|vertical|180 (default horizontal)", "MODE" },
    { "bitrate", 0, 0, G_OPTION_ARG_INT,    &bitrate, "H264 bitrate in kbps (default 800)", "KBPS" },
    { "sink",    0, 0, G_OPTION_ARG_STRING, &sink,    "final sink element (default autovideosink; fakesink for headless)", "ELEM" },
    { "frames",  0, 0, G_OPTION_ARG_INT64,  &max_frames, "stop after N processed frames (0 = forever)", "N" },
    { "test",    0, 0, G_OPTION_ARG_NONE,   &test,    "use videotestsrc instead of the camera", NULL },
    { NULL }
  };

  GOptionContext *ctx = g_option_context_new("- camera -> process -> H264 -> screen");
  g_option_context_add_main_entries(ctx, entries, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
    g_printerr("invalid options: %s\n", err->message);
    return 2;
  }

  const gchar *method = flip_method(flip);
  if (!method) {
    g_printerr("invalid flip mode '%s' (none|horizontal|vertical|180)\n", flip);
    return 2;
  }
  if (width < 16 || height < 16 || bitrate < 1) {
    g_printerr("invalid width/height/bitrate\n");
    return 2;
  }
  if (!gst_element_factory_find("x264enc")) {
    g_printerr("x264enc not found - install gstreamer1.0-plugins-ugly\n");
    return 1;
  }
  const gchar *decoder =
      gst_element_factory_find("avdec_h264") ? "avdec_h264" : "decodebin";

  gchar *src = test ? g_strdup("videotestsrc is-live=true")
                    : g_strdup_printf("v4l2src device=%s", device);

  gchar *desc_in = g_strdup_printf(
      "%s ! decodebin ! videoconvert ! videoscale ! "
      "video/x-raw,format=RGB,width=%d,height=%d ! "
      "videoflip method=%s ! "
      "appsink name=proc emit-signals=true max-buffers=2 drop=false sync=false",
      src, width, height, method);

  gchar *desc_out = g_strdup_printf(
      "appsrc name=out is-live=true do-timestamp=true format=time "
      "caps=\"video/x-raw,format=RGB,width=%d,height=%d\" ! "
      "videoconvert ! "
      "x264enc tune=zerolatency speed-preset=ultrafast bitrate=%d key-int-max=60 ! "
      "video/x-h264,stream-format=byte-stream,alignment=au ! "
      "%s ! videoconvert ! %s sync=false",
      width, height, bitrate, decoder, sink);

  g_print("pipe_in : %s\npipe_out: %s\n\n", desc_in, desc_out);

  App app = {0};
  app.max_frames = max_frames;
  app.loop = g_main_loop_new(NULL, FALSE);

  app.pipe_in  = gst_parse_launch(desc_in, &err);
  if (!app.pipe_in) {
    g_printerr("cannot build input pipeline: %s\n", err ? err->message : "?");
    return 1;
  }
  app.pipe_out = gst_parse_launch(desc_out, &err);
  if (!app.pipe_out) {
    g_printerr("cannot build output pipeline: %s\n", err ? err->message : "?");
    return 1;
  }

  app.proc   = gst_bin_get_by_name(GST_BIN(app.pipe_in), "proc");
  app.appsrc = gst_bin_get_by_name(GST_BIN(app.pipe_out), "out");
  g_signal_connect(app.proc, "new-sample", G_CALLBACK(on_new_sample), &app);

  watch(app.pipe_in, &app);
  watch(app.pipe_out, &app);

  g_unix_signal_add(SIGINT, on_sigint, &app);
  g_unix_signal_add(SIGTERM, on_sigint, &app);

  app.t0 = g_get_monotonic_time();
  if (gst_element_set_state(app.pipe_in, GST_STATE_PLAYING) ==
          GST_STATE_CHANGE_FAILURE ||
      gst_element_set_state(app.pipe_out, GST_STATE_PLAYING) ==
          GST_STATE_CHANGE_FAILURE) {
    g_printerr("cannot start pipelines\n");
    return 1;
  }

  g_main_loop_run(app.loop);

  /* graceful stop: EOS into both pipelines, wait for the output pipeline
   * to drain (so e.g. a filesink receives all frames), then tear down */
  gst_element_send_event(app.pipe_in, gst_event_new_eos());
  gst_app_src_end_of_stream(GST_APP_SRC(app.appsrc));
  GstBus *out_bus = gst_element_get_bus(app.pipe_out);
  gst_bus_timed_pop_filtered(out_bus, 3 * GST_SECOND,
                             GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  gst_object_unref(out_bus);

  gst_element_set_state(app.pipe_in, GST_STATE_NULL);
  gst_element_set_state(app.pipe_out, GST_STATE_NULL);
  gdouble dt = (g_get_monotonic_time() - app.t0) / 1e6;
  g_print("Processed %" G_GINT64_FORMAT " frames in %.2f s (%.1f fps)\n",
          app.frames, dt, dt > 0 ? app.frames / dt : 0.0);

  gst_object_unref(app.proc);
  gst_object_unref(app.appsrc);
  gst_object_unref(app.pipe_in);
  gst_object_unref(app.pipe_out);
  g_main_loop_unref(app.loop);
  return 0;
}

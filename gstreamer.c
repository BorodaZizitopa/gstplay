/*
    gstplay -- Simple gstreamer-based media player

    Copyright 2013 Harm Hanemaaijer <fgenfb@yahoo.com>

    gstplay is free software: you can redistribute it and/or modify it
    under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    gstplay is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with gstplay.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#if GST_CHECK_VERSION(1, 0, 0)
#include <gst/video/videooverlay.h>
#else
#include <gst/interfaces/xoverlay.h>
#endif
#if GST_CHECK_VERSION(1, 0, 0)
#include <gst/video/colorbalance.h>
#else
#include <gst/interfaces/colorbalance.h>
#endif
#include <glib.h>
#include "gstplay.h"

/* The constants below don't appear to be defined in any standard header files. */
typedef enum {
  GST_PLAY_FLAG_VIDEO         = (1 << 0),
  GST_PLAY_FLAG_AUDIO         = (1 << 1),
  GST_PLAY_FLAG_TEXT          = (1 << 2),
  GST_PLAY_FLAG_VIS           = (1 << 3),
  GST_PLAY_FLAG_SOFT_VOLUME   = (1 << 4),
  GST_PLAY_FLAG_NATIVE_AUDIO  = (1 << 5),
  GST_PLAY_FLAG_NATIVE_VIDEO  = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD      = (1 << 7),
  GST_PLAY_FLAG_BUFFERING     = (1 << 8),
  GST_PLAY_FLAG_DEINTERLACE   = (1 << 9),
  GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10)
} GstPlayFlags;

#if GST_CHECK_VERSION(1, 0, 0)
#define GSTREAMER_X_OVERLAY GstVideoOverlay
#define PLAYBIN_STR "playbin"
#else
#define GSTREAMER_X_OVERLAY GstXOverlay
#define PLAYBIN_STR "playbin2"

/* Definitions for compatibility with GStreamer 0.10. */
#define GST_VIDEO_OVERLAY(x) GST_X_OVERLAY(x)

static inline void gst_video_overlay_expose(GstXOverlay *overlay) {
	gst_x_overlay_expose(overlay);
}

static inline void gst_video_overlay_set_window_handle(GstXOverlay *overlay, guintptr handle) {
	gst_x_overlay_set_window_handle(overlay, handle);
}

static inline gboolean gst_is_video_overlay_prepare_window_handle_message(GstMessage *message) {
	if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
		return FALSE;
	if (!gst_structure_has_name(message->structure, "prepare-xwindow-id"))
		return FALSE;
	return TRUE;
}

static inline GstCaps *gst_pad_get_current_caps(GstPad *pad) {
	return gst_pad_get_negotiated_caps(pad);
}

#endif

static GSTREAMER_X_OVERLAY *video_window_overlay = NULL;
static GstElement *playbin_pipeline;
static GstElement *pipeline;
static guint bus_watch_id;
static gboolean bus_quit_on_playing = FALSE;
static gboolean set_default_settings_on_playing = FALSE;
static GList *created_pads_list = NULL;
static const char *pipeline_description = "";
static GstState suspended_state;
static GstClockTime suspended_pos;
static GstClockTime requested_position;
static gdouble suspended_audio_volume;
static gboolean end_of_stream = FALSE;
static gboolean using_playbin;
static GList *inform_pipeline_destroyed_cb_list;

void gstreamer_expose_video_overlay() {
	if (video_window_overlay == NULL)
		return;
	gst_video_overlay_expose(video_window_overlay);
}

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
	GMainLoop *loop = (GMainLoop *)data;

	switch (GST_MESSAGE_TYPE (msg)) {
	case GST_MESSAGE_EOS:
		if (config_quit_on_stream_end() || !main_have_gui()) {
			gstreamer_destroy_pipeline();
			g_main_loop_quit(loop);
		}
		end_of_stream = TRUE;
		break;
	case GST_MESSAGE_ERROR: {
		gchar  *debug;
		GError *error;

		gst_message_parse_error(msg, &error, &debug);
		g_free(debug);

		gstreamer_destroy_pipeline();

		main_show_error_message(
			"Processing error (unrecognized format or other error).",
			error->message);
		g_error_free (error);
		break;
		}
	case GST_MESSAGE_STATE_CHANGED:
		if (bus_quit_on_playing) {
			// When doing the initial run to determine video parameters,
			// stop immediately when play starts.
			if (GST_STATE(playbin_pipeline) == GST_STATE_PLAYING)
				g_main_loop_quit(loop);
		}
		if (set_default_settings_on_playing &&
		GST_STATE(pipeline) == GST_STATE_PLAYING) {
			gstreamer_set_default_settings();
			set_default_settings_on_playing = FALSE;
		}
		break;
	case GST_MESSAGE_BUFFERING:
		if (bus_quit_on_playing)
			break;
		gint percent = 0;
		gst_message_parse_buffering(msg, &percent);
		if (percent < 100) {
			if (GST_STATE(pipeline) != GST_STATE_PAUSED)
				gst_element_set_state(pipeline, GST_STATE_PAUSED);
//			printf("Buffering (%u percent done)\n", percent);
		}
		else {
			if (GST_STATE(pipeline) != GST_STATE_PLAYING)
				gst_element_set_state(pipeline, GST_STATE_PLAYING);
		}
		break;
	case GST_MESSAGE_APPLICATION: {
	        const GstStructure *s;
	        s = gst_message_get_structure (msg);
	        if (gst_structure_has_name (s, "GstLaunchInterrupt")) {
			/* This application message is posted when we caught an interrupt and
			 * we need to stop the pipeline. */
			printf("gstplay: Interrupt: Stopping pipeline ...\n");
			fflush(stdout);
			fflush(stderr);
			gstreamer_destroy_pipeline();
	          	g_main_loop_quit(loop);
		}
		break;
        }
	default:
		break;
	}

	return TRUE;
}

static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
	if (!gst_is_video_overlay_prepare_window_handle_message(msg))
		return GST_BUS_PASS;
	guintptr video_window_handle = gui_get_video_window_handle();
	g_assert(video_window_handle != 0);
	// GST_MESSAGE_SRC (message) will be the video sink element.
	video_window_overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
	gst_video_overlay_set_window_handle(video_window_overlay, video_window_handle);
	return GST_BUS_DROP;
}

void gstreamer_init(int *argcp, char **argvp[]) {
	gst_init(argcp, argvp);
	guint major, minor, micro, nano;
	gst_version(&major, &minor, &micro, &nano);
	if (major != GST_VERSION_MAJOR) {
		printf("Error: gstreamer API major version is not %d (version %d.%d found).\n",
			GST_VERSION_MAJOR, major, minor);
		exit(1);
	}
	if (minor != GST_VERSION_MINOR) {
		printf("Warning: gstreamer API version is not %d.%d (version %d.%d found).\n",
			GST_VERSION_MAJOR, GST_VERSION_MINOR, major, minor);
	}
}

// Return the dimensions of a non-running pipeline by test-running it briefly.

void gstreamer_determine_video_dimensions(const char *uri, int *video_width,
int *video_height) {
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	char *playbin_launch_str = malloc(strlen(uri) + 64);
	sprintf(playbin_launch_str, PLAYBIN_STR
		" uri=%s audio-sink=fakesink video-sink=fakesink", uri);
	GError *error2 = NULL;
	GstElement *playbin = gst_parse_launch(playbin_launch_str, &error2);
	if (error2) {
		printf("Error: Could not create gstreamer pipeline for identification.\n");
		printf("Parse error: %s\n", error2->message);
		exit(1);
	}

	playbin_pipeline = playbin;
	bus_quit_on_playing = TRUE;
	GstBus *playbin_bus = gst_pipeline_get_bus(GST_PIPELINE(playbin));
	guint type_find_bus_watch_id = gst_bus_add_watch(playbin_bus, bus_callback, loop);
	gst_object_unref(playbin_bus);

	gst_element_set_state(GST_ELEMENT(playbin), GST_STATE_READY);
	gst_element_set_state(GST_ELEMENT(playbin), GST_STATE_PLAYING);
	g_main_loop_run(loop);
	gst_element_set_state(GST_ELEMENT(playbin), GST_STATE_PAUSED);

	GstPad *pad = gst_pad_new("", GST_PAD_UNKNOWN);
	g_signal_emit_by_name(playbin, "get-video-pad", 0, &pad, NULL);
	GstCaps *caps = gst_pad_get_current_caps(pad);
	*video_width = g_value_get_int(gst_structure_get_value(
		gst_caps_get_structure(caps, 0), "width"));
	*video_height = g_value_get_int(gst_structure_get_value(
		gst_caps_get_structure(caps, 0), "height"));
	g_object_unref(pad);

	gst_element_set_state(GST_ELEMENT(playbin), GST_STATE_NULL);
	gst_object_unref(GST_OBJECT(playbin));
	g_source_remove(type_find_bus_watch_id);
	g_main_loop_unref(loop);
}


static void read_video_props(GstCaps *caps, const char **formatp, int *widthp, int *heightp,
int *framerate_numeratorp, int *framerate_denomp, int *pixel_aspect_ratio_nump,
int *pixel_aspect_ratio_denomp) {
	const GstStructure *str;
	if (!gst_caps_is_fixed(caps))
		return;
	str = gst_caps_get_structure (caps, 0);
	int width = 0;
	int height = 0;
	int framerate_numerator = 0;
	int framerate_denom = 0;
	int pixel_aspect_ratio_num = 0;
	int pixel_aspect_ratio_denom = 0;

	const char *format = gst_structure_get_string(str, "format");
	gst_structure_get_int(str, "width", widthp);
	gst_structure_get_int(str, "height", heightp);
	gst_structure_get_fraction(str, "pixel-aspect-ratio", pixel_aspect_ratio_nump,
		pixel_aspect_ratio_denomp);
	gst_structure_get_fraction(str, "framerate", framerate_numeratorp, framerate_denomp);

	if (format != NULL)
		*formatp = format;
	if (width != 0)
		*widthp = width;
	if (height != 0)
		*heightp = height;
	if (pixel_aspect_ratio_num != 0 && pixel_aspect_ratio_denom != 0) {
		*pixel_aspect_ratio_nump = pixel_aspect_ratio_num;
		*pixel_aspect_ratio_denomp = pixel_aspect_ratio_denom;
	}
	if (framerate_numerator != 0 && framerate_denom != 0) {
		*framerate_numeratorp = framerate_numerator;
		*framerate_denomp = framerate_denom;
	}
}

// Return info about a running pipeline.

extern void gstreamer_get_video_info(const char **formatp, int *widthp, int *heightp,
int *framerate_numeratorp, int *framerate_denomp, int *pixel_aspect_ratio_nump,
int *pixel_aspect_ratio_denomp) {
	*formatp = NULL;
	*widthp = 0;
	*heightp = 0;
	*framerate_numeratorp = 0;
	*framerate_denomp = 0;
	*pixel_aspect_ratio_nump = 0;
	*pixel_aspect_ratio_denomp = 0;
	GList *list = g_list_first(created_pads_list);
	while (list != NULL) {
		GstPad *pad = list->data;
		GstCaps *caps = gst_pad_get_current_caps(pad);
		if (GST_IS_CAPS(caps))
			read_video_props(caps, formatp, widthp, heightp, framerate_numeratorp,
				framerate_denomp, pixel_aspect_ratio_nump,
				pixel_aspect_ratio_denomp);
		list = g_list_next(list);
	}
}

extern void gstreamer_get_video_dimensions(int *widthp, int *heightp) {
	const char *format = NULL;
	*widthp = 0;
	*heightp = 0;
	int framerate_numerator = 0;
	int framerate_denom = 0;
	int pixel_aspect_ratio_num = 0;
	int pixel_aspect_ratio_denom = 0;
	GList *list = g_list_first(created_pads_list);
	while (list != NULL) {
		GstPad *pad = list->data;
		GstCaps *caps = gst_pad_get_current_caps(pad);
		if (GST_IS_CAPS(caps))
			read_video_props(caps, &format, widthp, heightp, &framerate_numerator,
				&framerate_denom, &pixel_aspect_ratio_num,
				&pixel_aspect_ratio_denom);
		list = g_list_next(list);
	}
}

const char *gstreamer_get_pipeline_description() {
	return pipeline_description;
}

static void new_pad_cb(GstElement *element, GstPad *pad, gpointer data) {
	gchar *name;

	created_pads_list = g_list_append(created_pads_list, pad);
}

#if GST_CHECK_VERSION(1, 0, 0)
void for_each_pipeline_element(const GValue *value, gpointer data) {
	GstElement *element = g_value_get_object(value);
#else
void for_each_pipeline_element(gpointer value_data, gpointer data) {
	GstElement *element = value_data;
#endif

	/* Listen for newly created pads. */
	g_signal_connect(element, "pad-added", G_CALLBACK(new_pad_cb), NULL);
}

gboolean gstreamer_run_pipeline(GMainLoop *loop, const char *s, StartupState state) {
	GError *error = NULL;
	pipeline = gst_parse_launch(s, &error);
	if (!pipeline) {
		printf("Error: Could not create gstreamer pipeline.\n");
		printf("Parse error: %s\n", error->message);
		return FALSE;
	}

	bus_quit_on_playing = FALSE;
	GstBus *bus;
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, bus_callback, loop);
	if (main_have_gui())
#if GST_CHECK_VERSION(1, 0, 0)
		gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler, NULL, NULL);
#else
		gst_bus_set_sync_handler(bus, (GstBusSyncHandler)bus_sync_handler, NULL);
#endif
	gst_object_unref(bus);

	// Iterate over all elements of the pipeline to hook a handler
	// listing newly created pads.
	if (created_pads_list != NULL) {
		g_list_free(created_pads_list);
		created_pads_list = NULL;
	}
	GstIterator *iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
	gst_iterator_foreach(iterator, for_each_pipeline_element, NULL);
	gst_iterator_free(iterator);

	gst_element_set_state(pipeline, GST_STATE_READY);

	set_default_settings_on_playing = TRUE;

	if (state == STARTUP_PLAYING)
		gst_element_set_state(pipeline, GST_STATE_PLAYING);
	else
		gst_element_set_state(pipeline, GST_STATE_PAUSED);

	pipeline_description = s;
	end_of_stream = FALSE;

	inform_pipeline_destroyed_cb_list = NULL;
	return TRUE;
}

void gstreamer_destroy_pipeline() {
	GstState state, pending;
	gst_element_set_state (pipeline, GST_STATE_PAUSED);
	gst_element_get_state (pipeline, &state, &pending, GST_CLOCK_TIME_NONE);

	/* Iterate main loop to process pending stuff. */
	while (g_main_context_iteration (NULL, FALSE));

	gst_element_set_state (pipeline, GST_STATE_READY);
	gst_element_get_state (pipeline, &state, &pending, GST_CLOCK_TIME_NONE);

	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_source_remove(bus_watch_id);
	gst_object_unref(GST_OBJECT(pipeline));
	pipeline_description = "";

	GList *list = g_list_first(inform_pipeline_destroyed_cb_list);
	GValue value = G_VALUE_INIT;
	g_value_init(&value, G_TYPE_POINTER);
	g_value_set_pointer(&value, NULL);
	while (list != NULL) {
		GClosure *closure = list->data;
		g_closure_invoke(closure, NULL, 1, &value, NULL);
		g_closure_unref(closure);
		list = g_list_next(list);
	}
	g_value_unset(&value);
	g_list_free(inform_pipeline_destroyed_cb_list);
}

void gstreamer_add_pipeline_destroyed_cb(GCallback cb_func, gpointer user_data) {
	GClosure *closure = g_cclosure_new(cb_func, user_data, NULL);
	g_closure_set_marshal(closure, g_cclosure_marshal_VOID__VOID);
	inform_pipeline_destroyed_cb_list = g_list_append(inform_pipeline_destroyed_cb_list,
		closure);
}

void gstreamer_play() {
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void gstreamer_pause() {
	gst_element_set_state(pipeline, GST_STATE_PAUSED);
}

static GstState gstreamer_get_state() {
	GstState state = GST_STATE_VOID_PENDING;
	gst_element_get_state(pipeline, &state, NULL, 0);
	return state;
}

gint64 gstreamer_get_position(gboolean *error) {
	gint64 pos, len;

	if (end_of_stream) {
#if GST_CHECK_VERSION(1, 0, 0)
		if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &len)) {
#else
		GstFormat format = GST_FORMAT_TIME;
		if (gst_element_query_duration(pipeline, &format, &len)) {
#endif
			if (error != NULL)
				*error = FALSE;
			return len;
		}
		if (error != NULL)
			*error = TRUE;
		return 0;
	}

#if GST_CHECK_VERSION(1, 0, 0)
	if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos)) {
		if (gst_element_query_duration (pipeline, GST_FORMAT_TIME, &len)) {
#else
	GstFormat format = GST_FORMAT_TIME;
	if (gst_element_query_position(pipeline, &format, &pos)) {
		GstFormat format2 = GST_FORMAT_TIME;
		if (gst_element_query_duration(pipeline, &format2, &len)) {
#endif
			if (error != NULL)
				*error = FALSE;
			return pos;
		}
	}
	printf("gstplay: Could not succesfully query current position.\n");
	if (error != NULL)
		*error = TRUE;
	return 0;
}

gint64 gstreamer_get_duration() {
	GstQuery *query;
	gboolean res;
	query = gst_query_new_duration (GST_FORMAT_TIME);
	res = gst_element_query (pipeline, query);
	gint64 duration;
	if (res) {
		gst_query_parse_duration(query, NULL, &duration);
	}
	else
		duration = 0;
	gst_query_unref (query);
	return duration;
}

const gchar *gstreamer_get_duration_str() {
	gint64 duration = gstreamer_get_duration();
	char s[80];
	sprintf(s, "%"GST_TIME_FORMAT, GST_TIME_ARGS(duration));
	return strdup(s);
}

static gboolean seek_to_time_cb(gpointer data) {
	gstreamer_seek_to_time(requested_position);
	gstreamer_set_volume(suspended_audio_volume);
	return FALSE;
}

void gstreamer_seek_to_time(gint64 time_nanoseconds) {
	end_of_stream = FALSE;
	if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
	GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, time_nanoseconds)) {
		printf("gstplay: Seek failed!.n");
	}
}

gboolean gstreamer_end_of_stream() {
	return end_of_stream;
}

gboolean gstreamer_no_pipeline() {
	return strlen(pipeline_description) == 0;
}

gboolean gstreamer_no_video() {
	if (gstreamer_no_pipeline())
		return TRUE;
	int width, height;
	gstreamer_get_video_dimensions(&width, &height);
	if (width == 0 || height == 0)
		return TRUE;
	return FALSE;
}

void gstreamer_suspend_pipeline() {
	if (gstreamer_no_pipeline()) {
		suspended_state = GST_STATE_NULL;
		return;
	}
	/* Save the current position and wind down the pipeline. */
	suspended_pos = gstreamer_get_position(NULL);
	suspended_state = gstreamer_get_state();
	suspended_audio_volume = gstreamer_get_volume();
	gstreamer_pause();
	gstreamer_destroy_pipeline();
}

void gstreamer_restart_pipeline() {
	if (suspended_state == GST_STATE_NULL)
		return;
	/* Restart the pipeline. */
	const char *uri;
	const char *video_title_filename;
	main_get_current_uri(&uri, &video_title_filename);
	const char *pipeline_str = main_create_pipeline(uri, video_title_filename);
	StartupState startup;
	if (suspended_state == GST_STATE_PLAYING)
		startup = STARTUP_PLAYING;
	else
		startup = STARTUP_PAUSED;
	gstreamer_run_pipeline(main_get_main_loop(), pipeline_str, startup);
	requested_position = suspended_pos;
	g_timeout_add_seconds(1, seek_to_time_cb, NULL);
}

void gstreamer_inform_playbin_used(gboolean status) {
	using_playbin = status;
}

gdouble gstreamer_get_volume() {
	if (!using_playbin) {
		printf("gst_play: Could not get audio volume because playbin is not used.\n");
		return 0;
	}
	gdouble volume;
	g_object_get(G_OBJECT(pipeline), "volume", &volume, NULL);
	return volume;
}

void gstreamer_set_volume(gdouble volume) {
	if (!using_playbin) {
		printf("gst_play: Could not set audio volume because playbin is not used.\n");
		return;
	}
	g_object_set(G_OBJECT(pipeline), "volume", volume, NULL);
}

gboolean gstreamer_have_software_color_balance() {
	return TRUE;
}

void gstreamer_get_version(guint *major, guint *minor, guint *micro) {
	guint nano;
	gst_version(major, minor, micro, &nano);
}

void gstreamer_get_compiled_version(guint *major, guint *minor, guint *micro) {
	*major = GST_VERSION_MAJOR;
	*minor = GST_VERSION_MINOR;
	*micro = GST_VERSION_MICRO;
}

#if !GST_CHECK_VERSION(1, 0, 0)

static gboolean is_valid_color_balance_element(GstElement *element) {
	return TRUE;
}

static GstElement *find_color_balance_element() {
	GstIterator *iterator = gst_bin_iterate_all_by_interface(
		GST_BIN(pipeline),  GST_TYPE_COLOR_BALANCE);
	
	GstElement *color_balance_element = NULL;
	gboolean done = FALSE, hardware = FALSE;
	gpointer item;
	switch (gst_iterator_next(iterator, &item)) {
	case GST_ITERATOR_OK : {
		GstElement *element = GST_ELEMENT(item);
		if (is_valid_color_balance_element(element)) {
			if (!color_balance_element) {
				color_balance_element = GST_ELEMENT_CAST(	
					gst_object_ref(element));
				hardware =
					(gst_color_balance_get_balance_type(GST_COLOR_BALANCE
					(element)) == GST_COLOR_BALANCE_HARDWARE);
			}
			else if (!hardware) {
				gboolean tmp =
					(gst_color_balance_get_balance_type(GST_COLOR_BALANCE
					(element)) == GST_COLOR_BALANCE_HARDWARE);

				if (tmp) {
					if (color_balance_element)
						gst_object_unref(color_balance_element);
					color_balance_element =
						GST_ELEMENT_CAST(gst_object_ref(element));
					hardware = TRUE;
				}
			}
		}
		gst_object_unref(element);
		if (hardware && color_balance_element)
			done = TRUE;
        	break;
		}
	case GST_ITERATOR_RESYNC :
		gst_iterator_resync(iterator);
		done = FALSE;
		hardware = FALSE;
		if (color_balance_element)
			gst_object_unref(color_balance_element);
		color_balance_element = NULL;
		break;
	case GST_ITERATOR_DONE:
	case GST_ITERATOR_ERROR:
	default:
		done = TRUE;
	}
	gst_iterator_free(iterator);
	return color_balance_element;
}
	
#endif

static GstElement *color_balance_element;
static GstColorBalanceChannel *color_balance_channel[4];
static gint last_value_set[4];

int gstreamer_prepare_color_balance() {
#if GST_CHECK_VERSION(1, 0, 0)
	color_balance_element = pipeline;
#else
	color_balance_element = find_color_balance_element();
	if (color_balance_element == NULL)
		return 0;
#endif
	if (!GST_IS_COLOR_BALANCE(color_balance_element))
		return 0;
	GList *channel_list = gst_color_balance_list_channels(
		GST_COLOR_BALANCE(color_balance_element));
	if (channel_list == NULL)
		return 0;
	for (int i = 0; i < 4; i++)
		color_balance_channel[i] = NULL;
	channel_list = g_list_first(channel_list);
	while (channel_list) {
		GstColorBalanceChannel *channel = channel_list->data;
		if (strcmp(channel->label, "BRIGHTNESS") == 0)
			color_balance_channel[CHANNEL_BRIGHTNESS] = channel;
		else if (strcmp(channel->label, "CONTRAST") == 0)
			color_balance_channel[CHANNEL_CONTRAST] = channel;
		else if (strcmp(channel->label, "HUE") == 0)
			color_balance_channel[CHANNEL_HUE] = channel;
		else if (strcmp(channel->label, "SATURATION") == 0)
			color_balance_channel[CHANNEL_SATURATION] = channel;
		channel_list = g_list_next(channel_list);
	}
	int r;
	for (int i = 0; i < 4; i++) {
		if (color_balance_channel[i] != NULL) {
			r |= 1 << i;
			last_value_set[i] = gst_color_balance_get_value(
				GST_COLOR_BALANCE(color_balance_element), color_balance_channel[i]);
		}
	}
	return r;
}

void gstreamer_set_color_balance(int channel, gdouble value) {
	if (color_balance_channel[channel] == NULL)
		return;
	gint v = color_balance_channel[channel]->min_value
		+ value * 0.01 * (color_balance_channel[channel]->max_value -
		color_balance_channel[channel]->min_value);
	if (v != last_value_set[channel]) {
		gst_color_balance_set_value(GST_COLOR_BALANCE(color_balance_element),
			color_balance_channel[channel], v);
		last_value_set[channel] = v;
	}
}

gdouble gstreamer_get_color_balance(int channel) {
	if (color_balance_channel[channel] == NULL) {
		printf("gstplay: Could not read color balance channel.\n");
		return - 1.0;
	}
	gdouble v = gst_color_balance_get_value(GST_COLOR_BALANCE(color_balance_element),
		color_balance_channel[channel]);
	// Normalize to [0, 100].
	return (v - color_balance_channel[channel]->min_value) * 100.0 /
		(color_balance_channel[channel]->max_value -
		color_balance_channel[channel]->min_value);
}

void gstreamer_set_default_settings() {
	if (!config_software_color_balance())
		return;
	gstreamer_prepare_color_balance();
	for (int i = 0; i < 4; i++)
		gstreamer_set_color_balance(i, config_get_global_color_balance_default(i));
}

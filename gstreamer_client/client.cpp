#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <cstring>

#define STUN_SERVER "stun://stun.l.google.com:19302"

struct PendingIce {
    guint mlineindex;
    gchar *candidate;
};

struct AppState {
    GstElement *pipeline = nullptr;
    GstElement *webrtcbin = nullptr;
    SoupSession *soup = nullptr;
    GMainLoop *loop = nullptr;
    gchar *server_url = nullptr;
    gchar *webrtc_id = nullptr;
    bool remote_desc_set = false;
    GQueue *pending_ice = nullptr;
    GPtrArray *mids = nullptr;
};

static void log_msg(const gchar *msg) {
    g_print("[client] %s\n", msg);
}

static gchar *json_node_to_string(JsonNode *root) {
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *data = json_generator_to_data(gen, nullptr);
    g_object_unref(gen);
    return data;
}

static JsonNode *post_json(AppState *app, const gchar *path, JsonNode *payload) {
    gchar *url = g_strdup_printf("%s%s", app->server_url, path);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);

    gchar *body = json_node_to_string(payload);
    GBytes *request_body = g_bytes_new_take(body, std::strlen(body));
    soup_message_set_request_body_from_bytes(msg, "application/json", request_body);
    g_bytes_unref(request_body);

    GError *error = nullptr;
    GBytes *response = soup_session_send_and_read(app->soup, msg, nullptr, &error);
    if (!response) {
        g_printerr("[client] HTTP error on %s: %s\n", path, error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
        g_object_unref(msg);
        return nullptr;
    }

    JsonParser *parser = json_parser_new();
    gsize resp_len = 0;
    const gchar *resp_body = static_cast<const gchar *>(g_bytes_get_data(response, &resp_len));
    if (!json_parser_load_from_data(parser, resp_body, resp_len, nullptr)) {
        g_object_unref(parser);
        g_bytes_unref(response);
        g_object_unref(msg);
        return nullptr;
    }

    JsonNode *root = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser);
    g_bytes_unref(response);
    g_object_unref(msg);
    return root;
}

static void queue_ice(AppState *app, guint mlineindex, const gchar *candidate) {
    if (!app->pending_ice) {
        app->pending_ice = g_queue_new();
    }
    PendingIce *pending = g_new0(PendingIce, 1);
    pending->mlineindex = mlineindex;
    pending->candidate = g_strdup(candidate);
    g_queue_push_tail(app->pending_ice, pending);
    g_print("[client] Queuing ICE candidate until remote description is set\n");
}

static void flush_pending_ice(AppState *app);

static void send_ice_candidate(AppState *app, guint mlineindex, const gchar *candidate) {
    if (!app->remote_desc_set) {
        queue_ice(app, mlineindex, candidate);
        return;
    }

    const gchar *sdp_mid = nullptr;
    if (app->mids && mlineindex < app->mids->len) {
        sdp_mid = static_cast<const gchar *>(g_ptr_array_index(app->mids, mlineindex));
    }
    if (!sdp_mid || !*sdp_mid) {
        sdp_mid = "0";
    }

    if (std::strstr(candidate, " TCP ") != nullptr || std::strstr(candidate, " tcptype ") != nullptr) {
        g_print("[client] Dropping ICE TCP candidate (mid=%s)\n", sdp_mid);
        return;
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_add_string_value(builder, candidate);
    json_builder_set_member_name(builder, "sdpMid");
    json_builder_add_string_value(builder, sdp_mid);
    json_builder_set_member_name(builder, "sdpMLineIndex");
    json_builder_add_int_value(builder, static_cast<int>(mlineindex));
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "webrtc_id");
    json_builder_add_string_value(builder, app->webrtc_id);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = post_json(app, "/webrtc/ice", root);
    if (response) {
        json_node_free(response);
    }
    json_node_free(root);
    g_object_unref(builder);
}

static void flush_pending_ice(AppState *app) {
    while (app->pending_ice && !g_queue_is_empty(app->pending_ice)) {
        PendingIce *p = static_cast<PendingIce *>(g_queue_pop_head(app->pending_ice));
        send_ice_candidate(app, p->mlineindex, p->candidate);
        g_free(p->candidate);
        g_free(p);
    }
}

static void force_setup_active(GstSDPMessage *sdp) {
    guint media_len = gst_sdp_message_medias_len(sdp);
    for (guint i = 0; i < media_len; ++i) {
        GstSDPMedia *media = (GstSDPMedia *)gst_sdp_message_get_media(sdp, i);
        if (!media) {
            continue;
        }
        bool replaced = false;
        for (gint j = (gint)gst_sdp_media_attributes_len(media) - 1; j >= 0; --j) {
            const GstSDPAttribute *attr = gst_sdp_media_get_attribute(media, j);
            if (attr && g_strcmp0(attr->key, "setup") == 0) {
                gst_sdp_media_remove_attribute(media, j);
                gst_sdp_media_add_attribute(media, "setup", "active");
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            gst_sdp_media_add_attribute(media, "setup", "active");
        }
    }
}

static void on_offer_created(GstPromise *promise, gpointer user_data) {
    AppState *app = static_cast<AppState *>(user_data);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    if (!offer) {
        g_printerr("[client] Failed to create offer\n");
        return;
    }

    if (app->mids) {
        g_ptr_array_set_size(app->mids, 0);
        guint media_len = gst_sdp_message_medias_len(offer->sdp);
        g_print("[client] SDP media count: %u\n", media_len);
        for (guint i = 0; i < media_len; ++i) {
            const GstSDPMedia *media = gst_sdp_message_get_media(offer->sdp, i);
            const gchar *mid = media ? gst_sdp_media_get_attribute_val(media, "mid") : nullptr;
            g_ptr_array_add(app->mids, mid ? g_strdup(mid) : g_strdup(""));
            g_print("[client] SDP mid for mline %u: %s\n", i, mid ? mid : "(empty)");
        }
    }

    force_setup_active(offer->sdp);

    GstPromise *set_promise = gst_promise_new();
    g_signal_emit_by_name(app->webrtcbin, "set-local-description", offer, set_promise);
    gst_promise_interrupt(set_promise);
    gst_promise_unref(set_promise);

    gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);
    const gchar *setup = std::strstr(sdp_str, "a=setup:");
    if (setup) {
        const gchar *end = std::strchr(setup, '\n');
        if (end) {
            g_print("[client] Offer %.*s\n", (int)(end - setup), setup);
        } else {
            g_print("[client] Offer %s\n", setup);
        }
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, sdp_str);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "offer");
    json_builder_set_member_name(builder, "webrtc_id");
    json_builder_add_string_value(builder, app->webrtc_id);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = post_json(app, "/webrtc/offer", root);

    if (response) {
        JsonObject *obj = json_node_get_object(response);
        const gchar *answer_sdp = json_object_get_string_member(obj, "sdp");
        const gchar *answer_type = json_object_get_string_member(obj, "type");
        if (answer_sdp && answer_type && g_strcmp0(answer_type, "answer") == 0) {
            GstSDPMessage *sdp = nullptr;
            gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(answer_sdp), std::strlen(answer_sdp), sdp);
            GstWebRTCSessionDescription *answer =
                gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
            GstPromise *remote_promise = gst_promise_new();
            g_signal_emit_by_name(app->webrtcbin, "set-remote-description", answer, remote_promise);
            gst_promise_interrupt(remote_promise);
            gst_promise_unref(remote_promise);
            gst_webrtc_session_description_free(answer);
            app->remote_desc_set = true;
            flush_pending_ice(app);
        } else {
            g_printerr("[client] Invalid answer from server\n");
        }
        json_node_free(response);
    } else {
        g_printerr("[client] Failed to get answer from server\n");
    }

    json_node_free(root);
    g_object_unref(builder);
    g_free(sdp_str);
    gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *webrtcbin, gpointer user_data) {
    AppState *app = static_cast<AppState *>(user_data);
    log_msg("Negotiation needed");
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, app, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
}

static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex, gchar *candidate, gpointer user_data) {
    (void)webrtcbin;
    AppState *app = static_cast<AppState *>(user_data);
    if (candidate && *candidate) {
        const gchar *mid = nullptr;
        if (app->mids && mlineindex < app->mids->len) {
            mid = static_cast<const gchar *>(g_ptr_array_index(app->mids, mlineindex));
        }
        if (!mid || !*mid) {
            mid = "0";
        }
        g_print("[client] ICE candidate gathered (mline=%u, mid=%s): %s\n", mlineindex, mid, candidate);
        send_ice_candidate(app, mlineindex, candidate);
    }
}

static void on_webrtc_state_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    GstWebRTCICEGatheringState ice_state;
    GstWebRTCICEConnectionState ice_conn_state;
    GstWebRTCSignalingState signaling_state;
    GstWebRTCPeerConnectionState conn_state;

    g_object_get(obj, "ice-gathering-state", &ice_state, nullptr);
    g_object_get(obj, "ice-connection-state", &ice_conn_state, nullptr);
    g_object_get(obj, "signaling-state", &signaling_state, nullptr);
    g_object_get(obj, "connection-state", &conn_state, nullptr);

    g_print("[client] ICE gathering=%d ICE conn=%d signaling=%d conn=%d\n",
            ice_state, ice_conn_state, signaling_state, conn_state);
}

static void handle_media_stream(GstPad *pad, GstElement *pipe, const char *convert_name) {
    GstElement *q = gst_element_factory_make("queue", nullptr);
    GstElement *conv = gst_element_factory_make(convert_name, nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    if (!q || !conv || !sink) {
        g_printerr("[client] Failed to create incoming stream elements\n");
        return;
    }

    gst_bin_add_many(GST_BIN(pipe), q, conv, sink, nullptr);
    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(q, conv, sink, nullptr);

    GstPad *qpad = gst_element_get_static_pad(q, "sink");
    gst_pad_link(pad, qpad);
    gst_object_unref(qpad);
}

static void on_incoming_decodebin_stream(GstElement *decodebin, GstPad *pad, GstElement *pipe) {
    if (!gst_pad_has_current_caps(pad)) {
        g_printerr("[client] Incoming pad has no caps, ignoring\n");
        return;
    }
    GstCaps *caps = gst_pad_get_current_caps(pad);
    const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    if (g_str_has_prefix(name, "video")) {
        handle_media_stream(pad, pipe, "videoconvert");
    } else if (g_str_has_prefix(name, "audio")) {
        handle_media_stream(pad, pipe, "audioconvert");
    } else {
        g_printerr("[client] Unknown incoming pad caps: %s\n", name);
    }
    gst_caps_unref(caps);
}

static void on_incoming_stream(GstElement *webrtc, GstPad *pad, GstElement *pipe) {
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) {
        return;
    }
    GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
    if (!decodebin) {
        g_printerr("[client] Failed to create decodebin for incoming stream\n");
        return;
    }
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_incoming_decodebin_stream), pipe);
    gst_bin_add(GST_BIN(pipe), decodebin);
    gst_element_sync_state_with_parent(decodebin);

    GstPad *sinkpad = gst_element_get_static_pad(decodebin, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
    (void)webrtc;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    AppState *app = static_cast<AppState *>(data);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("[client] Error: %s\n", err ? err->message : "unknown");
            if (debug && *debug) {
                g_printerr("[client] Debug: %s\n", debug);
            }
            g_error_free(err);
            g_free(debug);
            g_main_loop_quit(app->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            log_msg("EOS");
            g_main_loop_quit(app->loop);
            break;
        default:
            break;
    }
    return TRUE;
}

static bool ensure_element_available(const gchar *name) {
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory) {
        g_printerr("[client] Missing GStreamer element: %s\n", name);
        return false;
    }
    gst_object_unref(factory);
    return true;
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    AppState app;
    app.server_url = g_strdup(argc > 1 ? argv[1] : "http://localhost:7860");
    app.webrtc_id = g_uuid_string_random();
    app.soup = soup_session_new();
    app.pending_ice = g_queue_new();
    app.mids = g_ptr_array_new_with_free_func(g_free);

    if (!ensure_element_available("v4l2src") ||
        !ensure_element_available("videoconvert") ||
        !ensure_element_available("videoscale") ||
        !ensure_element_available("videorate") ||
        !ensure_element_available("vp8enc") ||
        !ensure_element_available("rtpvp8pay") ||
        !ensure_element_available("webrtcbin") ||
        !ensure_element_available("nicesrc") ||
        !ensure_element_available("nicesink") ||
        !ensure_element_available("dtlsenc") ||
        !ensure_element_available("dtlsdec") ||
        !ensure_element_available("srtpenc") ||
        !ensure_element_available("srtpdec")) {
        return 1;
    }

    gchar *pipeline_desc = g_strdup(
        "v4l2src device=/dev/video0 "
        "! videoconvert "
        "! videoscale "
        "! videorate "
        "! video/x-raw,width=640,height=480,framerate=30/1 "
        "! queue "
        "! vp8enc deadline=1 keyframe-max-dist=30 "
        "! rtpvp8pay pt=96 "
        "! application/x-rtp,media=video,encoding-name=VP8,payload=96 "
        "! webrtcbin name=webrtc"
    );

    GError *error = nullptr;
    app.pipeline = gst_parse_launch(pipeline_desc, &error);
    g_free(pipeline_desc);

    if (!app.pipeline) {
        g_printerr("[client] Failed to create pipeline: %s\n", error ? error->message : "unknown");
        if (error) {
            g_error_free(error);
        }
        return 1;
    }

    app.webrtcbin = gst_bin_get_by_name(GST_BIN(app.pipeline), "webrtc");
    if (!app.webrtcbin) {
        g_printerr("[client] Failed to get webrtcbin\n");
        gst_object_unref(app.pipeline);
        return 1;
    }

    g_object_set(app.webrtcbin,
                 "stun-server", STUN_SERVER,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "sdp-semantic", "unified-plan",
                 "rtcp-mux-policy", "require",
                 nullptr);

    g_signal_connect(app.webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), &app);
    g_signal_connect(app.webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), &app);
    g_signal_connect(app.webrtcbin, "notify::ice-gathering-state", G_CALLBACK(on_webrtc_state_changed), &app);
    g_signal_connect(app.webrtcbin, "notify::ice-connection-state", G_CALLBACK(on_webrtc_state_changed), &app);
    g_signal_connect(app.webrtcbin, "notify::signaling-state", G_CALLBACK(on_webrtc_state_changed), &app);
    g_signal_connect(app.webrtcbin, "notify::connection-state", G_CALLBACK(on_webrtc_state_changed), &app);
    g_signal_connect(app.webrtcbin, "pad-added", G_CALLBACK(on_incoming_stream), app.pipeline);

    gst_element_set_state(app.pipeline, GST_STATE_READY);

    GstWebRTCDataChannel *data_channel = nullptr;
    g_signal_emit_by_name(app.webrtcbin, "create-data-channel", "client-data", nullptr, &data_channel);
    if (data_channel) {
        g_object_unref(data_channel);
    } else {
        g_printerr("[client] Could not create data channel\n");
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(app.pipeline));
    gst_bus_add_watch(bus, bus_call, &app);
    gst_object_unref(bus);

    log_msg("WebRTC client starting...");
    log_msg(app.server_url);

    gst_element_set_state(app.pipeline, GST_STATE_PLAYING);

    app.loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(app.loop);

    gst_element_set_state(app.pipeline, GST_STATE_NULL);
    gst_object_unref(app.webrtcbin);
    gst_object_unref(app.pipeline);
    g_object_unref(app.soup);
    g_free(app.server_url);
    g_free(app.webrtc_id);
    if (app.mids) {
        g_ptr_array_unref(app.mids);
    }
    if (app.pending_ice) {
        while (!g_queue_is_empty(app.pending_ice)) {
            PendingIce *pending = static_cast<PendingIce *>(g_queue_pop_head(app.pending_ice));
            g_free(pending->candidate);
            g_free(pending);
        }
        g_queue_free(app.pending_ice);
    }
    g_main_loop_unref(app.loop);

    return 0;
}

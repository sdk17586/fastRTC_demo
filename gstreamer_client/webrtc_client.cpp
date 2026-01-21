#include "webrtc_client.h"

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <glib.h>

#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>

#define STUN_SERVER "stun://stun.l.google.com:19302"

// PendingIce 구조체
struct PendingIce {
    guint mlineindex;
    gchar *candidate;
};

// 내부 구현 클래스 (PIMPL 패턴)
class WebRTCClient::Impl {
public:
    // GStreamer 관련
    GstElement *pipeline = nullptr;
    GstElement *webrtcbin = nullptr;
    SoupSession *soup = nullptr;
    GMainLoop *loop = nullptr;

    // 설정
    std::string server_url;
    gchar *webrtc_id = nullptr;
    CameraConfig camera_config;

    // 상태 관리
    bool remote_desc_set = false;
    GQueue *pending_ice = nullptr;
    GPtrArray *mids = nullptr;

    // 콜백
    StateCallback state_callback;
    ErrorCallback error_callback;
    ConnectionState current_state = ConnectionState::DISCONNECTED;

    mutable std::mutex state_mutex;  // const 메서드에서도 락을 걸 수 있도록 mutable

    Impl(const std::string& url, const CameraConfig& config)
        : server_url(url), camera_config(config) {
        soup = soup_session_new();
        pending_ice = g_queue_new();
        mids = g_ptr_array_new_with_free_func(g_free);
        webrtc_id = g_uuid_string_random();
    }

    ~Impl() {
        cleanup();
    }

    void cleanup() {
        if (loop) {
            g_main_loop_quit(loop);
            g_main_loop_unref(loop);
            loop = nullptr;
        }

        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            if (webrtcbin) {
                gst_object_unref(webrtcbin);
                webrtcbin = nullptr;
            }
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }

        if (soup) {
            g_object_unref(soup);
            soup = nullptr;
        }

        if (webrtc_id) {
            g_free(webrtc_id);
            webrtc_id = nullptr;
        }

        if (mids) {
            g_ptr_array_unref(mids);
            mids = nullptr;
        }

        if (pending_ice) {
            while (!g_queue_is_empty(pending_ice)) {
                PendingIce *p = static_cast<PendingIce *>(g_queue_pop_head(pending_ice));
                g_free(p->candidate);
                g_free(p);
            }
            g_queue_free(pending_ice);
            pending_ice = nullptr;
        }
    }

    void setState(ConnectionState state) {
        std::lock_guard<std::mutex> lock(state_mutex);
        current_state = state;
        if (state_callback) {
            state_callback(state);
        }
    }

    ConnectionState getState() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return current_state;
    }

    void reportError(const std::string& error) {
        if (error_callback) {
            error_callback(error);
        }
        setState(ConnectionState::FAILED);
    }

    // 정적 콜백 함수들 - Impl 포인터를 전달받아 인스턴스 메서드 호출
    static void on_negotiation_needed(GstElement *webrtcbin, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleNegotiationNeeded(webrtcbin);
    }

    static void on_offer_created_static(GstPromise *promise, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleOfferCreated(promise);
    }

    static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex, 
                                  gchar *candidate, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleIceCandidate(webrtcbin, mlineindex, candidate);
    }

    static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        return self->handleBusMessage(bus, msg);
    }

    static void on_incoming_stream(GstElement *webrtc, GstPad *pad, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleIncomingStream(webrtc, pad);
    }

    static void on_incoming_decodebin_stream(GstElement * /*decodebin*/, GstPad *pad, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleDecodebinStream(pad);
    }

    static void on_webrtc_state_changed(GObject *obj, GParamSpec * /*pspec*/, gpointer user_data) {
        Impl *self = static_cast<Impl *>(user_data);
        self->handleWebRTCStateChanged(obj);
    }

    // 인스턴스 메서드들
    void handleNegotiationNeeded(GstElement *webrtcbin);
    void handleOfferCreated(GstPromise *promise);
    void handleIceCandidate(GstElement *webrtcbin, guint mlineindex, gchar *candidate);
    gboolean handleBusMessage(GstBus *bus, GstMessage *msg);
    void handleIncomingStream(GstElement *webrtc, GstPad *pad);
    void handleDecodebinStream(GstPad *pad);
    void handleMediaStream(GstPad *pad, const char *convert_name);
    void handleWebRTCStateChanged(GObject *obj);

    // 헬퍼 메서드들
    bool ensureElementAvailable(const gchar *name);
    void queueIce(guint mlineindex, const gchar *candidate);
    void flushPendingIce();
    void sendIceCandidate(guint mlineindex, const gchar *candidate);
    JsonNode *postJson(const gchar *path, JsonNode *payload);
    void forceSetupActive(GstSDPMessage *sdp);
};

// WebRTCClient 구현

WebRTCClient::WebRTCClient(const std::string& server_url)
    : pImpl(std::make_unique<Impl>(server_url, CameraConfig{})) {
}

WebRTCClient::WebRTCClient(const std::string& server_url, const CameraConfig& camera_config)
    : pImpl(std::make_unique<Impl>(server_url, camera_config)) {
}

WebRTCClient::~WebRTCClient() {
    stop();
}

bool WebRTCClient::start() {
    if (!pImpl->ensureElementAvailable("v4l2src") ||
        !pImpl->ensureElementAvailable("videoconvert") ||
        !pImpl->ensureElementAvailable("videoscale") ||
        !pImpl->ensureElementAvailable("videorate") ||
        !pImpl->ensureElementAvailable("vp8enc") ||
        !pImpl->ensureElementAvailable("rtpvp8pay") ||
        !pImpl->ensureElementAvailable("webrtcbin") ||
        !pImpl->ensureElementAvailable("nicesrc") ||
        !pImpl->ensureElementAvailable("nicesink") ||
        !pImpl->ensureElementAvailable("dtlsenc") ||
        !pImpl->ensureElementAvailable("dtlsdec") ||
        !pImpl->ensureElementAvailable("srtpenc") ||
        !pImpl->ensureElementAvailable("srtpdec")) {
        pImpl->reportError("Required GStreamer elements not available");
        return false;
    }

    // 파이프라인 생성
    gchar *codec_name = g_strdup(pImpl->camera_config.codec == "vp8" ? "vp8enc" : "x264enc");
    gchar *payloader = g_strdup(pImpl->camera_config.codec == "vp8" ? "rtpvp8pay" : "rtph264pay");
    gchar *media_encoding = g_strdup(pImpl->camera_config.codec == "vp8" ? "VP8" : "H264");
    
    gchar *pipeline_desc = g_strdup_printf(
        "v4l2src device=%s "
        "! videoconvert "
        "! videoscale "
        "! videorate "
        "! video/x-raw,width=%d,height=%d,framerate=%d/1 "
        "! queue "
        "! %s deadline=1 keyframe-max-dist=30 "
        "! %s pt=96 "
        "! application/x-rtp,media=video,encoding-name=%s,payload=96 "
        "! webrtcbin name=webrtc",
        pImpl->camera_config.device.c_str(),
        pImpl->camera_config.width,
        pImpl->camera_config.height,
        pImpl->camera_config.framerate,
        codec_name,
        payloader,
        media_encoding
    );

    g_free(codec_name);
    g_free(payloader);
    g_free(media_encoding);

    GError *error = nullptr;
    pImpl->pipeline = gst_parse_launch(pipeline_desc, &error);
    g_free(pipeline_desc);

    if (!pImpl->pipeline) {
        std::string error_msg = "Failed to create pipeline: ";
        error_msg += error ? error->message : "unknown";
        if (error) {
            g_error_free(error);
        }
        pImpl->reportError(error_msg);
        return false;
    }

    pImpl->webrtcbin = gst_bin_get_by_name(GST_BIN(pImpl->pipeline), "webrtc");
    if (!pImpl->webrtcbin) {
        pImpl->reportError("Failed to get webrtcbin");
        return false;
    }

    g_object_set(pImpl->webrtcbin,
                 "stun-server", STUN_SERVER,
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 nullptr);

    // 시그널 연결
    g_signal_connect(pImpl->webrtcbin, "on-negotiation-needed", 
                     G_CALLBACK(Impl::on_negotiation_needed), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "on-ice-candidate", 
                     G_CALLBACK(Impl::on_ice_candidate), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "notify::ice-gathering-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "notify::ice-connection-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "notify::signaling-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "notify::connection-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    g_signal_connect(pImpl->webrtcbin, "pad-added", 
                     G_CALLBACK(Impl::on_incoming_stream), pImpl.get());

    gst_element_set_state(pImpl->pipeline, GST_STATE_READY);

    // Data channel 생성
    GstWebRTCDataChannel *data_channel = nullptr;
    g_signal_emit_by_name(pImpl->webrtcbin, "create-data-channel", "client-data", nullptr, &data_channel);
    if (data_channel) {
        g_object_unref(data_channel);
    }

    // Bus watch 설정
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pImpl->pipeline));
    gst_bus_add_watch(bus, Impl::bus_call, pImpl.get());
    gst_object_unref(bus);

    pImpl->setState(ConnectionState::CONNECTING);

    gst_element_set_state(pImpl->pipeline, GST_STATE_PLAYING);

    pImpl->loop = g_main_loop_new(nullptr, FALSE);

    return true;
}

void WebRTCClient::stop() {
    if (pImpl) {
        pImpl->cleanup();
        pImpl->setState(ConnectionState::DISCONNECTED);
    }
}

void WebRTCClient::run() {
    if (pImpl->loop) {
        g_main_loop_run(pImpl->loop);
    }
}

void WebRTCClient::runAsync() {
    std::thread([this]() {
        run();
    }).detach();
}

void WebRTCClient::setStateCallback(StateCallback callback) {
    pImpl->state_callback = callback;
}

void WebRTCClient::setErrorCallback(ErrorCallback callback) {
    pImpl->error_callback = callback;
}

WebRTCClient::ConnectionState WebRTCClient::getState() const {
    return pImpl->getState();
}

void WebRTCClient::setServerUrl(const std::string& url) {
    pImpl->server_url = url;
}

void WebRTCClient::setCameraConfig(const CameraConfig& config) {
    pImpl->camera_config = config;
}

// Impl 메서드 구현들
bool WebRTCClient::Impl::ensureElementAvailable(const gchar *name) {
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory) {
        g_printerr("[client] Missing GStreamer element: %s\n", name);
        return false;
    }
    gst_object_unref(factory);
    return true;
}

void WebRTCClient::Impl::queueIce(guint mlineindex, const gchar *candidate) {
    if (!pending_ice) {
        pending_ice = g_queue_new();
    }
    PendingIce *pending = g_new0(PendingIce, 1);
    pending->mlineindex = mlineindex;
    pending->candidate = g_strdup(candidate);
    g_queue_push_tail(pending_ice, pending);
}

void WebRTCClient::Impl::flushPendingIce() {
    while (pending_ice && !g_queue_is_empty(pending_ice)) {
        PendingIce *p = static_cast<PendingIce *>(g_queue_pop_head(pending_ice));
        sendIceCandidate(p->mlineindex, p->candidate);
        g_free(p->candidate);
        g_free(p);
    }
}

void WebRTCClient::Impl::sendIceCandidate(guint mlineindex, const gchar *candidate) {
    if (!remote_desc_set) {
        queueIce(mlineindex, candidate);
        return;
    }

    const gchar *sdp_mid = nullptr;
    if (mids && mlineindex < mids->len) {
        sdp_mid = static_cast<const gchar *>(g_ptr_array_index(mids, mlineindex));
    }
    if (!sdp_mid || !*sdp_mid) {
        sdp_mid = "0";
    }

    if (std::strstr(candidate, " TCP ") != nullptr || 
        std::strstr(candidate, " tcptype ") != nullptr) {
        return;  // TCP candidate 무시
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
    json_builder_add_string_value(builder, webrtc_id);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = postJson("/webrtc/ice", root);
    if (response) {
        json_node_free(response);
    }
    json_node_free(root);
    g_object_unref(builder);
}

JsonNode *WebRTCClient::Impl::postJson(const gchar *path, JsonNode *payload) {
    gchar *url = g_strdup_printf("%s%s", server_url.c_str(), path);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);

    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, payload);
    gchar *body = json_generator_to_data(gen, nullptr);
    g_object_unref(gen);

    GBytes *request_body = g_bytes_new_take(body, std::strlen(body));
    soup_message_set_request_body_from_bytes(msg, "application/json", request_body);
    g_bytes_unref(request_body);

    GError *error = nullptr;
    GBytes *response = soup_session_send_and_read(soup, msg, nullptr, &error);
    if (!response) {
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

void WebRTCClient::Impl::forceSetupActive(GstSDPMessage *sdp) {
    guint media_len = gst_sdp_message_medias_len(sdp);
    for (guint i = 0; i < media_len; ++i) {
        GstSDPMedia *media = (GstSDPMedia *)gst_sdp_message_get_media(sdp, i);
        if (!media) continue;

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

void WebRTCClient::Impl::handleNegotiationNeeded(GstElement *webrtcbin) {
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created_static, this, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
}

void WebRTCClient::Impl::handleOfferCreated(GstPromise *promise) {
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    if (!offer) {
        reportError("Failed to create offer");
        return;
    }

    // MIDs 수집
    if (mids) {
        g_ptr_array_set_size(mids, 0);
        guint media_len = gst_sdp_message_medias_len(offer->sdp);
        for (guint i = 0; i < media_len; ++i) {
            const GstSDPMedia *media = gst_sdp_message_get_media(offer->sdp, i);
            const gchar *mid = media ? gst_sdp_media_get_attribute_val(media, "mid") : nullptr;
            g_ptr_array_add(mids, mid ? g_strdup(mid) : g_strdup(""));
        }
    }

    forceSetupActive(offer->sdp);

    // 로컬 description 설정
    GstPromise *set_promise = gst_promise_new();
    g_signal_emit_by_name(webrtcbin, "set-local-description", offer, set_promise);
    gst_promise_interrupt(set_promise);
    gst_promise_unref(set_promise);

    // 서버로 offer 전송
    gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, sdp_str);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "offer");
    json_builder_set_member_name(builder, "webrtc_id");
    json_builder_add_string_value(builder, webrtc_id);
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = postJson("/webrtc/offer", root);

    if (response) {
        JsonObject *obj = json_node_get_object(response);
        const gchar *answer_sdp = json_object_get_string_member(obj, "sdp");
        const gchar *answer_type = json_object_get_string_member(obj, "type");

        if (answer_sdp && answer_type && g_strcmp0(answer_type, "answer") == 0) {
            GstSDPMessage *sdp = nullptr;
            gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(answer_sdp), 
                                         std::strlen(answer_sdp), sdp);
            GstWebRTCSessionDescription *answer =
                gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);

            GstPromise *remote_promise = gst_promise_new();
            g_signal_emit_by_name(webrtcbin, "set-remote-description", answer, remote_promise);
            gst_promise_interrupt(remote_promise);
            gst_promise_unref(remote_promise);
            gst_webrtc_session_description_free(answer);

            remote_desc_set = true;
            flushPendingIce();
            setState(ConnectionState::CONNECTED);
        } else {
            reportError("Invalid answer from server");
        }
        json_node_free(response);
    } else {
        reportError("Failed to get answer from server");
    }

    json_node_free(root);
    g_object_unref(builder);
    g_free(sdp_str);
    gst_webrtc_session_description_free(offer);
}

void WebRTCClient::Impl::handleIceCandidate(GstElement *webrtcbin, guint mlineindex, 
                                             gchar *candidate) {
    (void)webrtcbin;
    if (candidate && *candidate) {
        sendIceCandidate(mlineindex, candidate);
    }
}

gboolean WebRTCClient::Impl::handleBusMessage(GstBus *bus, GstMessage *msg) {
    (void)bus;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            std::string error_msg = "GStreamer error: ";
            error_msg += err ? err->message : "unknown";
            if (debug && *debug) {
                error_msg += " (";
                error_msg += debug;
                error_msg += ")";
            }
            reportError(error_msg);
            g_error_free(err);
            g_free(debug);
            if (loop) {
                g_main_loop_quit(loop);
            }
            break;
        }
        case GST_MESSAGE_EOS:
            if (loop) {
                g_main_loop_quit(loop);
            }
            break;
        default:
            break;
    }
    return TRUE;
}

void WebRTCClient::Impl::handleIncomingStream(GstElement *webrtc, GstPad *pad) {
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) {
        return;
    }

    GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
    if (!decodebin) {
        return;
    }

    // decodebin이 디코딩된 스트림 패드를 생성할 때 처리할 콜백 연결
    g_signal_connect(decodebin, "pad-added", 
                     G_CALLBACK(Impl::on_incoming_decodebin_stream), this);

    gst_bin_add(GST_BIN(pipeline), decodebin);
    gst_element_sync_state_with_parent(decodebin);

    GstPad *sinkpad = gst_element_get_static_pad(decodebin, "sink");
    gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
    (void)webrtc;
}

void WebRTCClient::Impl::handleDecodebinStream(GstPad *pad) {
    if (!gst_pad_has_current_caps(pad)) {
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);
    const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    
    if (g_str_has_prefix(name, "video")) {
        handleMediaStream(pad, "videoconvert");
    } else if (g_str_has_prefix(name, "audio")) {
        handleMediaStream(pad, "audioconvert");
    }
    
    gst_caps_unref(caps);
}

void WebRTCClient::Impl::handleMediaStream(GstPad *pad, const char *convert_name) {
    GstElement *q = gst_element_factory_make("queue", nullptr);
    GstElement *conv = gst_element_factory_make(convert_name, nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    
    if (!q || !conv || !sink) {
        if (q) gst_object_unref(q);
        if (conv) gst_object_unref(conv);
        if (sink) gst_object_unref(sink);
        return;
    }

    gst_bin_add_many(GST_BIN(pipeline), q, conv, sink, nullptr);
    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(conv);
    gst_element_sync_state_with_parent(sink);
    gst_element_link_many(q, conv, sink, nullptr);

    GstPad *qpad = gst_element_get_static_pad(q, "sink");
    gst_pad_link(pad, qpad);
    gst_object_unref(qpad);
}

void WebRTCClient::Impl::handleWebRTCStateChanged(GObject *obj) {
    GstWebRTCICEConnectionState ice_conn_state;
    g_object_get(obj, "ice-connection-state", &ice_conn_state, nullptr);

    if (ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ||
        ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) {
        setState(ConnectionState::CONNECTED);
    } else if (ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED) {
        setState(ConnectionState::FAILED);
        reportError("ICE connection failed");
    }
}


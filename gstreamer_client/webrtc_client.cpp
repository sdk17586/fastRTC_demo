#include "webrtc_client.h"

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "signaling_transport.h"
#include <glib.h>

#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>

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
    GMainLoop *loop = nullptr;
    guint bus_watch_id = 0;

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
    std::atomic<bool> loop_running{false};
    std::atomic<bool> shutting_down{false};
    std::thread loop_thread;
    std::mutex cleanup_mutex;

    std::unique_ptr<ISignalingTransport> transport;

    Impl(const std::string& url, const CameraConfig& config)
        : server_url(url), camera_config(config) {
        transport = std::make_unique<HttpSignalingTransport>(url);
        pending_ice = g_queue_new();
        mids = g_ptr_array_new_with_free_func(g_free);
        webrtc_id = g_uuid_string_random();
    }

    ~Impl() {
        requestStop();
        if (loop_thread.joinable() && std::this_thread::get_id() != loop_thread.get_id()) {
            loop_thread.join();
        }
        cleanupPipeline();
        cleanupSession();
    }

    void requestStop() {
        shutting_down = true;
        if (loop) {
            g_main_loop_quit(loop);
        }
    }

    void cleanupPipeline() {
        std::lock_guard<std::mutex> lock(cleanup_mutex);
        if (bus_watch_id != 0) {
            g_source_remove(bus_watch_id);
            bus_watch_id = 0;
        }

        if (loop) {
            g_main_loop_unref(loop);
            loop = nullptr;
        }

        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            if (webrtcbin) {
                g_signal_handlers_disconnect_by_data(webrtcbin, this);
                gst_object_unref(webrtcbin);
                webrtcbin = nullptr;
            }
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    void cleanupSession() {
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

    void resetSessionState() {
        shutting_down = false;
        remote_desc_set = false;
        if (webrtc_id) {
            g_free(webrtc_id);
        }
        webrtc_id = g_uuid_string_random();
        if (mids) {
            g_ptr_array_set_size(mids, 0);
        }
        if (pending_ice) {
            while (!g_queue_is_empty(pending_ice)) {
                PendingIce *p = static_cast<PendingIce *>(g_queue_pop_head(pending_ice));
                g_free(p->candidate);
                g_free(p);
            }
        }
    }

    void setState(ConnectionState state) {
        StateCallback callback;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (current_state == state) {
                return;
            }
            current_state = state;
            callback = state_callback;
        }
        if (callback) {
            callback(state);
        }
    }

    ConnectionState getState() const {
        std::lock_guard<std::mutex> lock(state_mutex);
        return current_state;
    }

    void reportError(const std::string& error) {
        if (shutting_down) {
            return;
        }
        emitError(error);
        setState(ConnectionState::FAILED);
    }

    void emitError(const std::string& error) {
        if (shutting_down) {
            return;
        }
        ErrorCallback callback;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            callback = error_callback;
        }
        if (callback) {
            callback(error);
        }
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
    void runLoop();

    // 헬퍼 메서드들
    bool ensureElementAvailable(const gchar *name);
    void queueIce(guint mlineindex, const gchar *candidate);
    void flushPendingIce();
    void sendIceCandidate(guint mlineindex, const gchar *candidate);
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
    if (pImpl->pipeline || pImpl->webrtcbin) {
        pImpl->reportError("Client already started");
        return false;
    }
    if (!pImpl->transport || !pImpl->pending_ice || !pImpl->mids) {
        pImpl->cleanupSession();
        pImpl->transport = std::make_unique<HttpSignalingTransport>(pImpl->server_url);
        pImpl->pending_ice = g_queue_new();
        pImpl->mids = g_ptr_array_new_with_free_func(g_free);
    }
    pImpl->resetSessionState();

    if (pImpl->camera_config.codec != "vp8" && pImpl->camera_config.codec != "h264") {
        pImpl->reportError("Unsupported codec (use \"vp8\" or \"h264\")");
        return false;
    }

    if (!pImpl->ensureElementAvailable("v4l2src") ||
        !pImpl->ensureElementAvailable("videoconvert") ||
        !pImpl->ensureElementAvailable("videoscale") ||
        !pImpl->ensureElementAvailable("videorate") ||
        !pImpl->ensureElementAvailable(pImpl->camera_config.codec == "vp8" ? "vp8enc" : "x264enc") ||
        !pImpl->ensureElementAvailable(pImpl->camera_config.codec == "vp8" ? "rtpvp8pay" : "rtph264pay") ||
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

    // offer 생성 및 수신 및 전송
    g_signal_connect(pImpl->webrtcbin, "on-negotiation-needed", 
                     G_CALLBACK(Impl::on_negotiation_needed), pImpl.get());
    // ICE candidate 수신 및 전송
    g_signal_connect(pImpl->webrtcbin, "on-ice-candidate", 
                     G_CALLBACK(Impl::on_ice_candidate), pImpl.get());
    // WebRTC 상태 변경 감지
    g_signal_connect(pImpl->webrtcbin, "notify::ice-gathering-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    // ICE 연결 상태 변경 감지
    g_signal_connect(pImpl->webrtcbin, "notify::ice-connection-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    // 시그널링 상태 변경 감지
    g_signal_connect(pImpl->webrtcbin, "notify::signaling-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    // 연결 상태 변경 감지
    g_signal_connect(pImpl->webrtcbin, "notify::connection-state", 
                     G_CALLBACK(Impl::on_webrtc_state_changed), pImpl.get());
    // 새로운 스트림 수신 감지. 서버에서 수신하는 미디어 데이터가 없어도 해당 코드라인이 없으면 링크에러남
    g_signal_connect(pImpl->webrtcbin, "pad-added", 
                     G_CALLBACK(Impl::on_incoming_stream), pImpl.get());

    gst_element_set_state(pImpl->pipeline, GST_STATE_READY);

    // Bus watch 설정
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pImpl->pipeline));
    pImpl->bus_watch_id = gst_bus_add_watch(bus, Impl::bus_call, pImpl.get());
    gst_object_unref(bus);

    pImpl->setState(ConnectionState::CONNECTING);

    gst_element_set_state(pImpl->pipeline, GST_STATE_PLAYING);

    pImpl->loop = g_main_loop_new(nullptr, FALSE);

    return true;
}

void WebRTCClient::stop() {
    if (pImpl) {
        pImpl->requestStop();
        if (pImpl->loop_thread.joinable() &&
            std::this_thread::get_id() != pImpl->loop_thread.get_id()) {
            pImpl->loop_thread.join();
        }
        if (!pImpl->loop_running.load()) {
            pImpl->cleanupPipeline();
        }
        pImpl->setState(ConnectionState::DISCONNECTED);
    }
}

void WebRTCClient::run() {
    if (pImpl->loop) {
        pImpl->runLoop();
    }
}

void WebRTCClient::runAsync() {
    if (!pImpl->loop) {
        return;
    }
    if (pImpl->loop_thread.joinable()) {
        return;
    }
    pImpl->loop_thread = std::thread([this]() {
        run();
    });
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
    if (pImpl->pipeline || pImpl->webrtcbin) {
        pImpl->emitError("Cannot change server URL while running");
        return;
    }
    pImpl->server_url = url;
    if (pImpl->transport) {
        pImpl->transport->setServerUrl(url);
    } else {
        pImpl->transport = std::make_unique<HttpSignalingTransport>(url);
    }
}

void WebRTCClient::setCameraConfig(const CameraConfig& config) {
    if (pImpl->pipeline || pImpl->webrtcbin) {
        pImpl->emitError("Cannot change camera config while running");
        return;
    }
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
    if (shutting_down) {
        return;
    }
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

    if (!transport) {
        emitError("Signaling transport not initialized");
        return;
    }

    std::string error_msg;
    if (!transport->sendIce(candidate, sdp_mid, static_cast<int>(mlineindex), webrtc_id, &error_msg)) {
        if (!error_msg.empty()) {
            emitError(error_msg);
        }
    }
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
    if (shutting_down) {
        return;
    }
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created_static, this, nullptr);
    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
}

void WebRTCClient::Impl::handleOfferCreated(GstPromise *promise) {
    if (shutting_down) {
        gst_promise_unref(promise);
        return;
    }
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
    if (!transport) {
        reportError("Signaling transport not initialized");
        g_free(sdp_str);
        gst_webrtc_session_description_free(offer);
        return;
    }

    std::string answer_sdp;
    std::string error_msg;

    //json으로 변환하여 서버로 offer로 전송과 동시에 asnwer을 받아오는 역할의 API
    if (!transport->sendOffer(sdp_str, webrtc_id, &answer_sdp, &error_msg)) {
        reportError(error_msg.empty() ? "Failed to get answer from server" : error_msg);
        g_free(sdp_str);
        gst_webrtc_session_description_free(offer);
        return;
    }

    GstSDPMessage *sdp = nullptr;
    gst_sdp_message_new(&sdp);
    gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(answer_sdp.c_str()),
                                 answer_sdp.size(), sdp);
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

    g_free(sdp_str);
    gst_webrtc_session_description_free(offer);
}

void WebRTCClient::Impl::handleIceCandidate(GstElement *webrtcbin, guint mlineindex, 
                                             gchar *candidate) {
    (void)webrtcbin;
    if (shutting_down) {
        return;
    }
    if (candidate && *candidate) {
        sendIceCandidate(mlineindex, candidate);
    }
}

gboolean WebRTCClient::Impl::handleBusMessage(GstBus *bus, GstMessage *msg) {
    (void)bus;
    if (shutting_down) {
        return TRUE;
    }
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
    if (shutting_down) {
        return;
    }
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
    if (shutting_down) {
        return;
    }
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
    if (shutting_down) {
        return;
    }
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
    if (shutting_down) {
        return;
    }
    GstWebRTCICEConnectionState ice_conn_state;
    g_object_get(obj, "ice-connection-state", &ice_conn_state, nullptr);

    if (ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED ||
        ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED) {
        setState(ConnectionState::CONNECTED);
    } else if (ice_conn_state == GST_WEBRTC_ICE_CONNECTION_STATE_FAILED) {
        reportError("ICE connection failed");
    }
}

void WebRTCClient::Impl::runLoop() {
    loop_running = true;
    g_main_loop_run(loop);
    loop_running = false;
    cleanupPipeline();
}

#include <gst/gst.h>
#include <gst/sdp/sdp.h>

// GST_USE_UNSTABLE_API는 Makefile에서 이미 정의됨
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <cstring>
#include <iostream>

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

static void parse_ice_candidate_details(const gchar *candidate_str) {
    if (!candidate_str) {
        return;
    }
    
    // ICE candidate 형식: candidate:<foundation> <component-id> <transport> <priority> <ip> <port> typ <type> [options...]
    gchar *candidate = g_strdup(candidate_str);
    gchar *colon = strstr(candidate, ":");
    if (!colon) {
        g_free(candidate);
        return;
    }
    
    gchar *rest = colon + 1;
    gchar **parts = g_strsplit_set(rest, " \t", 0);
    guint part_count = 0;
    while (parts[part_count]) part_count++;
    
    if (part_count >= 7) {
        g_print("    Foundation: %s\n", parts[0]);
        g_print("    Component ID: %s\n", parts[1]);
        g_print("    Transport: %s\n", parts[2]);
        g_print("    Priority: %s\n", parts[3]);
        g_print("    IP Address: %s\n", parts[4]);
        g_print("    Port: %s\n", parts[5]);
        
        // typ 필드 찾기
        for (guint i = 6; i < part_count; ++i) {
            if (g_strcmp0(parts[i], "typ") == 0 && i + 1 < part_count) {
                g_print("    Type: %s\n", parts[i + 1]);
                break;
            }
        }
        
        // 추가 옵션 파싱
        for (guint i = 6; i < part_count; ++i) {
            if (g_strcmp0(parts[i], "raddr") == 0 && i + 1 < part_count) {
                g_print("    Remote Address: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "rport") == 0 && i + 1 < part_count) {
                g_print("    Remote Port: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "generation") == 0 && i + 1 < part_count) {
                g_print("    Generation: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "ufrag") == 0 && i + 1 < part_count) {
                g_print("    ICE Ufrag: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "network-id") == 0 && i + 1 < part_count) {
                g_print("    Network ID: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "network-cost") == 0 && i + 1 < part_count) {
                g_print("    Network Cost: %s\n", parts[i + 1]);
            } else if (g_strcmp0(parts[i], "tcptype") == 0 && i + 1 < part_count) {
                g_print("    TCP Type: %s\n", parts[i + 1]);
            }
        }
    } else {
        g_print("    Raw candidate: %s\n", rest);
    }
    
    g_strfreev(parts);
    g_free(candidate);
}

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

    // ICE 송신 로그 출력
    std::cout << "\n========================================" << std::endl;
    std::cout << "📤 ICE CANDIDATE 송신 (클라이언트 -> 서버)" << std::endl;
    std::cout << "========================================\n" << std::endl;
    g_print("[client] Sending ICE candidate to server\n");
    g_print("[client] Media Line Index: %u\n", mlineindex);
    g_print("[client] SDP MID: %s\n", sdp_mid);
    g_print("[client] Candidate String: %s\n", candidate);
    g_print("[client] ICE Candidate Details:\n");
    parse_ice_candidate_details(candidate);

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
        std::cout << "✅ ICE CANDIDATE 전송 완료\n" << std::endl;
    } else {
        std::cout << "❌ ICE CANDIDATE 전송 실패\n" << std::endl;
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

static void print_sdp_details(const gchar *title, const gchar *sdp_text) {
    g_print("\n--- %s SDP Details ---\n", title);
    
    if (!sdp_text) {
        g_print("SDP text is null\n");
        g_print("--- End of %s SDP Details ---\n\n", title);
        return;
    }
    
    // Parse SDP text line by line
    const gchar *line_start = sdp_text;
    guint media_index = 0;
    gboolean in_media = FALSE;
    
    while (*line_start) {
        const gchar *line_end = line_start;
        while (*line_end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        
        if (line_end > line_start && *line_start == '=' && line_start + 1 < line_end) {
            gchar type = *(line_start + 1);
            gchar *line = g_strndup(line_start + 2, line_end - line_start - 2);
            
            switch (type) {
                case 'v':
                    g_print("Version: %s\n", line);
                    break;
                case 'o':
                    {
                        gchar **parts = g_strsplit(line, " ", 6);
                        if (g_strv_length(parts) >= 6) {
                            g_print("Origin: username=%s, sess-id=%s, sess-version=%s, nettype=%s, addrtype=%s, address=%s\n",
                                    parts[0], parts[1], parts[2], parts[3], parts[4], parts[5]);
                        } else {
                            g_print("Origin: %s\n", line);
                        }
                        g_strfreev(parts);
                    }
                    break;
                case 's':
                    g_print("Session Name: %s\n", line);
                    break;
                case 'i':
                    g_print("Session Info: %s\n", line);
                    break;
                case 'u':
                    g_print("URI: %s\n", line);
                    break;
                case 'e':
                    g_print("Email: %s\n", line);
                    break;
                case 'p':
                    g_print("Phone: %s\n", line);
                    break;
                case 'c':
                    {
                        gchar **parts = g_strsplit(line, " ", 3);
                        if (g_strv_length(parts) >= 3) {
                            g_print("Connection: nettype=%s, addrtype=%s, address=%s\n",
                                    parts[0], parts[1], parts[2]);
                        } else {
                            g_print("Connection: %s\n", line);
                        }
                        g_strfreev(parts);
                    }
                    break;
                case 't':
                    {
                        gchar **parts = g_strsplit(line, " ", 2);
                        if (g_strv_length(parts) >= 2) {
                            g_print("Timing: start=%s, stop=%s\n", parts[0], parts[1]);
                        }
                        g_strfreev(parts);
                    }
                    break;
                case 'm':
                    {
                        in_media = TRUE;
                        gchar **parts = g_strsplit(line, " ", 4);
                        if (g_strv_length(parts) >= 3) {
                            g_print("\n  Media #%u:\n", media_index++);
                            g_print("    Type: %s\n", parts[0]);
                            g_print("    Port: %s\n", parts[1]);
                            g_print("    Protocol: %s\n", parts[2]);
                            if (g_strv_length(parts) >= 4 && parts[3]) {
                                g_print("    Formats: %s\n", parts[3]);
                            }
                        }
                        g_strfreev(parts);
                    }
                    break;
                case 'a':
                    {
                        gchar *attr = line;
                        gchar *colon = strchr(attr, ':');
                        if (colon) {
                            *colon = '\0';
                            gchar *value = colon + 1;
                            if (g_str_has_prefix(attr, "fingerprint") || 
                                g_str_has_prefix(attr, "setup") ||
                                g_str_has_prefix(attr, "ice-ufrag") ||
                                g_str_has_prefix(attr, "ice-pwd") ||
                                g_str_has_prefix(attr, "ice-options") ||
                                g_str_has_prefix(attr, "rtcp-mux")) {
                                if (in_media) {
                                    g_print("    Attribute: %s=%s\n", attr, value);
                                } else {
                                    g_print("  Attribute: %s=%s\n", attr, value);
                                }
                            } else if (g_str_has_prefix(attr, "rtpmap")) {
                                if (in_media) {
                                    g_print("    RTP Map: %s\n", value);
                                } else {
                                    g_print("  RTP Map: %s\n", value);
                                }
                            } else if (g_str_has_prefix(attr, "fmtp")) {
                                if (in_media) {
                                    g_print("    Format Parameters: %s\n", value);
                                } else {
                                    g_print("  Format Parameters: %s\n", value);
                                }
                            } else if (g_str_has_prefix(attr, "ssrc")) {
                                if (in_media) {
                                    g_print("    SSRC: %s\n", value);
                                } else {
                                    g_print("  SSRC: %s\n", value);
                                }
                            } else {
                                if (in_media) {
                                    g_print("    Attribute: %s:%s\n", attr, value);
                                } else {
                                    g_print("  Attribute: %s:%s\n", attr, value);
                                }
                            }
                            *colon = ':';  // restore
                        } else {
                            if (g_strcmp0(attr, "sendrecv") == 0 ||
                                g_strcmp0(attr, "sendonly") == 0 ||
                                g_strcmp0(attr, "recvonly") == 0 ||
                                g_strcmp0(attr, "inactive") == 0) {
                                if (in_media) {
                                    g_print("    Direction: %s\n", attr);
                                } else {
                                    g_print("  Direction: %s\n", attr);
                                }
                            } else if (g_str_has_prefix(attr, "mid:")) {
                                if (in_media) {
                                    g_print("    Media ID: %s\n", attr + 4);
                                } else {
                                    g_print("  Media ID: %s\n", attr + 4);
                                }
                            } else {
                                if (in_media) {
                                    g_print("    Attribute: %s\n", attr);
                                } else {
                                    g_print("  Attribute: %s\n", attr);
                                }
                            }
                        }
                    }
                    break;
            }
            
            g_free(line);
        }
        
        // Move to next line
        line_start = line_end;
        if (*line_start == '\r') line_start++;
        if (*line_start == '\n') line_start++;
        if (*line_start == '\0') break;
    }
    
    g_print("--- End of %s SDP Details ---\n\n", title);
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
    
    // Offer 송신 로그 출력
    std::cout << "\n========================================" << std::endl;
    std::cout << "📤 OFFER 송신 (클라이언트 -> 서버)" << std::endl;
    std::cout << "========================================\n" << std::endl;
    g_print("[client] Sending offer to server\n");
    g_print("[client] Offer SDP length: %zu bytes\n", std::strlen(sdp_str));
    
    // SDP 상세 정보 출력
    print_sdp_details("OFFER", sdp_str);
    
    // Full SDP text 출력
    g_print("[client] Full OFFER SDP:\n");
    g_print("%s\n", sdp_str);

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
        
        // Answer 수신 로그 출력
        std::cout << "\n========================================" << std::endl;
        std::cout << "📥 ANSWER 수신 (서버 -> 클라이언트)" << std::endl;
        std::cout << "========================================\n" << std::endl;
        
        if (answer_sdp && answer_type && g_strcmp0(answer_type, "answer") == 0) {
            g_print("[client] Received valid answer from server\n");
            g_print("[client] Answer SDP length: %zu bytes\n", std::strlen(answer_sdp));
            g_print("[client] Answer type: %s\n", answer_type);
            
            // SDP 상세 정보 출력
            print_sdp_details("ANSWER", answer_sdp);
            
            // Full SDP text 출력
            g_print("[client] Full ANSWER SDP:\n");
            g_print("%s\n", answer_sdp);
            
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
            
            std::cout << "✅ ANSWER 처리 완료\n" << std::endl;
        } else {
            g_printerr("[client] Invalid answer from server\n");
            if (answer_type) {
                g_printerr("[client] Expected type 'answer', got '%s'\n", answer_type);
            }
            if (!answer_sdp) {
                g_printerr("[client] Answer SDP is null\n");
            }
            std::cout << "❌ ANSWER 처리 실패\n" << std::endl;
        }
        json_node_free(response);
    } else {
        g_printerr("[client] Failed to get answer from server\n");
        std::cout << "\n========================================" << std::endl;
        std::cout << "❌ ANSWER 수신 실패 (서버 응답 없음)" << std::endl;
        std::cout << "========================================\n" << std::endl;
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
        g_print("[client] ICE candidate gathered (mline=%u, mid=%s)\n", mlineindex, mid);
        send_ice_candidate(app, mlineindex, candidate);
    }
}

static const gchar *ice_gathering_state_to_string(GstWebRTCICEGatheringState state) {
    switch (state) {
        case GST_WEBRTC_ICE_GATHERING_STATE_NEW:
            return "NEW";
        case GST_WEBRTC_ICE_GATHERING_STATE_GATHERING:
            return "GATHERING";
        case GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE:
            return "COMPLETE";
        default:
            return "UNKNOWN";
    }
}

static const gchar *ice_connection_state_to_string(GstWebRTCICEConnectionState state) {
    switch (state) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_NEW:
            return "NEW";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
            return "CHECKING";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED:
            return "CONNECTED";
        case GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED:
            return "COMPLETED";
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
            return "FAILED";
        case GST_WEBRTC_ICE_CONNECTION_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case GST_WEBRTC_ICE_CONNECTION_STATE_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN";
    }
}

static const gchar *signaling_state_to_string(GstWebRTCSignalingState state) {
    switch (state) {
        case GST_WEBRTC_SIGNALING_STATE_STABLE:
            return "STABLE";
        case GST_WEBRTC_SIGNALING_STATE_CLOSED:
            return "CLOSED";
        case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_OFFER:
            return "HAVE_LOCAL_OFFER";
        case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_OFFER:
            return "HAVE_REMOTE_OFFER";
        case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER:
            return "HAVE_LOCAL_PRANSWER";
        case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER:
            return "HAVE_REMOTE_PRANSWER";
        default:
            return "UNKNOWN";
    }
}

static const gchar *connection_state_to_string(GstWebRTCPeerConnectionState state) {
    switch (state) {
        case GST_WEBRTC_PEER_CONNECTION_STATE_NEW:
            return "NEW";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTING:
            return "CONNECTING";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED:
            return "CONNECTED";
        case GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED:
            return "DISCONNECTED";
        case GST_WEBRTC_PEER_CONNECTION_STATE_FAILED:
            return "FAILED";
        case GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN";
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

    g_print("[client] WebRTC State: ICE_Gathering=%s(%d) ICE_Connection=%s(%d) Signaling=%s(%d) Connection=%s(%d)\n",
            ice_gathering_state_to_string(ice_state), ice_state,
            ice_connection_state_to_string(ice_conn_state), ice_conn_state,
            signaling_state_to_string(signaling_state), signaling_state,
            connection_state_to_string(conn_state), conn_state);
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

static void on_incoming_decodebin_stream(GstElement * /* decodebin */, GstPad *pad, GstElement *pipe) {
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

// WebRTC 연결 후 원격 스트림(서버로부터의 비디오/오디오)이 도착할 때 호출됨
// webrtcbin이 소스 패드를 생성하면 이 함수가 호출되어 패드를 decodebin에 연결함
// 이 연결이 없으면 "not-linked" 에러가 발생하여 파이프라인이 실패함
static void on_incoming_stream(GstElement *webrtc, GstPad *pad, GstElement *pipe) {
    // 소스 패드(src)만 처리 (webrtcbin에서 나오는 패드)
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) {
        return;
    }
    // 원격 스트림을 디코딩하기 위한 decodebin 생성
    GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
    if (!decodebin) {
        g_printerr("[client] Failed to create decodebin for incoming stream\n");
        return;
    }
    // decodebin이 디코딩된 스트림 패드를 생성할 때 처리할 콜백 연결
    g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_incoming_decodebin_stream), pipe);
    // decodebin을 파이프라인에 추가
    gst_bin_add(GST_BIN(pipe), decodebin);
    gst_element_sync_state_with_parent(decodebin);

    // webrtcbin의 소스 패드를 decodebin의 싱크 패드에 연결
    // 이 연결이 핵심: 연결되지 않으면 데이터가 흐를 곳이 없어 에러 발생
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
                 // "sdp-semantic" 속성은 GstWebRTCBin에 존재하지 않음 (제거)
                 "rtcp-mux-policy", "require",
                 nullptr);
    
    // WebRTC 협상이 필요할 때 호출되는 콜백 연결 (SDP offer/answer 교환 시작)
    g_signal_connect(app.webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), &app);
    // ICE 후보(candidate)가 수집될 때 호출되는 콜백 연결 (네트워크 연결 정보 전송)
    g_signal_connect(app.webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), &app);
    // ICE 수집 상태 변경 시 호출되는 콜백 연결 (gathering, complete 등 상태 모니터링)
    g_signal_connect(app.webrtcbin, "notify::ice-gathering-state", G_CALLBACK(on_webrtc_state_changed), &app);
    // ICE 연결 상태 변경 시 호출되는 콜백 연결 (new, checking, connected, failed 등 상태 모니터링)
    g_signal_connect(app.webrtcbin, "notify::ice-connection-state", G_CALLBACK(on_webrtc_state_changed), &app);
    // 시그널링 상태 변경 시 호출되는 콜백 연결 (stable, have-local-offer, have-remote-offer 등 상태 모니터링)
    g_signal_connect(app.webrtcbin, "notify::signaling-state", G_CALLBACK(on_webrtc_state_changed), &app);
    // 피어 연결 상태 변경 시 호출되는 콜백 연결 (new, connecting, connected, disconnected 등 상태 모니터링)
    g_signal_connect(app.webrtcbin, "notify::connection-state", G_CALLBACK(on_webrtc_state_changed), &app);
    // 원격 스트림이 추가될 때 호출되는 콜백 연결 (상대방으로부터 미디어 스트림 수신 시 처리)
    // [필수] 이 콜백이 없으면 webrtcbin의 소스 패드가 연결되지 않아 "not-linked" 에러 발생
    // 
    // 주의: 클라이언트가 서버로만 송신하더라도 이 콜백은 필수입니다.
    // 이유:
    // 1. WebRTC는 양방향 연결을 설정하므로, 서버의 SDP answer에 수신 경로가 포함될 수 있음
    // 2. RTCP 패킷(통계/제어)은 양방향으로 교환되며, 이로 인해 소스 패드가 생성될 수 있음
    // 3. 서버가 미디어 스트림을 보내지 않더라도, WebRTC 프로토콜 특성상 일부 패드가 생성될 수 있음
    // 4. 패드가 생성되었는데 연결되지 않으면 "not-linked" 에러로 파이프라인이 실패함
    // 
    // 따라서 서버가 스트림을 보내지 않는 경우에도 이 콜백을 등록해야 합니다.
    // (콜백이 호출되지 않을 수 있지만, 호출될 경우를 대비해 반드시 등록 필요)
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

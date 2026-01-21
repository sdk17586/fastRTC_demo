#include "signaling_transport.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include <cstring>

namespace {
JsonNode *post_json(SoupSession *soup, const std::string& server_url, const char *path, JsonNode *payload) {
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
}  // namespace

HttpSignalingTransport::HttpSignalingTransport(const std::string& url)
    : server_url(url) {
    soup = soup_session_new();
}

HttpSignalingTransport::~HttpSignalingTransport() {
    if (soup) {
        g_object_unref(soup);
        soup = nullptr;
    }
}

void HttpSignalingTransport::setServerUrl(const std::string& url) {
    server_url = url;
}

bool HttpSignalingTransport::sendOffer(const std::string& offer_sdp,
                                       const std::string& webrtc_id,
                                       std::string* answer_sdp,
                                       std::string* error) {
    if (!answer_sdp) {
        if (error) {
            *error = "Answer output is null";
        }
        return false;
    }

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, offer_sdp.c_str());
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "offer");
    json_builder_set_member_name(builder, "webrtc_id");
    json_builder_add_string_value(builder, webrtc_id.c_str());
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = post_json(soup, server_url, "/webrtc/offer", root);

    bool ok = false;
    if (response) {
        JsonObject *obj = json_node_get_object(response);
        const gchar *sdp_value = json_object_get_string_member(obj, "sdp");
        const gchar *answer_type = json_object_get_string_member(obj, "type");
        if (sdp_value && answer_type && g_strcmp0(answer_type, "answer") == 0) {
            *answer_sdp = sdp_value;
            ok = true;
        } else if (error) {
            *error = "Invalid answer from server";
        }
        json_node_free(response);
    } else if (error) {
        *error = "Failed to get answer from server";
    }

    json_node_free(root);
    g_object_unref(builder);
    return ok;
}

bool HttpSignalingTransport::sendIce(const std::string& candidate,
                                     const std::string& sdp_mid,
                                     int mlineindex,
                                     const std::string& webrtc_id,
                                     std::string* error) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_add_string_value(builder, candidate.c_str());
    json_builder_set_member_name(builder, "sdpMid");
    json_builder_add_string_value(builder, sdp_mid.c_str());
    json_builder_set_member_name(builder, "sdpMLineIndex");
    json_builder_add_int_value(builder, mlineindex);
    json_builder_end_object(builder);
    json_builder_set_member_name(builder, "webrtc_id");
    json_builder_add_string_value(builder, webrtc_id.c_str());
    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonNode *response = post_json(soup, server_url, "/webrtc/ice", root);

    bool ok = response != nullptr;
    if (response) {
        json_node_free(response);
    } else if (error) {
        *error = "Failed to send ICE candidate";
    }

    json_node_free(root);
    g_object_unref(builder);
    return ok;
}

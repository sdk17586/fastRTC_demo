#pragma once

#include <string>

typedef struct _SoupSession SoupSession;

class ISignalingTransport {
public:
    virtual ~ISignalingTransport() = default;
    virtual void setServerUrl(const std::string& url) = 0;
    virtual bool sendOffer(const std::string& offer_sdp,
                           const std::string& webrtc_id,
                           std::string* answer_sdp,
                           std::string* error) = 0;
    virtual bool sendIce(const std::string& candidate,
                         const std::string& sdp_mid,
                         int mlineindex,
                         const std::string& webrtc_id,
                         std::string* error) = 0;
};

class HttpSignalingTransport : public ISignalingTransport {
public:
    explicit HttpSignalingTransport(const std::string& url);
    ~HttpSignalingTransport() override;

    void setServerUrl(const std::string& url) override;
    bool sendOffer(const std::string& offer_sdp,
                   const std::string& webrtc_id,
                   std::string* answer_sdp,
                   std::string* error) override;
    bool sendIce(const std::string& candidate,
                 const std::string& sdp_mid,
                 int mlineindex,
                 const std::string& webrtc_id,
                 std::string* error) override;

private:
    std::string server_url;
    SoupSession *soup = nullptr;
};

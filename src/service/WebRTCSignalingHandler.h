//
// Created by ican on 10/10/26.
//

#ifndef WEBRTC_APP_WEBRTCSIGNALINGHANDLER_H
#define WEBRTC_APP_WEBRTCSIGNALINGHANDLER_H

#include "App.h"
#include <string>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <mutex>
#include <unordered_map>

#include "GStreamerMediaService.h"
#include "PerSocketData.h"
#include "ISignalingSink.h"

using namespace std;



class WebRTCSignalingHandler : public ISignalingSink {
public:
    using WebSocketType = uWS::WebSocket<false, true, PerSocketData>;

    void initRoutes(uWS::App &app);

    // ISignalingSink: called from GStreamer threads, sends are deferred to the uWS loop thread
    void sendOffer(const string &sessionId, const string &sdp) override;
    void sendIceCandidate(const string &sessionId, int sdpMLineIndex, const string &candidate) override;

private:
    uWS::Loop *loop = nullptr;
    GStreamerMediaService mediaService;

    //Thread-safe session registry
    mutex sessionMutex;
    unordered_map<string, WebSocketType*> sessions;

    void registerSession(const string &sessionId, WebSocketType *ws);
    void unregisterSession(const string &sessionId);
    void sendMessage(WebSocketType *ws, const nlohmann::json &j);

    bool isValidRoomId(const string &roomId);
};

#endif //WEBRTC_APP_WEBRTCSIGNALINGHANDLER_H

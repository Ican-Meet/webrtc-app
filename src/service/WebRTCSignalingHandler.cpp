//
// Created by ican on 10/10/26.
//
#include "WebRTCSignalingHandler.h"
#include <regex>

#include "GStreamerMediaService.h"

bool WebRTCSignalingHandler::isValidRoomId(const string &roomId) {
    static const regex roomPattern("^[A-Za-z0-9._-]{1,64}$");
    return regex_match(roomId, roomPattern);
}

void WebRTCSignalingHandler::initRoutes(uWS::App &app) {
    loop = uWS::Loop::get();

    app.ws<PerSocketData>("/meeting/:room_id", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 120,
        .maxBackpressure = 10 * 1024,

        .upgrade = [](auto *res, auto *req, auto *context) {
            string roomId(req->getParameter(0));

            static const regex roomPattern("^[A-Za-z0-9._-]{1,64}$");
            if (!regex_match(roomId, roomPattern)) {
                res->writeStatus("400 Bad Request")->end("Invalid room ID");
                return;
            }

            res->template upgrade<PerSocketData>({
                .sessionId = "",
                .roomId = roomId,
            },
            req->getHeader("sec-websocket-key"),
            req->getHeader("sec-websocket-protocol"),
            req->getHeader("sec-websocket-extensions"),
            context
            );
        },

        .open = [this](WebSocketType *ws) {
            PerSocketData *data = ws->getUserData();
            string roomId = data->roomId;

            //Generate a unique session ID based on memory pointer address
            string sessionId = to_string(reinterpret_cast<intptr_t>(ws));
            data->sessionId = sessionId;

            registerSession(sessionId, ws);
            ws->subscribe("room_" + roomId);

            spdlog::info("Client connected. Session: {}, Room Id: {}", sessionId, roomId);

            // 1. Send "init" messsage equivalent
            nlohmann::json initMsg = {
                {"type", "init"},
                {"senderId", sessionId},
                {"roomId", roomId},
            };
            sendMessage(ws, initMsg);

            // 2. Create the GStreamer WebRTC endpoint; offer/candidates come back via ISignalingSink
            mediaService.generatePeerConnection(roomId, sessionId, this);

        },

        .message = [this](WebSocketType *ws, string_view message, uWS::OpCode opCode) {
            if (message.empty()) return;

            PerSocketData *data = ws->getUserData();
            string sessionId = data->sessionId;

            try {
                auto jsonNode = nlohmann::json::parse(message);
                if (!jsonNode.contains("type") || jsonNode["type"].is_null()) {
                    spdlog::warn("Received message without 'type' field from session {}", sessionId);
                    return;
                }

                string type = jsonNode["type"];
                if (type == "client_ready") {
                    spdlog::info("Client ready signal received from session {}", sessionId);
                }
                else if (type == "close_media") {
                    spdlog::info("Close media signal received from session {}", sessionId);
                    mediaService.stopPeerConnection(sessionId);
                }
                else if (type == "answer") {
                    if (jsonNode.contains("sdp")) {
                        string sdp = jsonNode["sdp"];
                        spdlog::info("Received message from session {}", sessionId);
                        mediaService.handleRemoteAnswer(sessionId, sdp);
                    } else {
                        spdlog::warn("Missing 'sdp' in answer message from session {}", sessionId);
                    }
                }
                else if (type == "candidate") {
                    if (jsonNode.contains("candidate")) {
                        // Accept {"candidate": {"candidate", "sdpMLineIndex"}} or a flat message
                        const auto &c = jsonNode["candidate"].is_object() ? jsonNode["candidate"] : jsonNode;
                        string candidate = c.value("candidate", "");
                        int sdpMLineIndex = c.contains("sdpMLineIndex") ? c.value("sdpMLineIndex", 0)
                                                                         : c.value("sdpMlineIndex", 0);
                        spdlog::info("Received ICE candidate from session {}", sessionId);
                        mediaService.handleRemoteCandidate(sessionId, sdpMLineIndex, candidate);
                    }
                }  else {
                    spdlog::warn("Unhandled message type '{}' from session {}", type, sessionId);
                }
            } catch (const std::exception &e) {
                spdlog::error("Failed to parse incoming JSON message from session {}: {}", sessionId, e.what());
            }
        },

        .close = [this](WebSocketType *ws, int code, string_view message) {
            PerSocketData *data = ws->getUserData();
            string sessionId = data->sessionId;

            unregisterSession(sessionId);
            mediaService.stopPeerConnection(sessionId);
            spdlog::warn("WebSocket connection closed for session: {} (Code: {}). Resources cleaned up.", sessionId, code);

        }
    });
}

void WebRTCSignalingHandler::registerSession(const string &sessionId, WebSocketType *ws) {
    lock_guard<mutex> lock(sessionMutex);
    sessions[sessionId] = ws;
}

void WebRTCSignalingHandler::unregisterSession(const string &sessionId) {
    lock_guard<mutex> lock(sessionMutex);
    sessions.erase(sessionId);
}

void WebRTCSignalingHandler::sendMessage(WebSocketType *ws, const nlohmann::json &j) {
    const string payload = j.dump();
    ws->send(payload, uWS::OpCode::TEXT);
}

void WebRTCSignalingHandler::sendOffer(const string &sessionId, const string &sdp) {
    const nlohmann::json msg = {
        {"type", "offer"},
        {"sdp", sdp},
    };
    // uWS sockets may only be used on their loop thread
    loop->defer([this, sessionId, payload = msg.dump()]() {
        lock_guard<mutex> lock(sessionMutex);
        if (const auto it = sessions.find(sessionId); it != sessions.end() && it->second) {
            it->second->send(payload, uWS::OpCode::TEXT);
        }
    });
}

void WebRTCSignalingHandler::sendIceCandidate(const string &sessionId, int sdpMLineIndex, const string &candidate) {
    const nlohmann::json msg = {
        {"type", "ice_candidate"},
        {"candidate", {
            {"candidate", candidate},
            {"sdpMlineIndex", sdpMLineIndex}
        }}
    };
    loop->defer([this, sessionId, payload = msg.dump()]() {
        lock_guard<mutex> lock(sessionMutex);
        if (const auto it = sessions.find(sessionId); it != sessions.end() && it->second) {
            it->second->send(payload, uWS::OpCode::TEXT);
        }
    });
}

//
// Created by ican on 10/10/26.
//

#ifndef WEBRTC_APP_SIGNALINGSINK_H
#define WEBRTC_APP_SIGNALINGSINK_H

#include <string>

using namespace std;

// Implemented by the signaling layer. Called from GStreamer threads, so
// implementations must be thread-safe.
class ISignalingSink {
public:
    // Virtual destructor is crucial for proper cleanup of derived classes
    virtual ~ISignalingSink() = default;

    virtual void sendOffer(const string &sessionId, const string &sdp) = 0;
    virtual void sendIceCandidate(const string &sessionId, int sdpMLineIndex, const string &candidate) = 0;
};

#endif

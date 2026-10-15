//
// Created by ican on 10/10/26.
//

#ifndef WEBRTC_APP_GSTREAMERMEDIASERVICE_H
#define WEBRTC_APP_GSTREAMERMEDIASERVICE_H

#include <string>
#include <format>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include "PeerSession.h"
#include "ISignalingSink.h"

using namespace std;

enum OutboundKind { OUTBOUND_VIDEO, OUTBOUND_AUDIO };

// One GStreamer pipeline per room: every peer's inbound video is composited into
// a grid and every peer's audio is mixed; the result is sent back to all peers.
class GStreamerMediaService {
public:
    ~GStreamerMediaService();

    void generatePeerConnection(const string &roomId, const string &sessionId, ISignalingSink *signaling);
    void handleRemoteAnswer(const string &sessionId, const string &sdp);
    void handleRemoteCandidate(const string &sessionId, int sdpMLineIndex, const string &candidate);
    void stopPeerConnection(const string &sessionId);

private:
    unordered_map<std::string, shared_ptr<Room>> rooms;
    unordered_map<std::string, shared_ptr<PeerSession>> sessions;
    mutex roomsMutex; // guards rooms, sessions and Room::peers/usedTileSlots

    shared_ptr<Room> generateRoom(const string &roomId);
    shared_ptr<PeerSession> findPeer(const string &sessionId);

    static GstElement *attachOutboundBranch(PeerSession *peer, OutboundKind kind);
    static bool linkTeeToBranch(PeerSession *peer, GstElement *tee, GstElement *branch);

    // GStreamer bus callback function to handle errors and warnings asynchronously
    static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer data);

    // webrtcbin / decodebin signal callbacks (run on GStreamer threads)
    static void on_negotiation_needed(GstElement *webrtcbin, gpointer data);
    static void on_offer_created(GstPromise *promise, gpointer data);
    static void onOfferGenerated(PeerSession *peer, GstWebRTCSessionDescription *offer, ISignalingSink *signaling);
    static void on_ice_candidate(GstElement *webrtcbin, guint sdpMLineIndex, gchar *candidate, gpointer data);
    static void on_pad_added(GstElement *webrtcbin, GstPad *pad, gpointer data);
    static void on_decoded_pad_added(GstElement *decodebin, GstPad *pad, gpointer data);
};


constexpr int TILE_WIDTH = 640;
constexpr int TILE_HEIGHT = 360;
constexpr int GRID_COLUMNS = 2;
constexpr int GRID_ROWS = 2;

constexpr int VIDEO_PAYLOAD_TYPE = 96;
constexpr int AUDIO_PAYLOAD_TYPE = 111;

inline string get_room_pipeline() {
    return format(
    "videotestsrc is-live=true pattern=black ! video/x-raw,width={0},height={1},framerate=30/1 "
    "! compositor name=room-compositor background=black "
    "! video/x-raw,width={0},height={1},framerate=30/1 "
    "! videoconvert ! video/x-raw,format=I420 ! tee name=room-video-tee allow-not-linked=true "
    "audiotestsrc is-live=true wave=silence "
    "! audiomixer name=room-audio-mixer "
    "! audioconvert ! audioresample ! audio/x-raw,rate=48000,channels=2 "
    "! tee name=room-audio-tee allow-not-linked=true",
    TILE_WIDTH * GRID_COLUMNS,
    TILE_HEIGHT * GRID_ROWS
    );
}

// Per-peer encoder branches, linked from the room tees into webrtcbin
inline string get_outbound_branch(OutboundKind kind) {
    if (kind == OUTBOUND_VIDEO) {
        return format(
        "queue leaky=downstream max-size-buffers=2 "
        "! vp8enc deadline=1 cpu-used=8 keyframe-max-dist=60 target-bitrate=1500000 "
        "! rtpvp8pay pt={0} ! capsfilter caps=\"application/x-rtp,media=video,encoding-name=VP8,payload={0}\"",
        VIDEO_PAYLOAD_TYPE);
    }
    return format(
    "queue leaky=downstream max-size-buffers=10 "
    "! opusenc ! rtpopuspay pt={0} ! capsfilter caps=\"application/x-rtp,media=audio,encoding-name=OPUS,payload={0}\"",
    AUDIO_PAYLOAD_TYPE);
}

#endif //WEBRTC_APP_GSTREAMERMEDIASERVICE_H

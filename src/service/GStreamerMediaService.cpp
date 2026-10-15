//
// Created by ican on 10/10/26.
//

#include "GStreamerMediaService.h"

GStreamerMediaService::~GStreamerMediaService() {
    vector<string> sessionIds;
    {
        lock_guard<mutex> lock(roomsMutex);
        for (const auto &[id, peer] : sessions) sessionIds.push_back(id);
    }
    for (const auto &id : sessionIds) stopPeerConnection(id);
}

gboolean GStreamerMediaService::on_bus_message(GstBus *bus, GstMessage *message, gpointer data) {
    auto *roomIdPtr = static_cast<string *>(data);
    string roomId = roomIdPtr ? *roomIdPtr : "unknown";

    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = nullptr;
            gchar *debug_info = nullptr;
            gst_message_parse_error(message, &error, &debug_info);

            spdlog::error("GStreamer error in room {}, from {} : {} (code: {}) {}",
                roomId, GST_OBJECT_NAME(message->src), error->message, error->code,
                debug_info ? debug_info : "");
            g_clear_error(&error);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *error = nullptr;
            gchar *debug_info = nullptr;
            gst_message_parse_warning(message, &error, &debug_info);

            spdlog::warn("GStreamer warning in room {} from {} : {} (code: {})",
                roomId, GST_OBJECT_NAME(message->src), error->message, error->code);
            g_clear_error(&error);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_EOS: {
            spdlog::info("GStreamer EOS in room: {}", roomId);
            break;
        }
        default:
            break;
    }
    return TRUE;
}


shared_ptr<Room> GStreamerMediaService::generateRoom(const string &roomId) {
    GError *error = nullptr;

    // 1. Generate and parse the pipeline string
    string room_pipeline_str = get_room_pipeline();
    GstElement *parsedElement = gst_parse_launch(room_pipeline_str.c_str(), &error);

    if (error) {
        spdlog::error("Failed to parse pipeline for room {}: {}", roomId, error->message);
        g_clear_error(&error);
        if (parsedElement) gst_object_unref(parsedElement);
        return nullptr;
    }

    // gst_parse_launch returns a floating reference; take ownership of it
    gst_object_ref_sink(parsedElement);
    GstPipeline *pipeline = GST_PIPELINE(parsedElement);

    // 2. Fetch named internal elements from the parsed pipeline
    GstElement *compositor = gst_bin_get_by_name(GST_BIN(pipeline), "room-compositor");
    GstElement *videoTee = gst_bin_get_by_name(GST_BIN(pipeline), "room-video-tee");
    GstElement *audioMixer = gst_bin_get_by_name(GST_BIN(pipeline), "room-audio-mixer");
    GstElement *audioTee = gst_bin_get_by_name(GST_BIN(pipeline), "room-audio-tee");

    shared_ptr<Room> room;
    if (compositor && videoTee && audioMixer && audioTee) {
        room = make_shared<Room>(roomId, pipeline, compositor, videoTee, audioMixer, audioTee);
    } else {
        spdlog::error("Room pipeline for {} is missing a named element", roomId);
    }

    // 3. Unref local lookup handles; Room holds its own references
    if (compositor) gst_object_unref(compositor);
    if (videoTee) gst_object_unref(videoTee);
    if (audioMixer) gst_object_unref(audioMixer);
    if (audioTee) gst_object_unref(audioTee);
    gst_object_unref(pipeline);

    if (!room) return nullptr;

    // 4. Setup Bus message watch for errors and warnings (dispatched on the GLib main loop)
    GstBus *bus = gst_element_get_bus(GST_ELEMENT(room->pipeline));
    gst_bus_add_watch_full(bus, G_PRIORITY_DEFAULT, on_bus_message, new string(roomId),
                           [](gpointer p) { delete static_cast<string *>(p); });
    gst_object_unref(bus);

    // 5. Transition pipeline to PLAYING state
    if (gst_element_set_state(GST_ELEMENT(room->pipeline), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("Failed to set pipeline to PLAYING for room {}", roomId);
        return nullptr;
    }

    spdlog::info("Pipeline generated and transitioned to PLAYING for room {}", roomId);
    return room;
}


void GStreamerMediaService::generatePeerConnection(const string &roomId, const string &sessionId, ISignalingSink *signaling) {
    // 1. Create webrtcbin
    string binName = "webrtc-" + sessionId;
    GstElement *webrtcbin = gst_element_factory_make("webrtcbin", binName.c_str());
    if (!webrtcbin) {
        spdlog::error("Failed to create webrtcbin element");
        return;
    }

    g_object_set(G_OBJECT(webrtcbin),
                 "stun-server", "stun://stun.l.google.com:19302",
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 nullptr);

    // 2. Thread-safe room and session tracking (equivalent to synchronized(rooms))
    shared_ptr<PeerSession> peer;
    {
        lock_guard<mutex> lock(roomsMutex);

        if (sessions.contains(sessionId)) {
            spdlog::warn("Peer connection already exists for session {}", sessionId);
            gst_object_unref(gst_object_ref_sink(webrtcbin));
            return;
        }

        //computeIfAbsent equivalent
        shared_ptr<Room> room;
        if (auto it = rooms.find(roomId); it != rooms.end()) {
            room = it->second;
        } else {
            room = generateRoom(roomId);
            if (!room) {
                gst_object_unref(gst_object_ref_sink(webrtcbin));
                return;
            }
            rooms[roomId] = room;
        }

        int tileSlot = room->allocateTileSlot();
        if (tileSlot >= GRID_COLUMNS * GRID_ROWS) {
            spdlog::warn("Room {} has more peers than grid tiles; tile {} will be off-canvas", roomId, tileSlot);
        }

        peer = make_shared<PeerSession>(sessionId, room, webrtcbin, tileSlot, signaling);
        room->peers.insert(sessionId);
        sessions[sessionId] = peer;
    }

    auto room = peer->room;

    // 3. Connect GStreamer signals. Raw pointer is safe: stopPeerConnection disconnects
    //    these handlers (after stopping the element) before the session can be destroyed.
    g_signal_connect(webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), peer.get());
    g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), peer.get());
    g_signal_connect(webrtcbin, "pad-added", G_CALLBACK(on_pad_added), peer.get());

    // 4. Add webrtcbin to the room's pipeline (the bin takes the floating ref)
    gst_bin_add(GST_BIN(room->pipeline), webrtcbin);

    // 5. Create outbound encoder branches; each one requests a webrtcbin sink pad,
    //    which creates a sendrecv transceiver
    GstElement *videoBranch = attachOutboundBranch(peer.get(), OUTBOUND_VIDEO);
    GstElement *audioBranch = attachOutboundBranch(peer.get(), OUTBOUND_AUDIO);
    if (!videoBranch || !audioBranch) {
        spdlog::error("Failed to build outbound branches for session {}", sessionId);
        stopPeerConnection(sessionId);
        return;
    }

    // 6. Sync state with parent (triggers negotiation with both transceivers present)
    gst_element_sync_state_with_parent(webrtcbin);
    gst_element_sync_state_with_parent(videoBranch);
    gst_element_sync_state_with_parent(audioBranch);

    // 7. Only now start feeding the branches from the room tees, so no buffers hit
    //    a webrtcbin that isn't running yet
    if (!linkTeeToBranch(peer.get(), room->videoTee, videoBranch) ||
        !linkTeeToBranch(peer.get(), room->audioTee, audioBranch)) {
        spdlog::error("Failed to link room tees for session {}", sessionId);
        stopPeerConnection(sessionId);
        return;
    }

    spdlog::info("Generated WebRTC peer for session: {}, in room {} (tile slot {})",
        sessionId, roomId, peer->tileSlot);
}

GstElement *GStreamerMediaService::attachOutboundBranch(PeerSession *peer, OutboundKind kind) {
    GError *error = nullptr;
    string description = get_outbound_branch(kind);
    GstElement *branch = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    if (error) {
        spdlog::error("Failed to build outbound branch for session {}: {}", peer->id, error->message);
        g_clear_error(&error);
        if (branch) gst_object_unref(branch);
        return nullptr;
    }

    gst_bin_add(GST_BIN(peer->room->pipeline), branch);
    {
        lock_guard<mutex> lock(peer->elementsMutex);
        peer->elements.push_back(GST_ELEMENT(gst_object_ref(branch)));
    }

    GstPad *branchSrc = gst_element_get_static_pad(branch, "src");
    GstPad *webrtcSink = gst_element_request_pad_simple(peer->webRTCBin, "sink_%u");
    bool linked = branchSrc && webrtcSink && gst_pad_link(branchSrc, webrtcSink) == GST_PAD_LINK_OK;
    if (branchSrc) gst_object_unref(branchSrc);
    if (webrtcSink) gst_object_unref(webrtcSink); // released together with webrtcbin

    return linked ? branch : nullptr;
}

bool GStreamerMediaService::linkTeeToBranch(PeerSession *peer, GstElement *tee, GstElement *branch) {
    GstPad *teeSrc = gst_element_request_pad_simple(tee, "src_%u");
    GstPad *branchSink = gst_element_get_static_pad(branch, "sink");
    bool linked = teeSrc && branchSink && gst_pad_link(teeSrc, branchSink) == GST_PAD_LINK_OK;
    if (branchSink) gst_object_unref(branchSink);
    if (teeSrc) {
        lock_guard<mutex> lock(peer->elementsMutex);
        peer->requestPads.push_back(teeSrc); // keeps the request-pad reference
    }
    return linked;
}

void GStreamerMediaService::on_negotiation_needed(GstElement *webrtcbin, gpointer data) {
    auto *peer = static_cast<PeerSession *>(data);
    if (peer->closed) return;

    // Keep the session alive until the promise is resolved
    auto *holder = new shared_ptr<PeerSession>(peer->shared_from_this());
    GstPromise *promise = gst_promise_new_with_change_func(
        on_offer_created, holder,
        [](gpointer p) { delete static_cast<shared_ptr<PeerSession> *>(p); });
    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
}

void GStreamerMediaService::on_offer_created(GstPromise *promise, gpointer data) {
    shared_ptr<PeerSession> peer = *static_cast<shared_ptr<PeerSession> *>(data);
    GstWebRTCSessionDescription *offer = nullptr;

    if (gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED) {
        const GstStructure *reply = gst_promise_get_reply(promise);
        if (reply && gst_structure_has_field(reply, "offer")) {
            gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
        }
    }
    gst_promise_unref(promise);

    onOfferGenerated(peer.get(), offer, peer->signaling);
}

void GStreamerMediaService::onOfferGenerated(PeerSession *peer, GstWebRTCSessionDescription *offer, ISignalingSink *signaling) {
    if (offer == nullptr || peer->closed) {
        if (offer == nullptr) {
            spdlog::error("Failed to generate SDP offer for session: {}", peer->id);
        }
        // If offer was created but session is closed, we must free it to avoid leaks
        if (offer != nullptr) {
            gst_webrtc_session_description_free(offer);
        }
        return;
    }

    // 1. Set local description on webrtcbin. The signal copies the description,
    //    so we still own 'offer' and free it below.
    g_signal_emit_by_name(peer->webRTCBin, "set-local-description", offer, nullptr);

    // 2. Convert the internal GstSDPMessage into a text string
    // gst_sdp_message_as_text allocates a gchar* that must be freed with g_free()
    gchar *sdp_chars = gst_sdp_message_as_text(offer->sdp);
    gst_webrtc_session_description_free(offer);
    if (!sdp_chars) {
        spdlog::error("Failed to serialize SDP message to text for session: {}", peer->id);
        return;
    }

    // 3. Send the SDP offer via the signaling sink
    signaling->sendOffer(peer->id, sdp_chars);
    g_free(sdp_chars);

    peer->offerGenerated = true;
    spdlog::info("SDP offer sent for session: {}", peer->id);
}

void GStreamerMediaService::on_ice_candidate(GstElement *, guint sdpMLineIndex, gchar *candidate, gpointer data) {
    auto *peer = static_cast<PeerSession *>(data);
    if (peer->closed || !candidate) return;
    peer->signaling->sendIceCandidate(peer->id, static_cast<int>(sdpMLineIndex), candidate);
}

// Remote media arrived on webrtcbin: decode it, then route it into the room mixers
void GStreamerMediaService::on_pad_added(GstElement *, GstPad *pad, gpointer data) {
    auto *peer = static_cast<PeerSession *>(data);
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;

    string name = format("decodebin-{}-{}", peer->id, peer->inboundCounter++);
    GstElement *decodebin = gst_element_factory_make("decodebin", name.c_str());
    if (!decodebin) {
        spdlog::error("Failed to create decodebin for session {}", peer->id);
        return;
    }

    {
        lock_guard<mutex> lock(peer->elementsMutex);
        if (peer->closed) {
            gst_object_unref(gst_object_ref_sink(decodebin));
            return;
        }
        g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_decoded_pad_added), peer);
        gst_bin_add(GST_BIN(peer->room->pipeline), decodebin);
        peer->elements.push_back(GST_ELEMENT(gst_object_ref(decodebin)));
    }

    gst_element_sync_state_with_parent(decodebin);

    GstPad *sinkPad = gst_element_get_static_pad(decodebin, "sink");
    if (gst_pad_link(pad, sinkPad) != GST_PAD_LINK_OK) {
        spdlog::error("Failed to link inbound stream to decodebin for session {}", peer->id);
    }
    gst_object_unref(sinkPad);
}

void GStreamerMediaService::on_decoded_pad_added(GstElement *, GstPad *pad, gpointer data) {
    auto *peer = static_cast<PeerSession *>(data);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    string mediaType = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    gst_caps_unref(caps);

    bool isVideo = mediaType.starts_with("video/");
    bool isAudio = mediaType.starts_with("audio/");
    if (!isVideo && !isAudio) {
        spdlog::warn("Ignoring decoded stream of type {} for session {}", mediaType, peer->id);
        return;
    }

    string description = isVideo
        ? format("queue ! videoconvert ! videoscale ! capsfilter caps=\"video/x-raw,width={},height={},pixel-aspect-ratio=1/1\"",
                 TILE_WIDTH, TILE_HEIGHT)
        : string("queue ! audioconvert ! audioresample");

    GError *error = nullptr;
    GstElement *branch = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    if (error) {
        spdlog::error("Failed to build inbound branch for session {}: {}", peer->id, error->message);
        g_clear_error(&error);
        if (branch) gst_object_unref(branch);
        return;
    }

    lock_guard<mutex> lock(peer->elementsMutex);
    if (peer->closed) {
        gst_object_unref(gst_object_ref_sink(branch));
        return;
    }

    Room &room = *peer->room;
    GstElement *mixer = isVideo ? room.videoCompositor : room.audioMixer;
    GstPad *mixerSink = gst_element_request_pad_simple(mixer, "sink_%u");
    if (!mixerSink) {
        spdlog::error("Failed to request mixer pad for session {}", peer->id);
        gst_object_unref(gst_object_ref_sink(branch));
        return;
    }

    if (isVideo) {
        g_object_set(mixerSink,
                     "xpos", (peer->tileSlot % GRID_COLUMNS) * TILE_WIDTH,
                     "ypos", (peer->tileSlot / GRID_COLUMNS) * TILE_HEIGHT,
                     "width", TILE_WIDTH,
                     "height", TILE_HEIGHT,
                     nullptr);
    }

    gst_bin_add(GST_BIN(room.pipeline), branch);
    peer->elements.push_back(GST_ELEMENT(gst_object_ref(branch)));
    peer->requestPads.push_back(mixerSink);

    GstPad *branchSink = gst_element_get_static_pad(branch, "sink");
    GstPad *branchSrc = gst_element_get_static_pad(branch, "src");
    bool linked = gst_pad_link(branchSrc, mixerSink) == GST_PAD_LINK_OK;
    gst_element_sync_state_with_parent(branch);
    linked = linked && gst_pad_link(pad, branchSink) == GST_PAD_LINK_OK;
    gst_object_unref(branchSink);
    gst_object_unref(branchSrc);

    if (linked) {
        spdlog::info("Inbound {} from session {} routed into room {}",
                     isVideo ? "video" : "audio", peer->id, room.id);
    } else {
        spdlog::error("Failed to link inbound {} for session {}", isVideo ? "video" : "audio", peer->id);
    }
}

shared_ptr<PeerSession> GStreamerMediaService::findPeer(const string &sessionId) {
    lock_guard<mutex> lock(roomsMutex);
    auto it = sessions.find(sessionId);
    return it != sessions.end() ? it->second : nullptr;
}

void GStreamerMediaService::handleRemoteAnswer(const string &sessionId, const string &sdp) {
    auto peer = findPeer(sessionId);
    if (!peer) {
        spdlog::warn("Received answer for unknown session {}", sessionId);
        return;
    }

    GstSDPMessage *sdpMessage = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &sdpMessage) != GST_SDP_OK) {
        spdlog::error("Failed to parse SDP answer from session {}", sessionId);
        if (sdpMessage) gst_sdp_message_free(sdpMessage);
        return;
    }

    // The description takes ownership of sdpMessage
    GstWebRTCSessionDescription *answer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdpMessage);
    g_signal_emit_by_name(peer->webRTCBin, "set-remote-description", answer, nullptr);
    gst_webrtc_session_description_free(answer);

    spdlog::info("Remote answer applied for session {}", sessionId);
}

void GStreamerMediaService::handleRemoteCandidate(const string &sessionId, int sdpMLineIndex, const string &candidate) {
    if (candidate.empty()) return; // end-of-candidates marker

    auto peer = findPeer(sessionId);
    if (!peer) {
        spdlog::warn("Received ICE candidate for unknown session {}", sessionId);
        return;
    }
    g_signal_emit_by_name(peer->webRTCBin, "add-ice-candidate", static_cast<guint>(sdpMLineIndex), candidate.c_str());
}

void GStreamerMediaService::stopPeerConnection(const string &sessionId) {
    shared_ptr<PeerSession> peer;
    {
        lock_guard<mutex> lock(roomsMutex);
        auto it = sessions.find(sessionId);
        if (it == sessions.end()) return;
        peer = it->second;
        sessions.erase(it);
    }

    // 1. Mark closed under the elements lock so streaming threads stop adding elements
    vector<GstElement *> elements;
    vector<GstPad *> requestPads;
    {
        lock_guard<mutex> lock(peer->elementsMutex);
        peer->closed = true;
        elements = peer->elements;
        requestPads = peer->requestPads;
    }

    auto room = peer->room;
    GstBin *pipeline = GST_BIN(room->pipeline);

    auto releaseRequestPad = [](GstPad *pad) {
        if (GstElement *owner = gst_pad_get_parent_element(pad)) {
            gst_element_release_request_pad(owner, pad);
            gst_object_unref(owner);
        }
    };

    // 2. Stop outbound flow: detach this peer's branches from the room tees
    for (GstPad *pad : requestPads) {
        if (GST_PAD_DIRECTION(pad) == GST_PAD_SRC) releaseRequestPad(pad);
    }

    // 3. Stop webrtcbin and all per-peer elements (joins their streaming threads)
    gst_element_set_state(peer->webRTCBin, GST_STATE_NULL);
    for (GstElement *element : elements) gst_element_set_state(element, GST_STATE_NULL);

    // 4. No more callbacks may reference this session
    g_signal_handlers_disconnect_by_data(peer->webRTCBin, peer.get());
    for (GstElement *element : elements) g_signal_handlers_disconnect_by_data(element, peer.get());

    // 5. Free this peer's compositor/audiomixer inputs
    for (GstPad *pad : requestPads) {
        if (GST_PAD_DIRECTION(pad) == GST_PAD_SINK) releaseRequestPad(pad);
    }

    // 6. Remove everything from the room pipeline
    for (GstElement *element : elements) gst_bin_remove(pipeline, element);
    gst_bin_remove(pipeline, peer->webRTCBin);

    // 7. Room bookkeeping; drop the room (and its pipeline) when it becomes empty
    {
        lock_guard<mutex> lock(roomsMutex);
        room->peers.erase(sessionId);
        room->releaseTileSlot(peer->tileSlot);
        if (room->peers.empty()) {
            rooms.erase(room->id);
            spdlog::info("Room {} is empty; pipeline released", room->id);
        }
    }

    spdlog::info("Peer connection stopped for session {}", sessionId);
}

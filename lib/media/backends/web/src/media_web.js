// The page's side of the web media codec, which Emscripten links into the program (--js-library). It plays resources in
// video elements, and gives their frames as entries of the page's images, which a Canvas 2D device imports
// (gpu_canvas2d.js).

addToLibrary({
    // The players, by the number that prMediaWebOpen returns, or null for a free slot. A player holds the video
    // element, the object URL of its resource, and the entry of its last frame. `presented` counts the frames that the
    // video presented, and `time` is the media time of the last; `serial` is the count when the last entry was made.
    $prMediaWebPlayers: [null],

    prMediaWebAvailable: function() {
        return typeof document !== "undefined";
    },

    // Returns 2 for "probably", 1 for "maybe" and 0 for no, which are the values of PrMediaSupport.
    prMediaWebCanPlay__deps: ["$prWebString"],
    prMediaWebCanPlay: function(type, typeSize) {
        const answer = document.createElement("video").canPlayType(prWebString(type, typeSize));
        return answer === "probably" ? 2 : answer === "maybe" ? 1 : 0;
    },

    // Plays a copy of the bytes, muted and looping, from the start.
    prMediaWebOpen__deps: ["$prMediaWebPlayers", "$prWebString"],
    prMediaWebOpen: function(bytes, size, type, typeSize) {
        const blob = new Blob([HEAPU8.slice(bytes, bytes + size)], {type: prWebString(type, typeSize)});
        const video = document.createElement("video");
        video.muted = true;
        video.loop = true;
        video.playsInline = true;
        video.preload = "auto";
        const url = URL.createObjectURL(blob);
        video.src = url;
        video.play().catch(() => {});
        let slot = prMediaWebPlayers.indexOf(null, 1);
        if (slot < 0)
            slot = prMediaWebPlayers.push(null) - 1;
        const state = {video, url, frame: 0, serial: 0, presented: 0, time: 0, watched: false};
        prMediaWebPlayers[slot] = state;
        // The video reports each frame that it presents, if the browser has requestVideoFrameCallback. Without it, a
        // frame counts as presented when the video's time moves.
        if (video.requestVideoFrameCallback) {
            state.watched = true;
            const presented = (now, metadata) => {
                if (prMediaWebPlayers[slot] !== state)
                    return;
                ++state.presented;
                state.time = metadata.mediaTime;
                video.requestVideoFrameCallback(presented);
            };
            video.requestVideoFrameCallback(presented);
        }
        return slot;
    },

    prMediaWebClose__deps: ["$prMediaWebPlayers", "$prGpuCanvasRemove"],
    prMediaWebClose: function(player) {
        const state = prMediaWebPlayers[player];
        if (state.frame !== 0)
            prGpuCanvasRemove(state.frame);
        state.video.pause();
        state.video.removeAttribute("src");
        state.video.load();
        URL.revokeObjectURL(state.url);
        prMediaWebPlayers[player] = null;
    },

    // Returns the entry of the frame that the video shows, once it has one, and writes the frame's time and serial.
    // Returns zero if there is no frame. The video seeks if it plays far from `seconds`, and plays on its own between
    // calls.
    prMediaWebFrame__deps: ["$prMediaWebPlayers", "$prGpuCanvasAddImage", "$prGpuCanvasRemove"],
    prMediaWebFrame: function(player, seconds, time, serial) {
        const state = prMediaWebPlayers[player];
        const video = state.video;
        if (video.readyState < 2)
            return 0;
        if (!video.seeking && Math.abs(video.currentTime - seconds) > 0.5 &&
                !(isFinite(video.duration) && seconds >= video.duration))
            video.currentTime = seconds;
        if (video.paused)
            video.play().catch(() => {});
        if (!state.watched && video.currentTime !== state.time) {
            state.time = video.currentTime;
            ++state.presented;
        }
        // A new entry is made only for a frame that the video has not given yet. If the browser has VideoFrame, a
        // VideoFrame holds the picture that shows now. The element itself draws whatever shows when it is drawn.
        if (state.frame === 0 || state.serial !== state.presented) {
            if (state.frame !== 0)
                prGpuCanvasRemove(state.frame);
            state.frame = typeof VideoFrame !== "undefined" ? prGpuCanvasAddImage(new VideoFrame(video), true) :
                prGpuCanvasAddImage(video, false);
            state.serial = state.presented;
        }
        HEAPF64[time >>> 3] = state.time;
        HEAPU32[serial >>> 2] = state.serial;
        return state.frame;
    },
});

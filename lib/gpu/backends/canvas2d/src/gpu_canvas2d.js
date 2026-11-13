// The page's side of the Canvas 2D backend, which Emscripten links into the program (--js-library). It keeps the
// canvases of the device's textures, and copies texels into and out of them.

addToLibrary({
    // The canvases of the textures, by PrGpuCanvas. An entry holds `canvas` and its 2D `context`, and a free slot is
    // null. Zero names no canvas.
    $prGpuCanvases: [null],

    // Returns the PrGpuCanvas of a new entry for `canvas`.
    $prGpuCanvasAdd__deps: ["$prGpuCanvases"],
    $prGpuCanvasAdd: function(canvas, options) {
        const context = canvas.getContext("2d", options);
        if (context === null)
            return 0;
        let slot = prGpuCanvases.indexOf(null, 1);
        if (slot < 0)
            slot = prGpuCanvases.push(null) - 1;
        prGpuCanvases[slot] = {canvas, context};
        return slot;
    },

    // Returns the PrGpuCanvas of a new entry for an image that a context can draw, such as a VideoFrame, which has no
    // context of its own. If `owned` is true, the entry closes the image when it is released.
    $prGpuCanvasAddImage__deps: ["$prGpuCanvases"],
    $prGpuCanvasAddImage: function(image, owned) {
        let slot = prGpuCanvases.indexOf(null, 1);
        if (slot < 0)
            slot = prGpuCanvases.push(null) - 1;
        prGpuCanvases[slot] = {canvas: image, context: null, owned: owned === true};
        return slot;
    },

    // Returns a new entry that holds a reference of its own to the image of the entry `image`, and writes the image's
    // extent.
    prGpuCanvas2dImport__deps: ["$prGpuCanvases", "$prGpuCanvasAddImage"],
    prGpuCanvas2dImport: function(image, width, height) {
        const entry = prGpuCanvases[image];
        if (!entry)
            return 0;
        const source = entry.canvas;
        const isFrame = typeof VideoFrame !== "undefined" && source instanceof VideoFrame;
        const isVideo = typeof HTMLVideoElement !== "undefined" && source instanceof HTMLVideoElement;
        const w = isFrame ? source.displayWidth : isVideo ? source.videoWidth : source.width;
        const h = isFrame ? source.displayHeight : isVideo ? source.videoHeight : source.height;
        if (!(w > 0 && h > 0))
            return 0;
        HEAPU32[width >>> 2] = w;
        HEAPU32[height >>> 2] = h;
        return prGpuCanvasAddImage(isFrame ? source.clone() : source, isFrame);
    },

    // Frees an entry, and closes its image if the entry owns it.
    $prGpuCanvasRemove__deps: ["$prGpuCanvases"],
    $prGpuCanvasRemove: function(canvas) {
        const entry = prGpuCanvases[canvas];
        if (entry && entry.owned && entry.canvas.close)
            entry.canvas.close();
        prGpuCanvases[canvas] = null;
    },

    prGpuCanvas2dAvailable: function() {
        return typeof OffscreenCanvas !== "undefined" || typeof document !== "undefined";
    },

    prGpuCanvas2dCreateCanvas__deps: ["$prGpuCanvasAdd"],
    prGpuCanvas2dCreateCanvas: function(width, height) {
        let canvas;
        if (typeof OffscreenCanvas !== "undefined") {
            canvas = new OffscreenCanvas(width, height);
        } else {
            canvas = document.createElement("canvas");
            canvas.width = width;
            canvas.height = height;
        }
        return prGpuCanvasAdd(canvas);
    },

    // Returns an entry for the page's canvas that `selector` names, sized to the extent, which clears it. The canvas is
    // opaque, which is how a swap chain shows it.
    prGpuCanvas2dAttach__deps: ["$prGpuCanvasAdd", "$prWebString"],
    prGpuCanvas2dAttach: function(selector, selectorSize, width, height) {
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        if (canvas === null)
            return 0;
        canvas.width = width;
        canvas.height = height;
        return prGpuCanvasAdd(canvas, {alpha: false});
    },

    prGpuCanvas2dRelease__deps: ["$prGpuCanvasRemove"],
    prGpuCanvas2dRelease: function(canvas) {
        prGpuCanvasRemove(canvas);
    },

    // Replaces the canvas's contents with a premultiplied color.
    prGpuCanvas2dClear__deps: ["$prGpuCanvases"],
    prGpuCanvas2dClear: function(canvas, r, g, b, a) {
        const {context} = prGpuCanvases[canvas];
        context.save();
        context.resetTransform();
        context.globalCompositeOperation = "copy";
        context.globalAlpha = 1;
        context.filter = "none";
        const s = a > 0 ? 255 / a : 0;
        context.fillStyle = `rgb(${r * s} ${g * s} ${b * s} / ${a})`;
        context.fillRect(0, 0, context.canvas.width, context.canvas.height);
        context.restore();
    },

    // Puts premultiplied texels into the canvas. ImageData holds them unpremultiplied.
    prGpuCanvas2dWrite__deps: ["$prGpuCanvases"],
    prGpuCanvas2dWrite: function(canvas, texels, stride, width, height, bgra) {
        const {context} = prGpuCanvases[canvas];
        const image = new ImageData(width, height);
        const out = image.data;
        const red = bgra ? 2 : 0;
        const blue = bgra ? 0 : 2;
        for (let y = 0; y < height; ++y) {
            let from = texels + y * stride;
            let to = y * width * 4;
            for (let x = 0; x < width; ++x, from += 4, to += 4) {
                const a = HEAPU8[from + 3];
                const s = a > 0 ? 255 / a : 0;
                out[to] = HEAPU8[from + red] * s;
                out[to + 1] = HEAPU8[from + 1] * s;
                out[to + 2] = HEAPU8[from + blue] * s;
                out[to + 3] = a;
            }
        }
        context.putImageData(image, 0, 0);
    },

    // Reads the canvas into premultiplied texels.
    prGpuCanvas2dRead__deps: ["$prGpuCanvases"],
    prGpuCanvas2dRead: function(canvas, texels, stride, width, height, bgra) {
        let {context} = prGpuCanvases[canvas];
        if (context === null) {
            // An image has no context of its own, so it is drawn into a temporary one to be read.
            const copy = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(width, height) :
                Object.assign(document.createElement("canvas"), {width, height});
            context = copy.getContext("2d");
            context.drawImage(prGpuCanvases[canvas].canvas, 0, 0, width, height);
        }
        const data = context.getImageData(0, 0, width, height).data;
        const red = bgra ? 2 : 0;
        const blue = bgra ? 0 : 2;
        for (let y = 0; y < height; ++y) {
            let to = texels + y * stride;
            let from = y * width * 4;
            for (let x = 0; x < width; ++x, from += 4, to += 4) {
                const a = data[from + 3];
                HEAPU8[to + red] = Math.round(data[from] * a / 255);
                HEAPU8[to + 1] = Math.round(data[from + 1] * a / 255);
                HEAPU8[to + blue] = Math.round(data[from + 2] * a / 255);
                HEAPU8[to + 3] = a;
            }
        }
    },
});

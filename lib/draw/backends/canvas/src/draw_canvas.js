// The page's side of the Canvas 2D renderer, which Emscripten links into the program (--js-library). It keeps each
// renderer's caches, and makes the calls of a play (draw_canvas.cpp). Each call draws on the top surface of the play in
// progress.

addToLibrary({
    // The renderers' caches, by the number that prDrawCanvasOpen returns, or null for a free slot. A renderer caches
    // the canvases of images and the paths of glyphs, each by its id, and the canvases that layers reuse. `play` is the
    // play in progress: its renderer, its stack of surfaces, its current path, and the lists that it drew.
    $prDrawCanvas__deps: ["$prGpuCanvases"],
    $prDrawCanvas: {
        renderers: [null],
        play: null,

        // The names of the composite operations, indexed by PrDrawComposite and then by PrDrawBlendMode after those.
        // The context has no plus-darker or hard mix, so each draws as the nearest operation that it has.
        operations: [
            "source-over", "destination-out", "copy", "source-in", "source-out", "source-atop", "destination-over",
            "destination-in", "destination-out", "destination-atop", "xor", "lighter", "source-over",
            "source-over", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge", "color-burn",
            "hard-light", "soft-light", "difference", "exclusion", "hard-light", "hue", "saturation", "color",
            "luminosity",
        ],
        caps: ["butt", "round", "square"],
        joins: ["miter", "round", "bevel"],
        rules: ["nonzero", "evenodd"],

        makeCanvas: function(width, height) {
            if (typeof OffscreenCanvas !== "undefined")
                return new OffscreenCanvas(width, height);
            const canvas = document.createElement("canvas");
            canvas.width = width;
            canvas.height = height;
            return canvas;
        },

        css: function(r, g, b, a) {
            return `rgb(${r * 255} ${g * 255} ${b * 255} / ${a})`;
        },

        // Pushes a transparent surface from the renderer's pool, at least `width` by `height`, with its context's state
        // reset. The surface's pixels are target pixels offset by (`x`, `y`).
        pushSurface: function(width, height, x, y) {
            const play = this.play;
            width = Math.max(width, 1);
            height = Math.max(height, 1);
            const pool = play.renderer.pool;
            const index = pool.findIndex(s => s.canvas.width >= width && s.canvas.height >= height);
            let surface;
            if (index >= 0) {
                surface = pool.splice(index, 1)[0];
            } else {
                const canvas = this.makeCanvas(width, height);
                surface = {canvas, context: canvas.getContext("2d")};
            }
            const c = surface.context;
            if (c.reset)
                c.reset();
            c.resetTransform();
            c.clearRect(0, 0, surface.canvas.width, surface.canvas.height);
            Object.assign(surface, {x, y, width, height, pooled: true});
            play.surfaces.push(surface);
            play.top = surface;
            play.context = c;
            return surface;
        },

        popSurface: function() {
            const play = this.play;
            const surface = play.surfaces.pop();
            play.top = play.surfaces[play.surfaces.length - 1];
            play.context = play.top.context;
            return surface;
        },

        release: function(surface) {
            const pool = this.play.renderer.pool;
            if (surface.pooled && pool.length < 16)
                pool.push(surface);
        },

        // Returns a path of PrPathVerbs and their points.
        path: function(verbs, count, points) {
            const p = new Path2D();
            let at = points >>> 2;
            for (let i = 0; i < count; ++i) {
                switch (HEAPU8[verbs + i]) {
                    case 0: p.moveTo(HEAPF32[at], HEAPF32[at + 1]); at += 2; break;
                    case 1: p.lineTo(HEAPF32[at], HEAPF32[at + 1]); at += 2; break;
                    case 2: p.quadraticCurveTo(HEAPF32[at], HEAPF32[at + 1], HEAPF32[at + 2], HEAPF32[at + 3]);
                        at += 4; break;
                    case 3: p.bezierCurveTo(HEAPF32[at], HEAPF32[at + 1], HEAPF32[at + 2], HEAPF32[at + 3],
                        HEAPF32[at + 4], HEAPF32[at + 5]); at += 6; break;
                    default: p.closePath(); break;
                }
            }
            return p;
        },

        stops: function(gradient, stops, count) {
            let at = stops >>> 2;
            for (let i = 0; i < count; ++i, at += 5) {
                const color = this.css(HEAPF32[at + 1], HEAPF32[at + 2], HEAPF32[at + 3], HEAPF32[at + 4]);
                gradient.addColorStop(HEAPF32[at], color);
            }
            return gradient;
        },

        filters: function(filters, count) {
            let at = filters >>> 2;
            const out = [];
            for (let i = 0; i < count; ++i) {
                if (HEAPF32[at] === 0) {
                    out.push(`blur(${HEAPF32[at + 1]}px)`);
                    at += 2;
                } else {
                    const color = this.css(HEAPF32[at + 4], HEAPF32[at + 5], HEAPF32[at + 6], HEAPF32[at + 7]);
                    // The third length of drop-shadow() is the blur's standard deviation, not a blur radius as in
                    // box-shadow.
                    const dx = HEAPF32[at + 1], dy = HEAPF32[at + 2], sigma = HEAPF32[at + 3];
                    out.push(`drop-shadow(${dx}px ${dy}px ${sigma}px ${color})`);
                    at += 8;
                }
            }
            return out.length !== 0 ? out.join(" ") : "none";
        },

        // A canvas of 8-bit texels, unpremultiplied, which is how ImageData holds them.
        canvasOfTexels: function(pointer, stride, width, height, flags) {
            const image = new ImageData(width, height);
            const out = image.data;
            const red = flags & 1 ? 2 : 0;
            const blue = flags & 1 ? 0 : 2;
            const straight = (flags & 2) !== 0;
            const opaque = (flags & 4) !== 0;
            for (let y = 0; y < height; ++y) {
                let from = pointer + y * stride;
                let to = y * width * 4;
                for (let x = 0; x < width; ++x, from += 4, to += 4) {
                    const a = opaque ? 255 : HEAPU8[from + 3];
                    const s = straight || opaque ? 1 : a > 0 ? 255 / a : 0;
                    out[to] = HEAPU8[from + red] * s;
                    out[to + 1] = HEAPU8[from + 1] * s;
                    out[to + 2] = HEAPU8[from + blue] * s;
                    out[to + 3] = a;
                }
            }
            const canvas = this.makeCanvas(width, height);
            canvas.getContext("2d").putImageData(image, 0, 0);
            return canvas;
        },

        // Returns the canvas of a source, and the source's cache of crops if it keeps one.
        source: function(words) {
            const at = words >>> 2;
            switch (HEAPU32[at]) {
                case 0: {
                    const key = HEAPU32[at + 1] + ":" + HEAPU32[at + 2];
                    const images = this.play.renderer.images;
                    let entry = images.get(key);
                    if (entry === undefined) {
                        const canvas = this.canvasOfTexels(HEAPU32[at + 3], HEAPU32[at + 4], HEAPU32[at + 5],
                            HEAPU32[at + 6], HEAPU32[at + 7]);
                        entry = {canvas, crops: new Map()};
                        images.set(key, entry);
                    }
                    entry.used = this.play.serial;
                    return entry;
                }
                case 1:
                    return {canvas: prGpuCanvases[HEAPU32[at + 1]].canvas};
                case 2:
                    return {canvas: this.play.lists[HEAPU32[at + 1]]};
                default:
                    return {canvas: this.canvasOfTexels(HEAPU32[at + 1], HEAPU32[at + 2], HEAPU32[at + 3],
                        HEAPU32[at + 4], HEAPU32[at + 5])};
            }
        },

        // Returns the texels of a source within a crop: a canvas of their own, unless the crop covers all of them.
        crop: function(source, x, y, width, height) {
            const canvas = source.canvas;
            // A VideoFrame's extent is its display size, and a video element's its video's.
            const fullWidth = canvas.displayWidth ?? canvas.videoWidth ?? canvas.width;
            const fullHeight = canvas.displayHeight ?? canvas.videoHeight ?? canvas.height;
            if (x === 0 && y === 0 && width === fullWidth && height === fullHeight)
                return canvas;
            const key = x + "," + y + "," + width + "," + height;
            let crop = source.crops !== undefined ? source.crops.get(key) : undefined;
            if (crop === undefined) {
                crop = this.makeCanvas(width, height);
                crop.getContext("2d").drawImage(canvas, x, y, width, height, 0, 0, width, height);
                if (source.crops !== undefined)
                    source.crops.set(key, crop);
            }
            return crop;
        },

        sampling: function(c, sampling) {
            c.imageSmoothingEnabled = sampling !== 0;
            if (sampling !== 0)
                c.imageSmoothingQuality = sampling === 2 ? "high" : "low";
        },
    },

    prDrawCanvasOpen__deps: ["$prDrawCanvas"],
    prDrawCanvasOpen: function() {
        const renderers = prDrawCanvas.renderers;
        let slot = renderers.indexOf(null, 1);
        if (slot < 0)
            slot = renderers.push(null) - 1;
        renderers[slot] = {images: new Map(), glyphs: new Map(), pool: [], plays: 0};
        return slot;
    },

    prDrawCanvasClose__deps: ["$prDrawCanvas"],
    prDrawCanvasClose: function(renderer) {
        prDrawCanvas.renderers[renderer] = null;
    },

    prDrawCanvasBegin__deps: ["$prDrawCanvas", "$prGpuCanvases"],
    prDrawCanvasBegin: function(renderer, canvas, originX, originY) {
        const state = prDrawCanvas.renderers[renderer];
        const root = prGpuCanvases[canvas];
        const top = {canvas: root.canvas, context: root.context, x: originX, y: originY, pooled: false};
        prDrawCanvas.play = {renderer: state, serial: ++state.plays, surfaces: [top], top, context: root.context,
            path: null, lists: [], image: null};
        root.context.save();
    },

    // Ends the play, and drops what no play has drawn for a while.
    prDrawCanvasEnd__deps: ["$prDrawCanvas"],
    prDrawCanvasEnd: function() {
        const play = prDrawCanvas.play;
        play.context.restore();
        prDrawCanvas.play = null;
        const serial = play.serial;
        if (serial % 64 !== 0)
            return;
        for (const cache of [play.renderer.images, play.renderer.glyphs]) {
            for (const [key, entry] of cache) {
                if (serial - entry.used > 240)
                    cache.delete(key);
            }
        }
    },

    prDrawCanvasSave__deps: ["$prDrawCanvas"],
    prDrawCanvasSave: function() {
        prDrawCanvas.play.context.save();
    },

    prDrawCanvasRestore__deps: ["$prDrawCanvas"],
    prDrawCanvasRestore: function() {
        prDrawCanvas.play.context.restore();
    },

    prDrawCanvasTransform__deps: ["$prDrawCanvas"],
    prDrawCanvasTransform: function(a, b, c, d, e, f) {
        const play = prDrawCanvas.play;
        play.context.setTransform(a, b, c, d, e + play.top.x, f + play.top.y);
    },

    prDrawCanvasPath__deps: ["$prDrawCanvas"],
    prDrawCanvasPath: function(verbs, count, points) {
        prDrawCanvas.play.path = prDrawCanvas.path(verbs, count, points);
    },

    prDrawCanvasPolyline__deps: ["$prDrawCanvas"],
    prDrawCanvasPolyline: function(points, pointCount, contours, contourCount, closed) {
        const p = new Path2D();
        const base = points >>> 2;
        for (let c = 0; c < contourCount; ++c) {
            const start = HEAPU32[(contours >>> 2) + c];
            const end = c + 1 < contourCount ? HEAPU32[(contours >>> 2) + c + 1] : pointCount;
            for (let i = start; i < end; ++i) {
                const at = base + i * 2;
                if (i === start)
                    p.moveTo(HEAPF32[at], HEAPF32[at + 1]);
                else
                    p.lineTo(HEAPF32[at], HEAPF32[at + 1]);
            }
            if ((HEAPU32[(closed >>> 2) + (c >>> 5)] >>> (c & 31)) & 1)
                p.closePath();
        }
        prDrawCanvas.play.path = p;
    },

    prDrawCanvasRect__deps: ["$prDrawCanvas"],
    prDrawCanvasRect: function(left, top, right, bottom) {
        const p = new Path2D();
        p.rect(left, top, right - left, bottom - top);
        prDrawCanvas.play.path = p;
    },

    // Makes the glyph's outline the current path. The path is cached by its id across plays.
    prDrawCanvasGlyph__deps: ["$prDrawCanvas"],
    prDrawCanvasGlyph: function(id, verbs, count, points) {
        const play = prDrawCanvas.play;
        const glyphs = play.renderer.glyphs;
        let entry = glyphs.get(id);
        if (entry === undefined) {
            entry = {path: prDrawCanvas.path(verbs, count, points)};
            glyphs.set(id, entry);
        }
        entry.used = play.serial;
        play.path = entry.path;
    },

    prDrawCanvasMapPath__deps: ["$prDrawCanvas"],
    prDrawCanvasMapPath: function(a, b, c, d, e, f) {
        const play = prDrawCanvas.play;
        const mapped = new Path2D();
        mapped.addPath(play.path, new DOMMatrix([a, b, c, d, e, f]));
        play.path = mapped;
    },

    prDrawCanvasColor__deps: ["$prDrawCanvas"],
    prDrawCanvasColor: function(r, g, b, a) {
        prDrawCanvas.play.image = null;
        const context = prDrawCanvas.play.context;
        context.fillStyle = context.strokeStyle = prDrawCanvas.css(r, g, b, a);
    },

    prDrawCanvasLinear__deps: ["$prDrawCanvas"],
    prDrawCanvasLinear: function(x0, y0, x1, y1, stops, count) {
        prDrawCanvas.play.image = null;
        const context = prDrawCanvas.play.context;
        context.fillStyle = prDrawCanvas.stops(context.createLinearGradient(x0, y0, x1, y1), stops, count);
    },

    prDrawCanvasRadial__deps: ["$prDrawCanvas"],
    prDrawCanvasRadial: function(x0, y0, r0, x1, y1, r1, stops, count) {
        prDrawCanvas.play.image = null;
        const context = prDrawCanvas.play.context;
        context.fillStyle = prDrawCanvas.stops(context.createRadialGradient(x0, y0, r0, x1, y1, r1), stops, count);
    },

    prDrawCanvasConic__deps: ["$prDrawCanvas"],
    prDrawCanvasConic: function(angle, x, y, stops, count) {
        prDrawCanvas.play.image = null;
        const context = prDrawCanvas.play.context;
        context.fillStyle = prDrawCanvas.stops(context.createConicGradient(angle, x, y), stops, count);
    },

    // Sets a pattern of the crop's texels, which extends along each axis as PrDrawExtend says. The matrix maps the
    // crop's texels into the space of the transform. `outset` holds the number of texels that a padded axis adds to the
    // left, top, right and bottom of the crop, copies of its edge texels. Reflect repeats the crop beside its mirror
    // image.
    prDrawCanvasPattern__deps: ["$prDrawCanvas"],
    prDrawCanvasPattern: function(words, x, y, width, height, extendX, extendY, outset, a, b, c, d, e, f, sampling) {
        prDrawCanvas.play.image = null;
        const context = prDrawCanvas.play.context;
        const source = prDrawCanvas.source(words);
        if (source.canvas === null || source.canvas === undefined) {
            context.fillStyle = "transparent";
            return;
        }
        const Repeat = 1, Reflect = 2;
        let image = prDrawCanvas.crop(source, x, y, width, height);
        if (extendX === Reflect || extendY === Reflect) {
            const across = extendX === Reflect ? 2 : 1, down = extendY === Reflect ? 2 : 1;
            const mirrored = prDrawCanvas.makeCanvas(width * across, height * down);
            const m = mirrored.getContext("2d");
            for (let j = 0; j < down; ++j) {
                for (let i = 0; i < across; ++i) {
                    m.setTransform(i ? -1 : 1, 0, 0, j ? -1 : 1, i ? width * 2 : 0, j ? height * 2 : 0);
                    m.drawImage(image, 0, 0);
                }
            }
            image = mirrored;
        }
        const left = HEAPU32[outset >>> 2], top = HEAPU32[(outset >>> 2) + 1];
        const right = HEAPU32[(outset >>> 2) + 2], bottom = HEAPU32[(outset >>> 2) + 3];
        prDrawCanvas.sampling(context, sampling);
        // A fill within the tile of an image padded on both axes needs its edge texels only for sampling, which
        // drawImage clamps to the image, so the fill draws the image through a clip of its path, without a copy
        // (prDrawCanvasFill). A pattern would sample transparency past its edges.
        if (left === 1 && top === 1 && right === 1 && bottom === 1) {
            prDrawCanvas.play.image = {image, matrix: [a, b, c, d, e, f]};
            context.fillStyle = "transparent";
            return;
        }
        if (left + top + right + bottom !== 0) {
            const padded = prDrawCanvas.makeCanvas(left + width + right, top + height + bottom);
            const p = padded.getContext("2d");
            p.imageSmoothingEnabled = false;
            // The edges' texels stretch outward, and the corners' texels fill the corners.
            const xs = [[0, 1, 0, left], [0, width, left, width], [width - 1, 1, left + width, right]];
            const ys = [[0, 1, 0, top], [0, height, top, height], [height - 1, 1, top + height, bottom]];
            for (const [sx, sw, dx, dw] of xs) {
                for (const [sy, sh, dy, dh] of ys) {
                    if (dw !== 0 && dh !== 0)
                        p.drawImage(image, sx, sy, sw, sh, dx, dy, dw, dh);
                }
            }
            image = padded;
        }
        const repeatX = extendX === Repeat || extendX === Reflect, repeatY = extendY === Repeat || extendY === Reflect;
        const repetition = repeatX ? (repeatY ? "repeat" : "repeat-x") : (repeatY ? "repeat-y" : "no-repeat");
        const pattern = context.createPattern(image, repetition);
        pattern.setTransform(new DOMMatrix([a, b, c, d, e, f]).translate(-left, -top));
        context.fillStyle = pattern;
    },

    prDrawCanvasComposite__deps: ["$prDrawCanvas"],
    prDrawCanvasComposite: function(operation) {
        prDrawCanvas.play.context.globalCompositeOperation = prDrawCanvas.operations[operation];
    },

    prDrawCanvasAlpha__deps: ["$prDrawCanvas"],
    prDrawCanvasAlpha: function(alpha) {
        prDrawCanvas.play.context.globalAlpha = alpha;
    },

    prDrawCanvasFill__deps: ["$prDrawCanvas"],
    prDrawCanvasFill: function(rule) {
        const play = prDrawCanvas.play;
        const context = play.context;
        if (play.image === null) {
            context.fill(play.path, prDrawCanvas.rules[rule]);
            return;
        }
        context.save();
        context.clip(play.path, prDrawCanvas.rules[rule]);
        context.transform(...play.image.matrix);
        context.drawImage(play.image.image, 0, 0);
        context.restore();
    },

    prDrawCanvasStroke__deps: ["$prDrawCanvas"],
    prDrawCanvasStroke: function(width, cap, join, miterLimit, dashOffset, dashes, dashCount) {
        const play = prDrawCanvas.play;
        const context = play.context;
        context.lineWidth = width;
        context.lineCap = prDrawCanvas.caps[cap];
        context.lineJoin = prDrawCanvas.joins[join];
        context.miterLimit = miterLimit;
        if (dashCount !== 0) {
            const pattern = Array.from(HEAPF32.subarray(dashes >>> 2, (dashes >>> 2) + dashCount));
            context.setLineDash(dashCount % 2 === 1 ? pattern.concat(pattern) : pattern);
            context.lineDashOffset = dashOffset;
        }
        context.stroke(play.path);
        if (dashCount !== 0)
            context.setLineDash([]);
    },

    prDrawCanvasClip__deps: ["$prDrawCanvas"],
    prDrawCanvasClip: function(rule) {
        const play = prDrawCanvas.play;
        play.context.clip(play.path, prDrawCanvas.rules[rule]);
    },

    // Replaces the surface, within its clip, with the color.
    prDrawCanvasClear__deps: ["$prDrawCanvas"],
    prDrawCanvasClear: function(r, g, b, a) {
        const play = prDrawCanvas.play;
        const context = play.context;
        context.save();
        context.resetTransform();
        context.globalCompositeOperation = "copy";
        context.globalAlpha = 1;
        context.fillStyle = prDrawCanvas.css(r, g, b, a);
        context.fillRect(0, 0, play.top.canvas.width, play.top.canvas.height);
        context.restore();
    },

    // Draws only the shadow of the current path, in target pixels. The path itself is drawn `shift` pixels along x, off
    // the surface, and its shadow is offset back by `shift`.
    prDrawCanvasShadowFill__deps: ["$prDrawCanvas"],
    prDrawCanvasShadowFill: function(shift, sigma, r, g, b, a, rule) {
        const play = prDrawCanvas.play;
        const context = play.context;
        context.save();
        context.setTransform(1, 0, 0, 1, play.top.x + shift, play.top.y);
        context.shadowOffsetX = -shift;
        context.shadowOffsetY = 0;
        context.shadowBlur = sigma * 2;
        context.shadowColor = prDrawCanvas.css(r, g, b, a);
        context.fillStyle = "#000";
        context.fill(play.path, prDrawCanvas.rules[rule]);
        context.restore();
    },

    // Pushes a transparent layer over a box of target pixels.
    prDrawCanvasPushLayer__deps: ["$prDrawCanvas"],
    prDrawCanvasPushLayer: function(left, top, width, height) {
        const surface = prDrawCanvas.pushSurface(width, height, -left, -top);
        surface.left = left;
        surface.top = top;
    },

    // Composites the layer onto the surface below.
    prDrawCanvasPopLayer__deps: ["$prDrawCanvas"],
    prDrawCanvasPopLayer: function(alpha, operation, filters, count) {
        const layer = prDrawCanvas.popSurface();
        const play = prDrawCanvas.play;
        const context = play.context;
        context.save();
        context.resetTransform();
        context.globalAlpha = alpha;
        context.globalCompositeOperation = prDrawCanvas.operations[operation];
        context.filter = prDrawCanvas.filters(filters, count);
        context.imageSmoothingEnabled = false;
        context.drawImage(layer.canvas, 0, 0, layer.width, layer.height, layer.left + play.top.x,
            layer.top + play.top.y, layer.width, layer.height);
        context.restore();
        prDrawCanvas.release(layer);
    },

    // Draws the surface below, filtered, into the layer just pushed.
    prDrawCanvasBackdrop__deps: ["$prDrawCanvas"],
    prDrawCanvasBackdrop: function(filters, count) {
        const play = prDrawCanvas.play;
        const top = play.top;
        const below = play.surfaces[play.surfaces.length - 2];
        const context = play.context;
        context.save();
        context.filter = prDrawCanvas.filters(filters, count);
        context.drawImage(below.canvas, top.left + below.x, top.top + below.y, top.width, top.height, 0, 0, top.width,
            top.height);
        context.restore();
    },

    prDrawCanvasPushMask__deps: ["$prDrawCanvas"],
    prDrawCanvasPushMask: function() {
        const layer = prDrawCanvas.play.top;
        const mask = prDrawCanvas.pushSurface(layer.width, layer.height, layer.x, layer.y);
        mask.left = layer.left;
        mask.top = layer.top;
    },

    // Multiplies the layer by the mask's alpha or luminance, or by one minus either.
    prDrawCanvasPopMask__deps: ["$prDrawCanvas"],
    prDrawCanvasPopMask: function(luminance, invert) {
        const mask = prDrawCanvas.popSurface();
        if (luminance) {
            const image = mask.context.getImageData(0, 0, mask.width, mask.height);
            const d = image.data;
            for (let i = 0; i < d.length; i += 4) {
                const l = (0.2126 * d[i] + 0.7152 * d[i + 1] + 0.0722 * d[i + 2]) * d[i + 3] / 255;
                d[i] = d[i + 1] = d[i + 2] = 0;
                d[i + 3] = l;
            }
            mask.context.putImageData(image, 0, 0);
        }
        const context = prDrawCanvas.play.context;
        context.save();
        context.resetTransform();
        context.globalAlpha = 1;
        context.globalCompositeOperation = invert ? "destination-out" : "destination-in";
        context.drawImage(mask.canvas, 0, 0, mask.width, mask.height, 0, 0, mask.width, mask.height);
        context.restore();
        prDrawCanvas.release(mask);
    },

    prDrawCanvasPushList__deps: ["$prDrawCanvas"],
    prDrawCanvasPushList: function(width, height) {
        prDrawCanvas.pushSurface(width, height, 0, 0);
    },

    // Keeps the list's surface as the play's next list, for the sources that draw it. Returns the list's index.
    prDrawCanvasPopList__deps: ["$prDrawCanvas"],
    prDrawCanvasPopList: function() {
        const list = prDrawCanvas.popSurface();
        const canvas = prDrawCanvas.makeCanvas(list.width, list.height);
        canvas.getContext("2d").drawImage(list.canvas, 0, 0);
        prDrawCanvas.release(list);
        return prDrawCanvas.play.lists.push(canvas) - 1;
    },

    prDrawCanvasDrawImage__deps: ["$prDrawCanvas"],
    prDrawCanvasDrawImage: function(words, sx, sy, sw, sh, dx, dy, dw, dh, sampling) {
        const source = prDrawCanvas.source(words);
        if (source.canvas === null || source.canvas === undefined)
            return;
        const context = prDrawCanvas.play.context;
        prDrawCanvas.sampling(context, sampling);
        context.drawImage(source.canvas, sx, sy, sw, sh, dx, dy, dw, dh);
    },

    // Replaces target pixels with the texels, ignoring the clip and the transform.
    prDrawCanvasPutPixels__deps: ["$prDrawCanvas"],
    prDrawCanvasPutPixels: function(texels, stride, width, height, x, y, flags) {
        const play = prDrawCanvas.play;
        const canvas = prDrawCanvas.canvasOfTexels(texels, stride, width, height, flags);
        play.context.putImageData(canvas.getContext("2d").getImageData(0, 0, width, height), x + play.top.x,
            y + play.top.y);
    },
});

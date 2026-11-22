// The page's side of the WebGPU backend, which Emscripten links into the program (--js-library). It keeps the WebGPU
// objects of each device by the ids that gpu_webgpu.cpp gives them, and decodes each submitted command buffer's words
// into WebGPU's commands.
//
// The bind groups are the backend's own. A pipeline's layout follows the bindings that its WGSL declares: push data at
// group 0, and the uniforms and the slots at group 1, at their Vulkan bindings (pr/gpu.slang). Before each draw or
// dispatch, the bindings of the pipeline take the command buffer's root data and slots, through bind groups that the
// pipeline keeps by what they hold.

addToLibrary({
    // The devices, by id. An entry holds the GPUDevice and its objects: `objects` by resource id, `samplers` and
    // `pipelines` by their ids, and `canvases` by id. Zero names none.
    $prGpuWebgpuDevices: [null],

    // The page's names of lib/gpu's enumerations, by their values.
    $prGpuWebgpuNames: {
        // The page's texture formats, in the order of gpu_webgpu.cpp's WebgpuFormat, which the backend translates
        // lib/gpu's formats to. ASTC's follow, Unorm and Srgb by block size.
        formats: [
            "", "r8unorm", "rg8unorm", "r16unorm", "rg16unorm", "rgba8unorm", "rgba8unorm-srgb", "bgra8unorm",
            "bgra8unorm-srgb", "rgb10a2unorm", "rg11b10ufloat", "r16float", "rg16float", "rgba16float", "r32float",
            "rg32float", "rgba32float", "r8uint", "r16uint", "r32uint", "rg32uint", "rgba32uint", "depth16unorm",
            "depth32float", "depth32float-stencil8", "bc1-rgba-unorm", "bc1-rgba-unorm-srgb", "bc2-rgba-unorm",
            "bc2-rgba-unorm-srgb", "bc3-rgba-unorm", "bc3-rgba-unorm-srgb", "bc4-r-unorm", "bc4-r-snorm",
            "bc5-rg-unorm", "bc5-rg-snorm", "bc6h-rgb-ufloat", "bc6h-rgb-float", "bc7-rgba-unorm",
            "bc7-rgba-unorm-srgb", "etc2-rgb8unorm", "etc2-rgb8unorm-srgb", "etc2-rgb8a1unorm",
            "etc2-rgb8a1unorm-srgb", "etc2-rgba8unorm", "etc2-rgba8unorm-srgb", "eac-r11unorm", "eac-r11snorm",
            "eac-rg11unorm", "eac-rg11snorm",
            ...["4x4", "5x4", "5x5", "6x5", "6x6", "8x5", "8x6", "8x8", "10x5", "10x6", "10x8", "10x10", "12x10",
                "12x12"].flatMap(size => ["astc-" + size + "-unorm", "astc-" + size + "-unorm-srgb"])],
        vertexFormats: ["", "float32", "float32x2", "float32x3", "float32x4", "float16x2", "float16x4", "uint32",
            "uint32x2", "uint32x3", "uint32x4", "sint32", "sint32x2", "sint32x3", "sint32x4", "unorm8x4", "snorm8x4",
            "uint8x4", "unorm16x2", "unorm16x4", "snorm16x2", "snorm16x4"],
        dimensions: ["2d", "2d-array", "cube", "cube-array", "3d"],
        topologies: ["triangle-list", "triangle-strip", "line-list", "line-strip", "point-list"],
        culls: ["none", "front", "back"],
        compares: ["never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always"],
        stencilOps: ["keep", "zero", "replace", "increment-clamp", "decrement-clamp", "invert", "increment-wrap",
            "decrement-wrap"],
        blendFactors: ["zero", "one", "src", "one-minus-src", "src-alpha", "one-minus-src-alpha", "dst",
            "one-minus-dst", "dst-alpha", "one-minus-dst-alpha", "src-alpha-saturated", "constant",
            "one-minus-constant", "src1", "one-minus-src1"],
        blendOps: ["add", "subtract", "reverse-subtract", "min", "max"],
        addresses: ["clamp-to-edge", "repeat", "mirror-repeat", "clamp-to-edge"],
        filters: ["nearest", "linear"],
    },

    // Returns true if the program suspends while it waits for the page (JSPI), so that a device's creation and a wait
    // for a fence finish during the call.
    prGpuWebgpuWaits: function() {
#if ASYNCIFY
        return true;
#else
        return false;
#endif
    },

    // Requests a device for the C++ device `owner`, with timestamps where the adapter has them, and tells it once the
    // page has made one. Returns false at once if the page has no WebGPU. With `wait`, in a program that suspends,
    // returns once the page has made the device or failed to.
    // The optional features of WebGPU that a device reports to gpu_webgpu.cpp, as bits in this order (WebgpuFeature).
    $prGpuWebgpuFeatureNames: ["timestamp-query", "float32-filterable", "texture-compression-bc",
        "texture-compression-astc", "rg11b10ufloat-renderable", "texture-compression-etc2"],

    prGpuWebgpuRequest__deps: ["$prGpuWebgpuFeatureNames", "$prGpuWebgpuDevices", "prGpuWebgpuCreated",
        "prGpuWebgpuLost", "prGpuWebgpuError"],
    prGpuWebgpuRequest__async: true,
    prGpuWebgpuRequest: function(owner, wait) {
        if (typeof navigator === "undefined" || !navigator.gpu)
            return false;
        const request = navigator.gpu.requestAdapter().then(adapter => {
            if (!adapter)
                return null;
            // The features that the device takes where the adapter has them: those that it reports, and
            // "indirect-first-instance", without which an indirect draw whose first instance is not zero draws nothing.
            const requiredFeatures = ["indirect-first-instance", ...prGpuWebgpuFeatureNames]
                .filter(f => adapter.features.has(f));
            // What the adapter allows rather than the defaults: the least alignment of a buffer bound to a slot, and
            // the largest textures and buffers.
            const limits = ["minStorageBufferOffsetAlignment", "maxTextureDimension2D", "maxBufferSize",
                "maxStorageBufferBindingSize"];
            const requiredLimits = Object.fromEntries(limits.map(name => [name, adapter.limits[name]]));
            return adapter.requestDevice({requiredFeatures, requiredLimits});
        }).then(gpu => {
            if (!gpu) {
                _prGpuWebgpuCreated(owner, 0, 0, 0, 0);
                return;
            }
            const entry = {gpu, owner, objects: [null], samplers: [null], pipelines: [null], canvases: [null],
                modules: new Map(), sources: new Map(), serial: 0, uploads: new Map(), chain: Promise.resolve(),
                released: false, timestamps: gpu.features.has("timestamp-query"), clocks: []};
            const id = prGpuWebgpuDevices.push(entry) - 1;
            gpu.onuncapturederror = event => {
                console.error("lib/gpu: WebGPU: " + event.error.message);
                if (!entry.released)
                    _prGpuWebgpuError(owner);
            };
            gpu.lost.then(() => {
                if (!entry.released)
                    _prGpuWebgpuLost(owner);
            });
            const features = prGpuWebgpuFeatureNames.reduce((bits, f, i) => bits | (gpu.features.has(f) ? 1 << i : 0), 0);
            _prGpuWebgpuCreated(owner, id, gpu.limits.maxTextureDimension2D, gpu.limits.minStorageBufferOffsetAlignment,
                features);
        }).catch(() => _prGpuWebgpuCreated(owner, 0, 0, 0, 0));
#if ASYNCIFY
        if (wait)
            return request.then(() => true);
#endif
        return true;
    },

    // Returns once the page has reached the fences of the work submitted so far, in a program that suspends.
    prGpuWebgpuWait__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuWait__async: true,
    prGpuWebgpuWait: function(device) {
#if ASYNCIFY
        return prGpuWebgpuDevices[device].chain;
#endif
    },

    prGpuWebgpuRelease__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuRelease: function(device) {
        const d = prGpuWebgpuDevices[device];
        d.released = true;
        d.gpu.destroy();
        prGpuWebgpuDevices[device] = null;
    },

    prGpuWebgpuSimulateLoss__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuSimulateLoss: function(device) {
        prGpuWebgpuDevices[device].gpu.destroy();
    },

    // Makes a buffer of `memory`: device memory, upload memory that each submission copies, readback memory that the
    // work's end copies back into `shadow`, or a stream block, whose used part each submission copies.
    prGpuWebgpuBufferCreate__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuBufferCreate: function(device, id, size, memory, shadow) {
        const d = prGpuWebgpuDevices[device];
        size = Number(size);
        const usage = memory === 2 ? GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST :
            GPUBufferUsage.STORAGE | GPUBufferUsage.UNIFORM | GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX |
            GPUBufferUsage.INDIRECT | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST;
        let buffer;
        try {
            buffer = d.gpu.createBuffer({size, usage});
        } catch (error) {
            return false;
        }
        const object = {buffer, size, memory, shadow, serial: ++d.serial};
        d.objects[id] = object;
        if (memory === 1)
            d.uploads.set(id, object);
        return true;
    },

    prGpuWebgpuResourceRelease__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuResourceRelease: function(device, id) {
        const d = prGpuWebgpuDevices[device];
        const object = d.objects[id];
        if (object) {
            if (object.buffer)
                object.buffer.destroy();
            else if (object.texture && object.owned)
                object.texture.destroy();
        }
        d.uploads.delete(id);
        d.objects[id] = null;
    },

    // Writes bytes into a buffer, in order with the queue's work. WebGPU writes whole words, so the bytes of a word
    // that the write covers in part are merged into it on the GPU.
    prGpuWebgpuBufferWrite__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuBufferWrite: function(device, id, offset, data, size) {
        const d = prGpuWebgpuDevices[device];
        const buffer = d.objects[id].buffer;
        offset = Number(offset);
        size = Number(size);
        const end = offset + size;
        const first = Math.min((offset + 3) & ~3, end);
        const last = Math.max(end & ~3, first);
        if (first < last)
            d.gpu.queue.writeBuffer(buffer, first, HEAPU8, data + (first - offset), last - first);
        // The words covered in part, as [word, value, mask].
        const patches = new Map();
        const part = at => {
            const word = at >>> 2;
            const patch = patches.get(word) || [word, 0, 0];
            patch[1] |= HEAPU8[data + at - offset] << ((at & 3) * 8);
            patch[2] |= 0xff << ((at & 3) * 8);
            patches.set(word, patch);
        };
        for (let at = offset; at < first; ++at)
            part(at);
        for (let at = last; at < end; ++at)
            part(at);
        if (patches.size === 0)
            return;
        if (!d.merge) {
            const code = "struct Patch { word: u32, value: u32, mask: u32, unused: u32 }\n" +
                "@group(0) @binding(0) var<storage, read_write> words: array<u32>;\n" +
                "@group(0) @binding(1) var<storage, read> patches: array<Patch>;\n" +
                "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3u) {\n" +
                "    let p = patches[id.x];\n" +
                "    words[p.word] = (words[p.word] & ~p.mask) | (p.value & p.mask);\n" +
                "}\n";
            const module = d.gpu.createShaderModule({code});
            d.merge = d.gpu.createComputePipeline({layout: "auto", compute: {module, entryPoint: "main"}});
        }
        const values = new Uint32Array(patches.size * 4);
        [...patches.values()].forEach((p, i) => values.set([p[0], p[1] >>> 0, p[2] >>> 0, 0], i * 4));
        const merged = d.gpu.createBuffer({size: values.byteLength,
            usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST});
        d.gpu.queue.writeBuffer(merged, 0, values);
        const encoder = d.gpu.createCommandEncoder();
        const pass = encoder.beginComputePass();
        pass.setPipeline(d.merge);
        pass.setBindGroup(0, d.gpu.createBindGroup({layout: d.merge.getBindGroupLayout(0), entries: [
            {binding: 0, resource: {buffer}}, {binding: 1, resource: {buffer: merged}}]}));
        pass.dispatchWorkgroups(patches.size);
        pass.end();
        d.gpu.queue.submit([encoder.finish()]);
        merged.destroy();
    },

    $prGpuWebgpuTextureUsage: function(usage, samples) {
        let flags = GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST;
        if (usage & 1)
            flags |= GPUTextureUsage.TEXTURE_BINDING;
        if (usage & 2)
            flags |= GPUTextureUsage.STORAGE_BINDING;
        if (usage & 12)
            flags |= GPUTextureUsage.RENDER_ATTACHMENT;
        // A texture of several samples is only drawn into and resolved.
        if (samples > 1)
            flags &= GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING;
        return flags;
    },

    prGpuWebgpuTextureCreate__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuNames", "$prGpuWebgpuTextureUsage",
        "$prGpuWebgpuSampledView"],
    prGpuWebgpuTextureCreate: function(device, id, kind, format, viewFormat, usage, levels, samples, width, height,
            depth) {
        const d = prGpuWebgpuDevices[device];
        const names = prGpuWebgpuNames;
        let texture;
        try {
            texture = d.gpu.createTexture({
                size: [width, height, depth],
                format: names.formats[format],
                usage: prGpuWebgpuTextureUsage(usage, samples),
                mipLevelCount: levels,
                sampleCount: samples,
                dimension: kind === 4 ? "3d" : "2d",
                viewFormats: viewFormat !== 0 ? [names.formats[viewFormat]] : [],
            });
        } catch (error) {
            return false;
        }
        const object = {texture, owned: true, format: names.formats[format], kind, attachments: new Map(),
            serial: ++d.serial, baseLevel: 0, baseLayer: 0, levels, layers: kind === 4 ? 1 : depth};
        object.view = prGpuWebgpuSampledView(object, 0, levels, 0, object.layers);
        d.objects[id] = object;
        return true;
    },

    // Returns the view through which a shader samples levels and layers of `object`'s texture, as a texture of its own
    // kind. A shader samples the depth of a texture of depth and stencil.
    $prGpuWebgpuSampledView__deps: ["$prGpuWebgpuNames"],
    $prGpuWebgpuSampledView: function(object, firstLevel, levels, firstLayer, layers) {
        const desc = {
            format: object.format,
            dimension: prGpuWebgpuNames.dimensions[object.kind],
            baseMipLevel: firstLevel,
            mipLevelCount: levels,
            baseArrayLayer: firstLayer,
            arrayLayerCount: layers,
        };
        if (object.format.includes("stencil"))
            desc.aspect = "depth-only";
        return object.texture.createView(desc);
    },

    // Returns the view through which a shader reads and writes `object` without a sampler: its first level, made on
    // first use.
    $prGpuWebgpuStorageView: function(object) {
        if (!object.storageView) {
            object.storageView = object.texture.createView({
                format: object.format,
                dimension: object.kind === 4 ? "3d" : object.kind === 0 ? "2d" : "2d-array",
                baseMipLevel: object.baseLevel, mipLevelCount: 1,
                baseArrayLayer: object.baseLayer, arrayLayerCount: object.layers,
            });
        }
        return object.storageView;
    },

    // Makes `id` a view of levels and layers of the texture or view `of`, which counts them from its own first ones.
    prGpuWebgpuTextureCreateView__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuNames", "$prGpuWebgpuSampledView"],
    prGpuWebgpuTextureCreateView: function(device, id, of, kind, format, firstLevel, levels, firstLayer, layers) {
        const d = prGpuWebgpuDevices[device];
        const source = d.objects[of];
        const object = {texture: source.texture, owned: false, format: prGpuWebgpuNames.formats[format], kind,
            attachments: new Map(),
            serial: ++d.serial,
            baseLevel: source.baseLevel + firstLevel, baseLayer: kind === 4 ? 0 : source.baseLayer + firstLayer,
            levels, layers: kind === 4 ? 1 : layers};
        object.view = prGpuWebgpuSampledView(object, object.baseLevel, levels, object.baseLayer, object.layers);
        d.objects[id] = object;
        return true;
    },

    prGpuWebgpuTextureWrite__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuTextureWrite: function(device, id, level, x, y, z, width, height, depth, data, rowBytes, rows, size) {
        const d = prGpuWebgpuDevices[device];
        const object = d.objects[id];
        d.gpu.queue.writeTexture({texture: object.texture, mipLevel: (object.baseLevel || 0) + level,
            origin: {x, y, z: z + (object.baseLayer || 0)}},
            HEAPU8.subarray(data, data + Number(size)), {bytesPerRow: rowBytes, rowsPerImage: rows},
            [width, height, depth]);
    },

    prGpuWebgpuSamplerCreate__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuNames"],
    prGpuWebgpuSamplerCreate: function(device, id, words) {
        const d = prGpuWebgpuDevices[device];
        const names = prGpuWebgpuNames;
        const w = words >>> 2;
        const linear = HEAPU32[w] === 1 && HEAPU32[w + 1] === 1 && HEAPU32[w + 2] === 1;
        const desc = {
            magFilter: names.filters[HEAPU32[w]],
            minFilter: names.filters[HEAPU32[w + 1]],
            mipmapFilter: names.filters[HEAPU32[w + 2]],
            addressModeU: names.addresses[HEAPU32[w + 3]],
            addressModeV: names.addresses[HEAPU32[w + 4]],
            addressModeW: names.addresses[HEAPU32[w + 5]],
            maxAnisotropy: linear ? Math.max(1, HEAPU32[w + 6]) : 1,
            lodMinClamp: HEAPF32[w + 8],
            lodMaxClamp: Math.min(HEAPF32[w + 9], 32),
        };
        if (HEAPU32[w + 7] !== 0)
            desc.compare = names.compares[HEAPU32[w + 7]];
        try {
            d.samplers[id] = d.gpu.createSampler(desc);
        } catch (error) {
            return false;
        }
        return true;
    },

    // Returns the bindings that WGSL declares, as {group, binding, entry} for a bind group layout. Returns null if the
    // WGSL reaches resources by handle, through binding arrays at group 2.
    //
    // A texture of floats that the WGSL only reads by texel is unfilterable, so that a texture of depth binds to it.
    $prGpuWebgpuBindings__deps: ["$prGpuWebgpuViewDimension"],
    $prGpuWebgpuBindings: function(wgsl, visibility) {
        const bindings = [];
        const declaration = /@binding\((\d+)\)\s*@group\((\d+)\)\s*var(?:<([^>]*)>)?\s+(\w+)\s*:\s*([^;]+);/g;
        const unfiltered = name => {
            const uses = wgsl.match(new RegExp("\\b" + name + "\\b", "g")).length - 1;
            const reads = wgsl.match(new RegExp("\\b(?:textureLoad|textureDimensions|textureNumLevels|textureNumLayers)" +
                "\\s*\\(\\s*\\(?\\s*" + name + "\\b", "g"));
            return uses === (reads ? reads.length : 0);
        };
        for (const match of wgsl.matchAll(declaration)) {
            const binding = Number(match[1]);
            const group = Number(match[2]);
            const space = (match[3] || "").replace(/\s/g, "");
            const name = match[4];
            const type = match[5].trim();
            if (group > 1)
                return null;
            const entry = {binding, visibility};
            if (space === "uniform") {
                entry.buffer = {type: "uniform", hasDynamicOffset: binding === 0};
            } else if (space.startsWith("storage")) {
                entry.buffer = {type: space.endsWith("read_write") ? "storage" : "read-only-storage"};
            } else if (type.startsWith("sampler_comparison")) {
                entry.sampler = {type: "comparison"};
            } else if (type.startsWith("sampler")) {
                entry.sampler = {type: "filtering"};
            } else if (type.startsWith("texture_storage")) {
                const args = type.slice(type.indexOf("<") + 1, type.lastIndexOf(">")).split(",").map(s => s.trim());
                const access = {read: "read-only", write: "write-only", read_write: "read-write"}[args[1]];
                entry.storageTexture = {access, format: args[0], viewDimension: prGpuWebgpuViewDimension(type)};
            } else if (type.startsWith("texture")) {
                const multisampled = type.includes("multisampled");
                let sample = type.includes("<u32>") ? "uint" : type.includes("<i32>") ? "sint" :
                    type.startsWith("texture_depth") ? "depth" : "float";
                if (sample === "float" && (multisampled || unfiltered(name)))
                    sample = "unfilterable-float";
                entry.texture = {sampleType: sample, viewDimension: prGpuWebgpuViewDimension(type), multisampled};
            }
            bindings.push({group, binding, entry});
        }
        return bindings;
    },

    $prGpuWebgpuViewDimension: function(type) {
        if (type.includes("cube_array"))
            return "cube-array";
        if (type.includes("cube"))
            return "cube";
        if (type.includes("2d_array"))
            return "2d-array";
        if (type.includes("3d"))
            return "3d";
        return "2d";
    },

    // Returns the shader module of WGSL, made on first use, with the formats that `formats` holds by binding in place
    // of those that its storage textures declare, and its bindings.
    $prGpuWebgpuModule__deps: ["$prGpuWebgpuBindings"],
    $prGpuWebgpuModule: function(d, wgsl, visibility, formats) {
        let key = wgsl;
        for (const [binding, format] of formats) {
            key += "\n" + binding + ":" + format;
            const declaration = new RegExp("(@binding\\(" + binding +
                "\\)\\s*@group\\(1\\)\\s*var\\s+\\w+\\s*:\\s*texture_storage_\\w+<\\s*)\\w+");
            wgsl = wgsl.replace(declaration, "$1" + format);
        }
        let module = d.modules.get(key);
        if (!module) {
            module = {module: d.gpu.createShaderModule({code: wgsl}), bindings: new Map()};
            d.modules.set(key, module);
        }
        let bindings = module.bindings.get(visibility);
        if (bindings === undefined) {
            bindings = prGpuWebgpuBindings(wgsl, visibility);
            module.bindings.set(visibility, bindings);
        }
        return {module: module.module, bindings};
    },

    // Makes the pipeline's layout from the bindings of its shaders, two groups that the backend binds itself. A texture
    // that any stage filters is filterable.
    $prGpuWebgpuLayout: function(d, stages) {
        const groups = [new Map(), new Map()];
        for (const bindings of stages) {
            for (const {group, binding, entry} of bindings) {
                const held = groups[group].get(binding);
                if (!held) {
                    groups[group].set(binding, JSON.parse(JSON.stringify(entry)));
                    continue;
                }
                held.visibility |= entry.visibility;
                if (held.texture && entry.texture && entry.texture.sampleType === "float")
                    held.texture.sampleType = "float";
            }
        }
        const sorted = groups.map(group => [...group.values()].sort((a, b) => a.binding - b.binding));
        const layouts = sorted.map(entries => d.gpu.createBindGroupLayout({entries}));
        return {
            layout: d.gpu.createPipelineLayout({bindGroupLayouts: layouts}),
            layouts,
            bindings: sorted,
            groups: [new Map(), new Map()],
            // The storage textures that the shaders only write, which take any format that stores.
            written: sorted[1].filter(e => e.storageTexture && e.storageTexture.access === "write-only")
                .map(e => ({binding: e.binding, format: e.storageTexture.format})),
        };
    },

    // Makes a pipeline of `spec` (prGpuWebgpuPipelineCreate), with the formats that `formats` holds by binding in place
    // of those that its shaders' storage textures declare.
    $prGpuWebgpuPipelineMake__deps: ["$prGpuWebgpuModule", "$prGpuWebgpuLayout"],
    $prGpuWebgpuPipelineMake: function(d, spec, formats) {
        const stages = spec.stages.map(s => Object.assign(prGpuWebgpuModule(d, s.wgsl, s.visibility, formats),
            {name: s.name}));
        const layout = prGpuWebgpuLayout(d, stages.map(s => s.bindings));
        if (spec.compute) {
            const pipeline = d.gpu.createComputePipeline({
                layout: layout.layout, compute: {module: stages[0].module, entryPoint: stages[0].name},
            });
            return Object.assign(layout, {pipeline, compute: true, spec, variants: new Map()});
        }
        const desc = Object.assign({}, spec.desc, {
            layout: layout.layout,
            vertex: Object.assign({}, spec.desc.vertex, {module: stages[0].module, entryPoint: stages[0].name}),
        });
        if (stages.length > 1) {
            desc.fragment = {module: stages[1].module, entryPoint: stages[1].name, targets: spec.targets};
        } else if (spec.targets.length !== 0) {
            // A pipeline without a fragment shader writes only depth and stencil, in a pass that may have colors too.
            if (!d.noColor)
                d.noColor = d.gpu.createShaderModule({code: "@fragment fn main() {}"});
            desc.fragment = {module: d.noColor, entryPoint: "main",
                targets: spec.targets.map(t => ({format: t.format, writeMask: 0}))};
        }
        const pipeline = d.gpu.createRenderPipeline(desc);
        return Object.assign(layout, {pipeline, compute: false, spec, variants: new Map()});
    },

    // Returns the pipeline that a draw or dispatch of `state` runs: its pipeline, or a variant of it whose storage
    // textures take the formats of the textures bound to them.
    $prGpuWebgpuPipelineFor__deps: ["$prGpuWebgpuPipelineMake"],
    $prGpuWebgpuPipelineFor: function(d, state) {
        const p = state.pipeline;
        const formats = new Map();
        let key = "";
        for (const {binding, format} of p.written) {
            const slot = state.rwTextures[binding - 26];
            const object = slot && d.objects[slot.texture];
            if (object && object.format !== format) {
                formats.set(binding, object.format);
                key += binding + ":" + object.format + ",";
            }
        }
        if (key === "")
            return p;
        let variant = p.variants.get(key);
        if (!variant) {
            variant = prGpuWebgpuPipelineMake(d, p.spec, formats);
            p.variants.set(key, variant);
        }
        return variant;
    },

    // Makes a pipeline from the words of gpu_webgpu.cpp's pipelineCreate. Returns false for shaders that reach
    // resources by handle.
    prGpuWebgpuPipelineCreate__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuNames", "$prGpuWebgpuBindings",
        "$prGpuWebgpuPipelineMake", "$UTF8ToString"],
    prGpuWebgpuPipelineCreate: function(device, id, words, count) {
        const d = prGpuWebgpuDevices[device];
        const n = prGpuWebgpuNames;
        let at = words >>> 2;
        const word = () => HEAPU32[at++];
        const float = () => HEAPF32[at++];
        const compute = word() === 1;
        // An entry point's WGSL, kept by the pipeline, since its library may be destroyed first.
        const stage = visibility => {
            const code = word();
            const size = word();
            const name = UTF8ToString(word(), word());
            if (code === 0)
                return null;
            let wgsl = d.sources.get(code);
            if (wgsl === undefined) {
                wgsl = UTF8ToString(code, size);
                d.sources.set(code, wgsl);
            }
            return {wgsl, name, visibility};
        };
        try {
            const vertex = stage(compute ? GPUShaderStage.COMPUTE : GPUShaderStage.VERTEX);
            const fragment = compute ? null : stage(GPUShaderStage.FRAGMENT);
            const stages = fragment ? [vertex, fragment] : [vertex];
            if (stages.some(s => !prGpuWebgpuBindings(s.wgsl, s.visibility)))
                return false;
            if (compute) {
                d.pipelines[id] = prGpuWebgpuPipelineMake(d, {compute: true, stages}, new Map());
                return true;
            }
            const topology = word();
            const cull = word();
            const frontClockwise = word() !== 0;
            const depthTest = word() !== 0;
            const depthWrite = word() !== 0;
            const depthCompare = word();
            const depthBias = float();
            const depthBiasSlope = float();
            const depthBiasClamp = float();
            const stencilTest = word() !== 0;
            const stencilReadMask = word();
            const stencilWriteMask = word();
            const face = () => {
                const fail = word();
                const depthFail = word();
                const pass = word();
                const compare = word();
                return stencilTest ? {failOp: n.stencilOps[fail], depthFailOp: n.stencilOps[depthFail],
                    passOp: n.stencilOps[pass], compare: n.compares[compare]} : {};
            };
            const stencilFront = face();
            const stencilBack = face();
            const targets = [];
            const colorCount = word();
            for (let i = 0; i < colorCount; ++i) {
                const format = n.formats[word()];
                const enabled = word() !== 0;
                const colorSource = word();
                const colorTarget = word();
                const colorOp = word();
                const alphaSource = word();
                const alphaTarget = word();
                const alphaOp = word();
                const writeMask = word();
                const target = {format, writeMask};
                if (enabled) {
                    target.blend = {
                        color: {srcFactor: n.blendFactors[colorSource], dstFactor: n.blendFactors[colorTarget],
                            operation: n.blendOps[colorOp]},
                        alpha: {srcFactor: n.blendFactors[alphaSource], dstFactor: n.blendFactors[alphaTarget],
                            operation: n.blendOps[alphaOp]},
                    };
                }
                targets.push(target);
            }
            const depthFormat = word();
            const samples = word();
            const buffers = [];
            const bufferCount = word();
            for (let i = 0; i < bufferCount; ++i) {
                const arrayStride = word();
                const stepMode = word() === 1 ? "instance" : "vertex";
                buffers.push({arrayStride, stepMode, attributes: []});
            }
            const attributeCount = word();
            for (let i = 0; i < attributeCount; ++i) {
                const format = n.vertexFormats[word()];
                const buffer = word();
                const offset = word();
                buffers[buffer].attributes.push({shaderLocation: i, format, offset});
            }
            const desc = {
                vertex: {buffers},
                primitive: {
                    topology: n.topologies[topology],
                    cullMode: n.culls[cull],
                    frontFace: frontClockwise ? "cw" : "ccw",
                },
                multisample: {count: samples},
            };
            if (topology === 1 || topology === 3)
                desc.primitive.stripIndexFormat = "uint32";
            if (depthFormat !== 0) {
                desc.depthStencil = {
                    format: n.formats[depthFormat],
                    depthWriteEnabled: depthWrite,
                    depthCompare: depthTest ? n.compares[depthCompare] : "always",
                    stencilFront, stencilBack, stencilReadMask, stencilWriteMask,
                    depthBias, depthBiasSlopeScale: depthBiasSlope, depthBiasClamp,
                };
            }
            d.pipelines[id] = prGpuWebgpuPipelineMake(d, {compute: false, stages, desc, targets}, new Map());
            return true;
        } catch (error) {
            console.error("lib/gpu: WebGPU made no pipeline: " + error.message);
            return false;
        }
    },

    prGpuWebgpuPipelineRelease__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuPipelineRelease: function(device, id) {
        prGpuWebgpuDevices[device].pipelines[id] = null;
    },

    // Returns the view of one level and layer of a texture that a pass draws into, made on first use.
    $prGpuWebgpuAttachmentView: function(object, level, layer) {
        if (object.canvas)
            return object.view;
        const key = level * 65536 + layer;
        let view = object.attachments.get(key);
        if (!view) {
            view = object.texture.createView({
                format: object.format, dimension: "2d", baseMipLevel: (object.baseLevel || 0) + level, mipLevelCount: 1,
                baseArrayLayer: (object.baseLayer || 0) + layer, arrayLayerCount: 1,
            });
            object.attachments.set(key, view);
        }
        return view;
    },

    // Sets the pipeline that a draw or dispatch runs on `encoder` (prGpuWebgpuPipelineFor), and the bind groups of its
    // bindings from the command buffer's root data and slots. A pipeline keeps the bind groups that it made, by the
    // objects that they hold, until it has made too many. A Texture slot that the source reads only by texel may have
    // no sampler, and its sampler binding takes one of the device's own.
    $prGpuWebgpuBind__deps: ["$prGpuWebgpuStorageView", "$prGpuWebgpuPipelineFor"],
    $prGpuWebgpuBind: function(d, state, encoder) {
        const p = prGpuWebgpuPipelineFor(d, state);
        if (encoder.prPipeline !== p) {
            encoder.setPipeline(p.pipeline);
            encoder.prPipeline = p;
        }
        for (let group = 0; group < 2; ++group) {
            const bindings = p.bindings[group];
            const entries = [];
            const offsets = [];
            let key = "";
            for (const {binding} of bindings) {
                let resource;
                if (group === 0 || binding === 0) {
                    const root = group === 0 ? state.push : state.uniforms;
                    const buffer = d.objects[root.buffer];
                    resource = {buffer: buffer.buffer, size: root.size};
                    offsets.push(root.offset);
                    key += buffer.serial + ":" + root.size + ",";
                } else if (binding >= 2 && binding < 10) {
                    const slot = binding < 6 ? state.buffers[binding - 2] : state.rwBuffers[binding - 6];
                    const buffer = d.objects[slot.buffer];
                    resource = {buffer: buffer.buffer, offset: slot.offset};
                    key += buffer.serial + "@" + slot.offset + ",";
                } else if (binding >= 10 && binding < 26) {
                    const slot = state.textures[(binding - 10) >> 1];
                    if ((binding & 1) === 0) {
                        const texture = d.objects[slot.texture];
                        resource = texture.view;
                        key += "t" + texture.serial + ",";
                    } else {
                        if (!d.samplers[0])
                            d.samplers[0] = d.gpu.createSampler();
                        resource = d.samplers[slot.sampler];
                        key += "s" + slot.sampler + ",";
                    }
                } else {
                    const texture = d.objects[state.rwTextures[binding - 26].texture];
                    resource = prGpuWebgpuStorageView(texture);
                    key += "w" + texture.serial + ",";
                }
                entries.push({binding, resource});
            }
            let bindGroup = p.groups[group].get(key);
            if (!bindGroup) {
                if (p.groups[group].size > 256)
                    p.groups[group].clear();
                bindGroup = d.gpu.createBindGroup({layout: p.layouts[group], entries});
                p.groups[group].set(key, bindGroup);
            }
            encoder.setBindGroup(group, bindGroup, offsets);
        }
    },

    // Encodes and submits a command buffer's words, after the used parts of its stream blocks and every upload buffer
    // reach the GPU. Once the work is done and the readback buffers that it wrote are copied back, tells the device
    // that it reached `fence`.
    prGpuWebgpuSubmit__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuAttachmentView", "$prGpuWebgpuBind",
        "prGpuWebgpuReached", "prGpuWebgpuError"],
    prGpuWebgpuSubmit: function(device, words, count, streams, streamCount, fence) {
        const d = prGpuWebgpuDevices[device];
        const queue = d.gpu.queue;
        for (let i = 0; i < streamCount; ++i) {
            const block = d.objects[HEAPU32[(streams >>> 2) + 2 * i]];
            const used = (HEAPU32[(streams >>> 2) + 2 * i + 1] + 3) & ~3;
            if (used !== 0)
                queue.writeBuffer(block.buffer, 0, HEAPU8, block.shadow, used);
        }
        for (const upload of d.uploads.values())
            queue.writeBuffer(upload.buffer, 0, HEAPU8, upload.shadow, upload.size);
        const encoder = d.gpu.createCommandEncoder();
        const state = {
            pipeline: null,
            push: {buffer: 0, offset: 0, size: 128},
            uniforms: {buffer: 0, offset: 0, size: 0},
            buffers: [],
            rwBuffers: [],
            textures: [],
            rwTextures: [],
        };
        const readbacks = new Set();
        // The timestamps of the start of the first pass and the end of the last, from a clock that the device keeps
        // until the work is done.
        let clock = null;
        if (d.timestamps) {
            clock = d.clocks.pop() || {
                queries: d.gpu.createQuerySet({type: "timestamp", count: 2}),
                resolved: d.gpu.createBuffer({size: 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC}),
                read: d.gpu.createBuffer({size: 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST}),
            };
            clock.started = false;
        }
        const timestampWrites = () => {
            if (!clock)
                return undefined;
            const writes = {querySet: clock.queries, endOfPassWriteIndex: 1};
            if (!clock.started)
                writes.beginningOfPassWriteIndex = 0;
            clock.started = true;
            return writes;
        };
        let pass = null;
        let computePass = null;
        const endCompute = () => {
            if (computePass) {
                computePass.end();
                computePass = null;
            }
        };
        const readback = id => {
            if (d.objects[id].memory === 2)
                readbacks.add(id);
        };
        let at = words >>> 2;
        const end = at + count;
        const word = () => HEAPU32[at++];
        const signed = () => HEAP32[at++];
        const float = () => HEAPF32[at++];
        const wide = () => {
            const low = HEAPU32[at++];
            const high = HEAPU32[at++];
            return high * 4294967296 + low;
        };
        const region = () => ({level: word(), layer: word(), x: word(), y: word(), z: word(), width: word(),
            height: word(), depth: word()});
        // Copies between a buffer and a texture's region, through `copy`, which takes a part of the region, its layout
        // in the buffer and its extent. WebGPU lays out a copy's rows a multiple of 256 bytes apart, so rows that lie
        // otherwise are copied one at a time: a row of the texture's blocks, `blockHeight` texels high.
        const copyRows = (r, offset, bytesPerRow, rowsPerImage, blockHeight, copy) => {
            if (bytesPerRow % 256 === 0) {
                copy(r, {offset, bytesPerRow, rowsPerImage}, [r.width, r.height, r.depth]);
                return;
            }
            for (let layer = 0; layer < r.depth; ++layer) {
                for (let row = 0; row * blockHeight < r.height; ++row) {
                    const part = Object.assign({}, r,
                        {y: r.y + row * blockHeight, layer: r.layer + layer, height: blockHeight, depth: 1});
                    copy(part, {offset: offset + (layer * rowsPerImage + row) * bytesPerRow},
                        [r.width, blockHeight, 1]);
                }
            }
        };
        // A view's levels and layers start at its first ones in its texture.
        const copyOf = (texture, r) => {
            const object = d.objects[texture];
            return {texture: object.texture, mipLevel: (object.baseLevel || 0) + r.level,
                origin: {x: r.x, y: r.y, z: r.z + r.layer + (object.baseLayer || 0)}};
        };
        // A command that fails to decode, as one of an object that is gone, ends the decoding and counts as an error
        // of the page's, so that the work and its fence still complete.
        try {
            while (at < end) {
                switch (word()) {
                    case 1: {  // PassBegin
                        endCompute();
                        const colorAttachments = [];
                        const colors = word();
                        for (let i = 0; i < colors; ++i) {
                            const texture = word();
                            const resolve = word();
                            const level = word();
                            const layer = word();
                            const load = word();
                            const store = word();
                            const clearValue = {r: float(), g: float(), b: float(), a: float()};
                            const attachment = {
                                view: prGpuWebgpuAttachmentView(d.objects[texture], level, layer),
                                loadOp: load === 0 ? "load" : "clear",
                                // PrGpuStore_Keep and PrGpuStore_ResolveKeep keep the attachment's own samples.
                                storeOp: store === 0 || store === 3 ? "store" : "discard",
                                clearValue,
                            };
                            if (resolve !== 0)
                                attachment.resolveTarget = prGpuWebgpuAttachmentView(d.objects[resolve], level, layer);
                            colorAttachments.push(attachment);
                        }
                        const depthTexture = word();
                        word();
                        const depthLevel = word() || 0;
                        const depthLayer = word();
                        const depthLoad = word();
                        const depthStore = word();
                        const stencilLoad = word();
                        const stencilStore = word();
                        const clearDepth = float();
                        const clearStencil = word();
                        const desc = {colorAttachments};
                        if (depthTexture !== 0) {
                            const object = d.objects[depthTexture];
                            const attachment = {
                                view: prGpuWebgpuAttachmentView(object, depthLevel, depthLayer),
                                depthLoadOp: depthLoad === 0 ? "load" : "clear",
                                depthStoreOp: depthStore === 0 ? "store" : "discard",
                                depthClearValue: clearDepth,
                            };
                            if (object.format.includes("stencil")) {
                                attachment.stencilLoadOp = stencilLoad === 0 ? "load" : "clear";
                                attachment.stencilStoreOp = stencilStore === 0 ? "store" : "discard";
                                attachment.stencilClearValue = clearStencil;
                            }
                            desc.depthStencilAttachment = attachment;
                        }
                        desc.timestampWrites = timestampWrites();
                        pass = encoder.beginRenderPass(desc);
                        break;
                    }
                    case 2:  // PassEnd
                        pass.end();
                        pass = null;
                        break;
                    case 3:  // Pipeline
                        state.pipeline = d.pipelines[word()];
                        break;
                    case 4:  // Push
                        state.push = {buffer: word(), offset: word(), size: 128};
                        break;
                    case 5:  // Uniforms
                        state.uniforms = {buffer: word(), offset: word(), size: word()};
                        break;
                    case 6: {  // Buffer
                        const slot = word();
                        state.buffers[slot] = {buffer: word(), offset: word()};
                        break;
                    }
                    case 7: {  // RwBuffer
                        const slot = word();
                        state.rwBuffers[slot] = {buffer: word(), offset: word()};
                        break;
                    }
                    case 8: {  // Texture
                        const slot = word();
                        state.textures[slot] = {texture: word(), sampler: word()};
                        break;
                    }
                    case 9: {  // RwTexture
                        const slot = word();
                        state.rwTextures[slot] = {texture: word()};
                        break;
                    }
                    case 10: {  // VertexBuffer
                        const slot = word();
                        const buffer = word();
                        pass.setVertexBuffer(slot, d.objects[buffer].buffer, word());
                        break;
                    }
                    case 11: {  // IndexBuffer
                        const buffer = word();
                        const offset = word();
                        pass.setIndexBuffer(d.objects[buffer].buffer, word() ? "uint32" : "uint16", offset);
                        break;
                    }
                    case 12:  // Viewport
                        pass.setViewport(float(), float(), float(), float(), float(), float());
                        break;
                    case 13:  // Scissor
                        pass.setScissorRect(word(), word(), word(), word());
                        break;
                    case 14:  // BlendConstant
                        pass.setBlendConstant({r: float(), g: float(), b: float(), a: float()});
                        break;
                    case 15:  // StencilReference
                        pass.setStencilReference(word());
                        break;
                    case 16:  // Draw
                        prGpuWebgpuBind(d, state, pass);
                        pass.draw(word(), word(), word(), word());
                        break;
                    case 17:  // DrawIndexed
                        prGpuWebgpuBind(d, state, pass);
                        pass.drawIndexed(word(), word(), word(), signed(), word());
                        break;
                    case 18:  // DrawIndirect
                    case 19: {  // DrawIndexedIndirect
                        const indexed = HEAPU32[at - 1] === 19;
                        const buffer = d.objects[word()].buffer;
                        const offset = word();
                        const draws = word();
                        const stride = word();
                        prGpuWebgpuBind(d, state, pass);
                        for (let i = 0; i < draws; ++i) {
                            if (indexed)
                                pass.drawIndexedIndirect(buffer, offset + i * stride);
                            else
                                pass.drawIndirect(buffer, offset + i * stride);
                        }
                        break;
                    }
                    case 20:  // Dispatch
                    case 21: {  // DispatchIndirect
                        const indirect = HEAPU32[at - 1] === 21;
                        if (!computePass)
                            computePass = encoder.beginComputePass({timestampWrites: timestampWrites()});
                        prGpuWebgpuBind(d, state, computePass);
                        if (indirect)
                            computePass.dispatchWorkgroupsIndirect(d.objects[word()].buffer, word());
                        else
                            computePass.dispatchWorkgroups(word(), word(), word());
                        break;
                    }
                    case 22: {  // CopyBuffer
                        endCompute();
                        const source = word();
                        const sourceOffset = wide();
                        const target = word();
                        const targetOffset = wide();
                        const size = wide();
                        encoder.copyBufferToBuffer(d.objects[source].buffer, sourceOffset, d.objects[target].buffer,
                            targetOffset, size);
                        readback(target);
                        break;
                    }
                    case 23: {  // CopyBufferToTexture
                        endCompute();
                        const buffer = word();
                        const offset = wide();
                        const bytesPerRow = word();
                        const rowsPerImage = word();
                        const blockHeight = word();
                        const texture = word();
                        const r = region();
                        copyRows(r, offset, bytesPerRow, rowsPerImage, blockHeight, (part, layout, extent) =>
                            encoder.copyBufferToTexture(Object.assign({buffer: d.objects[buffer].buffer}, layout),
                                copyOf(texture, part), extent));
                        break;
                    }
                    case 24: {  // CopyTextureToBuffer
                        endCompute();
                        const texture = word();
                        const r = region();
                        const buffer = word();
                        const offset = wide();
                        const bytesPerRow = word();
                        const rowsPerImage = word();
                        const blockHeight = word();
                        copyRows(r, offset, bytesPerRow, rowsPerImage, blockHeight, (part, layout, extent) =>
                            encoder.copyTextureToBuffer(copyOf(texture, part),
                                Object.assign({buffer: d.objects[buffer].buffer}, layout), extent));
                        readback(buffer);
                        break;
                    }
                    case 25: {  // CopyTexture
                        endCompute();
                        const source = word();
                        const from = region();
                        const target = word();
                        const to = region();
                        encoder.copyTextureToTexture(copyOf(source, from), copyOf(target, to),
                            [from.width, from.height, from.depth]);
                        break;
                    }
                }
            }
        } catch (error) {
            console.error("lib/gpu: WebGPU: " + error.message);
            _prGpuWebgpuError(d.owner);
            endCompute();
            if (pass)
                pass.end();
        }
        endCompute();
        const timed = clock && clock.started;
        if (timed) {
            encoder.resolveQuerySet(clock.queries, 0, 2, clock.resolved, 0);
            encoder.copyBufferToBuffer(clock.resolved, 0, clock.read, 0, 16);
        }
        queue.submit([encoder.finish()]);
        const owner = d.owner;
        const reached = Number(fence);
        const copies = [...readbacks].map(id => d.objects[id]);
        let seconds = 0;
        const timing = () => {
            if (!timed)
                return null;
            return clock.read.mapAsync(GPUMapMode.READ).then(() => {
                const ticks = new BigUint64Array(clock.read.getMappedRange());
                if (ticks[1] > ticks[0] && ticks[0] !== 0n)
                    seconds = Number(ticks[1] - ticks[0]) / 1e9;
                clock.read.unmap();
            });
        };
        // The work's completion and its reads start at once, so that the waits of submissions overlap. A buffer maps
        // once at a time, so the reads of one buffer follow each other.
        const read = object => {
            object.reading = (object.reading || Promise.resolve()).then(() => object.buffer.mapAsync(GPUMapMode.READ))
                .then(() => {
                    HEAPU8.set(new Uint8Array(object.buffer.getMappedRange()), object.shadow);
                    object.buffer.unmap();
                }).catch(() => {});
            return object.reading;
        };
        const done = queue.onSubmittedWorkDone().then(() => Promise.all(copies.map(read).concat([timing()])))
            .catch(() => {});
        // Fences are reached in order: each once its own work and the fences before it are done.
        d.chain = Promise.all([d.chain, done]).then(() => {
            if (clock)
                d.clocks.push(clock);
            if (!d.released)
                _prGpuWebgpuReached(owner, reached, seconds);
        });
    },

    // Configures the page's canvas that `selector` names for the device, in the page's preferred 8-bit format, which it
    // writes as a PrGpuFormat. Returns the canvas's id, or zero if the page has no such canvas.
    prGpuWebgpuCanvasAttach__deps: ["$prGpuWebgpuDevices", "$prGpuWebgpuNames", "$prWebString"],
    prGpuWebgpuCanvasAttach: function(device, selector, selectorSize, width, height, transparent, format) {
        const d = prGpuWebgpuDevices[device];
        const canvas = document.querySelector(prWebString(selector, selectorSize));
        if (canvas === null)
            return 0;
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext("webgpu");
        if (!context)
            return 0;
        const preferred = navigator.gpu.getPreferredCanvasFormat();
        context.configure({
            device: d.gpu,
            format: preferred,
            alphaMode: transparent ? "premultiplied" : "opaque",
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
        });
        // The format's value is its position in the table of names, as WebgpuFormat orders them.
        HEAPU32[format >>> 2] = prGpuWebgpuNames.formats.indexOf(preferred);
        return d.canvases.push({canvas, context, format: preferred}) - 1;
    },

    prGpuWebgpuCanvasRelease__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuCanvasRelease: function(device, canvas) {
        const d = prGpuWebgpuDevices[device];
        if (d)
            d.canvases[canvas] = null;
    },

    // Gives the resource `id` the canvas's texture of this frame, at the frame's extent.
    prGpuWebgpuCanvasAcquire__deps: ["$prGpuWebgpuDevices"],
    prGpuWebgpuCanvasAcquire: function(device, canvas, id, width, height) {
        const d = prGpuWebgpuDevices[device];
        const c = d.canvases[canvas];
        if (c.canvas.width !== width || c.canvas.height !== height) {
            c.canvas.width = width;
            c.canvas.height = height;
        }
        const texture = c.context.getCurrentTexture();
        d.objects[id] = {texture, owned: false, canvas: true, format: c.format, view: texture.createView(),
            attachments: new Map(), serial: ++d.serial};
    },
});

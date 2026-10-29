// Evaluating a Lottie animation's expressions with QuickJS.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// Each expression compiles once into a function of a scope object. The prelude (lottie_expression_prelude.h) fills the
// scope with After Effects' names. The native functions it calls access the animation's tree of nodes.

#include "lottie/src/lottie_expression.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_expression_prelude.h"

#include "quickjs.h"

namespace pr_impl {

// An animation's expression evaluator, and the expressions it has compiled.
struct LottieExpressions {
    JSRuntime* runtime;
    JSContext* ctx;
    JSValue compile;            // converts an expression's source to its function
    JSValue evaluate;           // the evaluator that the prelude returns, or the exception that it threw
    const LottieAnimation* animation;

    // The compiled functions, in compilation order, with undefined for an expression that did not compile. For each
    // property, the index of its function, or kLottieNone if it is not compiled yet.
    Array<JSValue> functions;
    Array<uint32_t> functionOf;

    // A cache of each property's value, because an expression is evaluated once per frame however often it is read. For
    // each property: the frame at which it was last evaluated, the index of its value in `values` or kLottieNone if
    // there is none yet, and whether the expression returned a value.
    Array<double> frameOf;
    Array<uint32_t> valueOf;
    Array<uint8_t> answered;
    Array<float> values;
};

namespace {

// The stack size available to an expression's JavaScript before it is stopped. It is four times as large under
// AddressSanitizer, which makes every stack frame larger.
constexpr size_t kExpressionStackSize = (PR_ADDRESS_SANITIZER ? 4 : 1) * 1024 * 1024;

// The maximum number of floats in a value converted into JavaScript.
constexpr uint32_t kExpressionMaxFloats = 1u << 20;

constexpr JSMallocFunctions kExpressionAllocator = {
    [](void*, size_t count, size_t size) { return prMemoryAllocZeroed(count * size); },
    [](void*, size_t size) { return prMemoryAlloc(size); },
    [](void*, void* memory) { prMemoryFree(memory); },
    [](void*, void* memory, size_t size) { return prMemoryRealloc(memory, size); },
    [](const void* memory) { return prMemoryUsableSize(memory); },
};

// The accessor name for each role. Groups that only the interfaces use are named with "$".
struct RoleName {
    const char* accessor;
    LottieRole role;
};

constexpr RoleName kRoleNames[] = {
    {"transform", LottieRole_Transform},       {"$contents", LottieRole_Contents},
    {"$effects", LottieRole_Effects},          {"$masks", LottieRole_Masks},
    {"$effect", LottieRole_Effect},            {"$mask", LottieRole_Mask},
    {"$shapeGroup", LottieRole_ShapeGroup},    {"anchorPoint", LottieRole_AnchorPoint},
    {"position", LottieRole_Position},         {"xPosition", LottieRole_PositionX},
    {"yPosition", LottieRole_PositionY},       {"zPosition", LottieRole_PositionZ},
    {"scale", LottieRole_Scale},               {"rotation", LottieRole_Rotation},
    {"xRotation", LottieRole_RotationX},       {"yRotation", LottieRole_RotationY},
    {"orientation", LottieRole_Orientation},   {"opacity", LottieRole_Opacity},
    {"skew", LottieRole_Skew},                 {"skewAxis", LottieRole_SkewAxis},
    {"path", LottieRole_Path},                 {"size", LottieRole_Size},
    {"roundness", LottieRole_Roundness},       {"color", LottieRole_Color},
    {"strokeWidth", LottieRole_StrokeWidth},   {"start", LottieRole_Start},
    {"end", LottieRole_End},                   {"offset", LottieRole_Offset},
    {"points", LottieRole_Points},             {"outerRadius", LottieRole_OuterRadius},
    {"innerRadius", LottieRole_InnerRadius},   {"outerRoundness", LottieRole_OuterRoundness},
    {"innerRoundness", LottieRole_InnerRoundness}, {"copies", LottieRole_Copies},
    {"amount", LottieRole_Amount},             {"maskPath", LottieRole_MaskPath},
    {"maskOpacity", LottieRole_MaskOpacity},   {"maskExpansion", LottieRole_MaskExpansion},
    {"timeRemap", LottieRole_TimeRemap},
};

const LottieAnimation& animationOf(JSContext* ctx) {
    return *static_cast<LottieExpressions*>(JS_GetContextOpaque(ctx))->animation;
}

// Returns the node that an argument names. Returns kLottieNone if the argument names no node, or an index past the
// animation's nodes.
uint32_t nodeArg(JSContext* ctx, JSValueConst value) {
    int32_t node = -1;
    JS_ToInt32(ctx, &node, value);
    return node < 0 || uint32_t(node) >= animationOf(ctx).nodes.count ? kLottieNone : uint32_t(node);
}

JSValue nodeValue(JSContext* ctx, uint32_t node) {
    return JS_NewInt32(ctx, node == kLottieNone ? -1 : int32_t(node));
}

const LottieNode* PR_NULLABLE nodeOf(JSContext* ctx, JSValueConst value) {
    uint32_t node = nodeArg(ctx, value);
    return node == kLottieNone ? nullptr : &animationOf(ctx).nodes.data[node];
}

const LottieLayer* PR_NULLABLE layerOf(JSContext* ctx, JSValueConst value) {
    const LottieNode* node = nodeOf(ctx, value);
    return node == nullptr || node->layer == kLottieNone ? nullptr : &animationOf(ctx).layers.data[node->layer];
}

double secondsOf(const LottieAnimation& a, double frames) {
    return a.info.frameRate > 0 ? frames / a.info.frameRate : 0;
}

bool isPath(LottieRole role) {
    return role == LottieRole_Path || role == LottieRole_MaskPath;
}

// Converts floats to JavaScript: a number, an array, or a path, according to the role.
JSValue floatsValue(JSContext* ctx, const float* values, uint32_t dims, LottieRole role) {
    if (dims == 1 && !isPath(role))
        return JS_NewFloat64(ctx, double(values[0]));
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < dims; ++i)
        JS_SetPropertyUint32(ctx, array, i, JS_NewFloat64(ctx, double(values[i])));
    if (!isPath(role))
        return array;
    JSValue path = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, path, "$path", array);
    return path;
}

JSValue stringValue(JSContext* ctx, const LottieAnimation& a, LottieString s) {
    PrStringView text = lottieString(a, s);
    return JS_NewStringLen(ctx, text.data, text.size);
}

// ---------------------------------------------------------------------------------------------------------------------
// Natives

JSValue nativeValue(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    const LottieNode* node = nodeOf(ctx, argv[0]);
    if (node == nullptr || node->property == kLottieNone)
        return JS_UNDEFINED;
    double seconds = 0;
    JS_ToFloat64(ctx, &seconds, argv[1]);
    bool expressed = JS_ToBool(ctx, argv[2]) != 0;
    uint32_t dims = lottieDims(a, node->property);
    if (dims == 0 || dims > kExpressionMaxFloats)
        return JS_UNDEFINED;
    Array<float> values;
    values.reserve(dims);
    double frame = seconds * a.info.frameRate;
    if (expressed)
        lottieEvaluate(a, node->property, frame, values.data);
    else
        lottieEvaluateKeyframes(a, node->property, frame, values.data);
    JSValue result = floatsValue(ctx, values.data, dims, node->role);
    values.reset();
    return result;
}

JSValue nativeKeys(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    uint32_t count = node == nullptr || node->property == kLottieNone
                         ? 0
                         : animationOf(ctx).properties.data[node->property].keyframeCount;
    return JS_NewUint32(ctx, count);
}

JSValue nativeKeyTime(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    const LottieNode* node = nodeOf(ctx, argv[0]);
    uint32_t index = 0;
    JS_ToUint32(ctx, &index, argv[1]);
    if (node == nullptr || node->property == kLottieNone)
        return JS_NewFloat64(ctx, 0);
    const LottieProperty& p = a.properties.data[node->property];
    if (index >= p.keyframeCount)
        return JS_NewFloat64(ctx, 0);
    return JS_NewFloat64(ctx, secondsOf(a, a.keyframes.data[p.keyframe + index].time));
}

JSValue nativeKind(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return JS_NewInt32(ctx, node == nullptr ? -1 : node->kind);
}

JSValue nativeRole(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return JS_NewInt32(ctx, node == nullptr ? -1 : node->role);
}

JSValue nativeName(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return node == nullptr ? JS_NewString(ctx, "") : stringValue(ctx, animationOf(ctx), node->name);
}

JSValue nativeMatchName(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return node == nullptr ? JS_NewString(ctx, "") : stringValue(ctx, animationOf(ctx), node->matchName);
}

JSValue nativeIndex(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return JS_NewInt32(ctx, node == nullptr ? 0 : node->index);
}

JSValue nativeParent(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return nodeValue(ctx, node == nullptr ? kLottieNone : node->parent);
}

JSValue nativeChildren(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    const LottieNode* node = nodeOf(ctx, argv[0]);
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (uint32_t child = node != nullptr ? node->firstChild : kLottieNone; child != kLottieNone;
         child = a.nodes.data[child].nextSibling)
        JS_SetPropertyUint32(ctx, array, i++, nodeValue(ctx, child));
    return array;
}

JSValue nativeComp(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    const LottieNode* node = nodeOf(ctx, argv[0]);
    return nodeValue(ctx, node == nullptr || node->comp == kLottieNone ? kLottieNone : a.comps.data[node->comp].node);
}

JSValue nativeCompSize(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    const LottieNode* node = nodeOf(ctx, argv[0]);
    PrVec2 size{0, 0};
    if (node != nullptr && node->comp != kLottieNone)
        size = PrVec2{a.comps.data[node->comp].width, a.comps.data[node->comp].height};
    const float values[2] = {size.x, size.y};
    return floatsValue(ctx, values, 2, LottieRole_None);
}

JSValue nativeRoot(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const LottieAnimation& a = animationOf(ctx);
    return nodeValue(ctx, a.comps.count > 0 ? a.comps.data[0].node : kLottieNone);
}

JSValue nativeLayerIndex(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieLayer* layer = layerOf(ctx, argv[0]);
    return JS_NewInt32(ctx, layer == nullptr ? 0 : layer->id);
}

JSValue nativeInPoint(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieLayer* layer = layerOf(ctx, argv[0]);
    return JS_NewFloat64(ctx, layer == nullptr ? 0 : secondsOf(animationOf(ctx), layer->inPoint));
}

JSValue nativeOutPoint(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieLayer* layer = layerOf(ctx, argv[0]);
    return JS_NewFloat64(ctx, layer == nullptr ? 0 : secondsOf(animationOf(ctx), layer->outPoint));
}

JSValue nativeStartTime(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieLayer* layer = layerOf(ctx, argv[0]);
    return JS_NewFloat64(ctx, layer == nullptr ? 0 : secondsOf(animationOf(ctx), layer->startTime));
}

// Returns the node of the layer's parent layer, or kLottieNone if the layer has no parent.
uint32_t parentLayerNode(const LottieAnimation& a, uint32_t node) {
    const LottieNode& n = a.nodes.data[node];
    if (n.layer == kLottieNone || n.comp == kLottieNone)
        return kLottieNone;
    uint32_t parent = a.layers.data[n.layer].parent;
    return parent == kLottieNone ? kLottieNone : a.layers.data[a.comps.data[n.comp].layers + parent].node;
}

JSValue nativeLayerParent(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    uint32_t node = nodeArg(ctx, argv[0]);
    return nodeValue(ctx, node == kLottieNone ? node : parentLayerNode(animationOf(ctx), node));
}

// Returns the layer's transform to its composition's space, including the transforms of its parents, for toComp.
JSValue nativeToComp(JSContext* ctx, JSValueConst, int, JSValueConst* argv) {
    const LottieAnimation& a = animationOf(ctx);
    double seconds = 0;
    JS_ToFloat64(ctx, &seconds, argv[1]);
    double frame = seconds * a.info.frameRate;
    PrMatrix4 m = prMatrix4Identity();
    uint32_t node = nodeArg(ctx, argv[0]);
    for (uint32_t n = 0; node != kLottieNone && a.nodes.data[node].layer != kLottieNone && n < a.layers.count; ++n) {
        const LottieLayer& layer = a.layers.data[a.nodes.data[node].layer];
        PrMatrix4 own = lottieTransformMatrix4(a, a.transforms.data[layer.transform], frame, layer.autoOrient);
        m = prMatrix4Concat(&m, &own);
        node = parentLayerNode(a, node);
    }
    PrAffine2 t = prAffine2Identity();
    prAffine2FromMatrix4(&m, &t);
    const float values[6] = {t.a, t.b, t.c, t.d, t.e, t.f};
    return floatsValue(ctx, values, 6, LottieRole_None);
}

JSValue nativeFrameRate(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewFloat64(ctx, animationOf(ctx).info.frameRate);
}

JSValue nativeRoles(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue roles = JS_NewObject(ctx);
    for (const RoleName& r : kRoleNames)
        JS_SetPropertyStr(ctx, roles, r.accessor, JS_NewInt32(ctx, r.role));
    return roles;
}

struct Native {
    const char* name;
    JSCFunction* function;
    int length;
};

constexpr Native kNatives[] = {
    {"value", nativeValue, 3},         {"keys", nativeKeys, 1},           {"keyTime", nativeKeyTime, 2},
    {"kind", nativeKind, 1},           {"role", nativeRole, 1},           {"name", nativeName, 1},
    {"matchName", nativeMatchName, 1}, {"index", nativeIndex, 1},         {"parent", nativeParent, 1},
    {"children", nativeChildren, 1},   {"comp", nativeComp, 1},           {"compSize", nativeCompSize, 1},
    {"root", nativeRoot, 0},           {"layerIndex", nativeLayerIndex, 1}, {"inPoint", nativeInPoint, 1},
    {"outPoint", nativeOutPoint, 1},   {"startTime", nativeStartTime, 1}, {"layerParent", nativeLayerParent, 1},
    {"toComp", nativeToComp, 2},       {"frameRate", nativeFrameRate, 0}, {"roles", nativeRoles, 0},
};

// The wrapper for an expression's function. It runs the source with the scope's names available, and returns the value
// assigned to $bm_rt, the variable through which bodymovin exports the result.
constexpr const char* kCompile =
    "(function (source) {"
    " return new Function('$scope', 'with ($scope) {\\n' + source + '\\n}\\n'"
    " + 'return typeof $bm_rt === \"undefined\" ? undefined : $bm_rt;');"
    "})";

JSValue run(JSContext* ctx, const char* source) {
    PrStringView sourceView = source;
    return JS_Eval(ctx, source, sourceView.size, "<lottie>", JS_EVAL_TYPE_GLOBAL);
}

JSValue takeException(JSContext* ctx) {
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_UNDEFINED;
}

LottieExpressions* create(const LottieAnimation& a) {
    LottieExpressions* e = allocZeroed<LottieExpressions>();
    e->animation = &a;
    e->functionOf.reserve(a.properties.count);
    e->frameOf.reserve(a.properties.count);
    e->valueOf.reserve(a.properties.count);
    e->answered.reserve(a.properties.count);
    for (uint32_t i = 0; i < a.properties.count; ++i) {
        e->functionOf.push(kLottieNone);
        e->frameOf.push(0);
        e->valueOf.push(kLottieNone);
        e->answered.push(0);
    }
    e->runtime = JS_NewRuntime2(&kExpressionAllocator, nullptr);
    JS_SetMaxStackSize(e->runtime, kExpressionStackSize);
    e->ctx = JS_NewContext(e->runtime);
    JS_SetContextOpaque(e->ctx, e);
    JSValue natives = JS_NewObject(e->ctx);
    for (const Native& n : kNatives)
        JS_SetPropertyStr(e->ctx, natives, n.name, JS_NewCFunction(e->ctx, n.function, n.name, n.length));
    JSValue prelude = run(e->ctx, kExpressionPrelude);
    JSValue global = JS_GetGlobalObject(e->ctx);
    JSValue args[2] = {global, natives};
    e->evaluate = JS_IsException(prelude) ? takeException(e->ctx) : JS_Call(e->ctx, prelude, JS_UNDEFINED, 2, args);
    if (JS_IsException(e->evaluate))
        e->evaluate = takeException(e->ctx);
    JS_FreeValue(e->ctx, prelude);
    JS_FreeValue(e->ctx, global);
    JS_FreeValue(e->ctx, natives);
    e->compile = run(e->ctx, kCompile);
    return e;
}

// Returns the property's compiled expression, or undefined if it does not compile.
JSValue functionFor(LottieExpressions& e, uint32_t property) {
    uint32_t& slot = e.functionOf.data[property];
    if (slot != kLottieNone)
        return e.functions.data[slot];
    const LottieAnimation& a = *e.animation;
    const LottieProperty& p = a.properties.data[property];
    JSValue text = JS_NewStringLen(e.ctx, a.strings.data + p.expression, p.expressionSize);
    JSValue function = JS_Call(e.ctx, e.compile, JS_UNDEFINED, 1, &text);
    JS_FreeValue(e.ctx, text);
    if (JS_IsException(function))
        function = takeException(e.ctx);
    slot = e.functions.count;
    e.functions.push(function);
    return function;
}

// Reads the result into `value`: a number, an array or a path of `dims` floats. Returns false for anything else.
bool readResult(JSContext* ctx, JSValueConst result, float* value, uint32_t dims) {
    if (JS_IsNumber(result)) {
        double v = 0;
        JS_ToFloat64(ctx, &v, result);
        value[0] = float(v);
        return true;
    }
    JSValue array = JS_IsObject(result) && !JS_IsArray(result) ? JS_GetPropertyStr(ctx, result, "$path")
                                                               : JS_DupValue(ctx, result);
    if (!JS_IsArray(array)) {
        JS_FreeValue(ctx, array);
        return false;
    }
    uint32_t length = 0;
    JSValue count = JS_GetPropertyStr(ctx, array, "length");
    JS_ToUint32(ctx, &length, count);
    JS_FreeValue(ctx, count);
    for (uint32_t i = 0; i < length && i < dims; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, array, i);
        double v = 0;
        if (JS_ToFloat64(ctx, &v, item) == 0)
            value[i] = float(v);
        JS_FreeValue(ctx, item);
    }
    JS_FreeValue(ctx, array);
    return true;
}

// Evaluates the property's expression into `value`, which holds the keyframe value on entry. Returns false if the
// expression does not evaluate, and then leaves `value` unchanged.
bool evaluate(LottieExpressions& e, uint32_t property, double frame, float* value) {
    const LottieAnimation& a = *e.animation;
    const LottieProperty& p = a.properties.data[property];
    JSValue function = functionFor(e, property);
    if (JS_IsUndefined(function) || JS_IsUndefined(e.evaluate))
        return false;
    JSValue args[4] = {JS_DupValue(e.ctx, function), JS_NewInt32(e.ctx, int32_t(p.node)),
                       JS_NewFloat64(e.ctx, secondsOf(a, frame)),
                       floatsValue(e.ctx, value, p.dims, a.nodes.data[p.node].role)};
    JSValue result = JS_Call(e.ctx, e.evaluate, JS_UNDEFINED, 4, args);
    for (JSValue& arg : args)
        JS_FreeValue(e.ctx, arg);
    bool read = false;
    if (JS_IsException(result))
        takeException(e.ctx);
    else if (!JS_IsUndefined(result))
        read = readResult(e.ctx, result, value, p.dims);
    JS_FreeValue(e.ctx, result);
    return read;
}

} // namespace

bool lottieExpressionEvaluate(const LottieAnimation& a, uint32_t property, double frame, float* value) {
    const LottieProperty& p = a.properties.data[property];
    if (p.expression == kLottieNone || p.node == kLottieNone)
        return false;
    if (a.expressions == nullptr)
        a.expressions = create(a);
    LottieExpressions& e = *a.expressions;
    if (e.valueOf.data[property] != kLottieNone && e.frameOf.data[property] == frame) {
        if (e.answered.data[property] != 0)
            memCopy(value, e.values.data + e.valueOf.data[property], p.dims * sizeof(float));
        return e.answered.data[property] != 0;
    }
    bool read = evaluate(e, property, frame, value);
    if (e.valueOf.data[property] == kLottieNone) {
        e.valueOf.data[property] = e.values.count;
        for (uint32_t i = 0; i < p.dims; ++i)
            e.values.push(0);
    }
    e.frameOf.data[property] = frame;
    e.answered.data[property] = read ? 1 : 0;
    if (read)
        memCopy(e.values.data + e.valueOf.data[property], value, p.dims * sizeof(float));
    return read;
}

void lottieExpressionsDestroy(LottieExpressions* PR_NULLABLE expressions) {
    if (expressions == nullptr)
        return;
    LottieExpressions& e = *expressions;
    for (uint32_t i = 0; i < e.functions.count; ++i)
        JS_FreeValue(e.ctx, e.functions.data[i]);
    e.functions.reset();
    e.functionOf.reset();
    e.frameOf.reset();
    e.valueOf.reset();
    e.answered.reset();
    e.values.reset();
    JS_FreeValue(e.ctx, e.compile);
    JS_FreeValue(e.ctx, e.evaluate);
    JS_FreeContext(e.ctx);
    JS_FreeRuntime(e.runtime);
    prMemoryFree(expressions);
}

} // namespace pr_impl

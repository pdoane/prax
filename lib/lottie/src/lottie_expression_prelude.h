// The JavaScript that the expression evaluator runs once. It defines After Effects' globals, and the interfaces through
// which expressions reach the animation's tree. The interfaces are built on the native functions in `$n`
// (lottie_expression.cpp).

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A function of the global object and the native functions. It defines the globals and returns the function that
// evaluates an expression's compiled function: $evaluate(fn, node, seconds, value).
constexpr const char* kExpressionPrelude = R"JS(
(function (g, $n) {
  var degToRads = Math.PI / 180;
  function isArray(a) {
    return a !== null && a !== undefined && (a.constructor === Array || a.constructor === Float32Array);
  }
  function numerable(v) {
    var t = typeof v;
    return t === 'number' || v instanceof Number || t === 'boolean' || t === 'string';
  }
  function plain(v) {
    return v instanceof Number ? v.valueOf() : v;
  }
  g.$bm_neg = function (a) {
    if (numerable(a)) return -a;
    if (isArray(a)) return Array.prototype.map.call(a, function (x) { return -x; });
    return -a;
  };
  function sum(a, b) {
    if ((numerable(a) && numerable(b)) || typeof a === 'string' || typeof b === 'string') return plain(a) + plain(b);
    if (isArray(a) && numerable(b)) { a = Array.prototype.slice.call(a); a[0] += plain(b); return a; }
    if (numerable(a) && isArray(b)) { b = Array.prototype.slice.call(b); b[0] = plain(a) + b[0]; return b; }
    if (isArray(a) && isArray(b)) {
      var out = [];
      for (var i = 0; i < a.length || i < b.length; i += 1) {
        if (typeof a[i] === 'number' && typeof b[i] === 'number') out[i] = a[i] + b[i];
        else out[i] = b[i] === undefined ? a[i] : a[i] || b[i];
      }
      return out;
    }
    return 0;
  }
  function sub(a, b) {
    if (numerable(a) && numerable(b)) {
      if (typeof a === 'string') a = parseInt(a, 10);
      if (typeof b === 'string') b = parseInt(b, 10);
      return plain(a) - plain(b);
    }
    if (isArray(a) && numerable(b)) { a = Array.prototype.slice.call(a); a[0] -= plain(b); return a; }
    if (numerable(a) && isArray(b)) { b = Array.prototype.slice.call(b); b[0] = plain(a) - b[0]; return b; }
    if (isArray(a) && isArray(b)) {
      var out = [];
      for (var i = 0; i < a.length || i < b.length; i += 1) {
        if (typeof a[i] === 'number' && typeof b[i] === 'number') out[i] = a[i] - b[i];
        else out[i] = b[i] === undefined ? a[i] : a[i] || b[i];
      }
      return out;
    }
    return 0;
  }
  function scaleBy(a, b, f) {
    if (numerable(a) && numerable(b)) return f(plain(a), plain(b));
    if (isArray(a) && numerable(b)) return Array.prototype.map.call(a, function (x) { return f(x, plain(b)); });
    if (numerable(a) && isArray(b)) return Array.prototype.map.call(b, function (x) { return f(plain(a), x); });
    return 0;
  }
  function mul(a, b) { return scaleBy(a, b, function (x, y) { return x * y; }); }
  function div(a, b) { return scaleBy(a, b, function (x, y) { return x / y; }); }
  function mod(a, b) {
    if (typeof a === 'string') a = parseInt(a, 10);
    if (typeof b === 'string') b = parseInt(b, 10);
    return plain(a) % plain(b);
  }
  g.sum = g.add = g.$bm_sum = sum;
  g.sub = g.$bm_sub = sub;
  g.mul = g.$bm_mul = mul;
  g.div = g.$bm_div = div;
  g.mod = g.$bm_mod = mod;
  g.clamp = function (num, min, max) {
    if (min > max) { var m = max; max = min; min = m; }
    return Math.min(Math.max(num, min), max);
  };
  g.radiansToDegrees = g.radians_to_degrees = function (v) { return v / degToRads; };
  g.degreesToRadians = function (v) { return v * degToRads; };
  g.degrees_to_radians = g.radiansToDegrees;
  function length(a, b) {
    if (numerable(a)) return Math.abs(plain(a) - (b || 0));
    b = b || [0, 0, 0, 0, 0, 0];
    var total = 0;
    for (var i = 0; i < Math.min(a.length, b.length); i += 1) total += Math.pow(b[i] - a[i], 2);
    return Math.sqrt(total);
  }
  g.length = length;
  g.normalize = function (v) { return div(v, length(v)); };
  g.dot = function (a, b) {
    var s = 0;
    for (var i = 0; i < Math.min(a.length, b.length); i += 1) s += a[i] * b[i];
    return s;
  };
  g.cross = function (a, b) {
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  };
  g.rgbToHsl = function (v) {
    var r = v[0], gr = v[1], b = v[2];
    var max = Math.max(r, gr, b), min = Math.min(r, gr, b);
    var h = 0, s = 0, l = (max + min) / 2;
    if (max !== min) {
      var d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      if (max === r) h = (gr - b) / d + (gr < b ? 6 : 0);
      else if (max === gr) h = (b - r) / d + 2;
      else h = (r - gr) / d + 4;
      h /= 6;
    }
    return [h, s, l, v[3]];
  };
  function hue(p, q, t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1 / 6) return p + (q - p) * 6 * t;
    if (t < 1 / 2) return q;
    if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
    return p;
  }
  g.hslToRgb = function (v) {
    var h = v[0], s = v[1], l = v[2];
    if (s === 0) return [l, l, l, v[3]];
    var q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    var p = 2 * l - q;
    return [hue(p, q, h + 1 / 3), hue(p, q, h), hue(p, q, h - 1 / 3), v[3]];
  };
  g.linear = function (t, tMin, tMax, v1, v2) {
    if (v1 === undefined || v2 === undefined) { v1 = tMin; v2 = tMax; tMin = 0; tMax = 1; }
    if (tMax < tMin) { var m = tMax; tMax = tMin; tMin = m; }
    if (t <= tMin) return v1;
    if (t >= tMax) return v2;
    var perc = tMax === tMin ? 0 : (t - tMin) / (tMax - tMin);
    if (!v1.length) return plain(v1) + (plain(v2) - plain(v1)) * perc;
    return Array.prototype.map.call(v1, function (x, i) { return x + (v2[i] - x) * perc; });
  };
  function bezier(x1, y1, x2, y2) {
    function at(t, a, b) { var u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; }
    return function (x) {
      if (x <= 0) return 0;
      if (x >= 1) return 1;
      var lo = 0, hi = 1, t = x;
      for (var i = 0; i < 40; i += 1) {
        var v = at(t, x1, x2);
        if (Math.abs(v - x) < 1e-7) break;
        if (v < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
      }
      return at(t, y1, y2);
    };
  }
  var easeInBez = bezier(0.333, 0, 0.833, 0.833);
  var easeOutBez = bezier(0.167, 0.167, 0.667, 1);
  var easeInOutBez = bezier(0.33, 0, 0.667, 1);
  function applyEase(fn, t, tMin, tMax, v1, v2) {
    if (v1 === undefined) { v1 = tMin; v2 = tMax; } else { t = (t - tMin) / (tMax - tMin); }
    t = t > 1 ? 1 : t < 0 ? 0 : t;
    var k = fn(t);
    if (isArray(v1)) return Array.prototype.map.call(v1, function (x, i) { return (v2[i] - x) * k + x; });
    return (plain(v2) - plain(v1)) * k + plain(v1);
  }
  g.ease = function (t, a, b, c, d) { return applyEase(easeInOutBez, t, a, b, c, d); };
  g.easeIn = function (t, a, b, c, d) { return applyEase(easeInBez, t, a, b, c, d); };
  g.easeOut = function (t, a, b, c, d) { return applyEase(easeOutBez, t, a, b, c, d); };

  // The random generator. Each evaluation seeds it from the property and the frame, so a property draws the same
  // numbers at a frame every time, and different properties draw different numbers. seedRandom offsets the seed, and a
  // timeless seed holds across frames.
  var seed = 1;
  function next() {
    seed = (seed * 1103515245 + 12345) % 2147483648;
    return seed / 2147483648;
  }
  function mix(a, b) {
    var h = Math.imul(a ^ ((b + 0x9e3779b9) | 0), 0x85ebca6b);
    h ^= h >>> 13;
    h = Math.imul(h, 0xc2b2ae35);
    h ^= h >>> 16;
    return h >>> 0;
  }
  function reseed(offset, timeless) {
    var frame = timeless ? 0 : Math.floor(state.time * $n.frameRate() + 0.5);
    seed = mix(mix(cur.node, offset | 0), frame) % 2147483647 + 1;
  }
  g.seedRandom = function (s, timeless) { reseed(Math.floor(Math.abs(s)), !!timeless); };
  g.random = function (min, max) {
    if (max === undefined) {
      if (min === undefined) { min = 0; max = 1; } else { max = min; min = undefined; }
    }
    var r = next();
    if (max.length) {
      return Array.prototype.map.call(max, function (x, i) { var m = min ? min[i] : 0; return m + r * (x - m); });
    }
    return (min || 0) + r * (max - min || max);
  };
  g.gaussRandom = function (min, max) { return g.random(min, max); };
  g.lookAt = function (a, b) {
    var f = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    var pitch = Math.atan2(f[0], Math.sqrt(f[1] * f[1] + f[2] * f[2])) / degToRads;
    var yaw = -Math.atan2(f[1], f[2]) / degToRads;
    return [yaw, pitch, 0];
  };
  g.framesToTime = function (f, fps) { return f / (fps || $n.frameRate()); };
  g.timeToFrames = function (t, fps) { return t * (fps || $n.frameRate()); };

  // A path value, with the methods of After Effects' paths: its parts, and the points along it.
  function pathOf(floats) {
    var p = { $path: floats };
    var count = floats[1];
    function part(offset) {
      var out = [];
      for (var i = 0; i < count; i += 1) {
        var at = 2 + i * 6;
        out.push(offset === 0 ? [floats[at], floats[at + 1]]
                              : [floats[at + offset] - floats[at], floats[at + offset + 1] - floats[at + 1]]);
      }
      return out;
    }
    function segments() {
      var out = [];
      var n = floats[0] !== 0 ? count : count - 1;
      for (var i = 0; i < n; i += 1) {
        var a = 2 + i * 6, b = 2 + ((i + 1) % count) * 6;
        out.push([[floats[a], floats[a + 1]], [floats[a + 4], floats[a + 5]], [floats[b + 2], floats[b + 3]],
                  [floats[b], floats[b + 1]]]);
      }
      return out;
    }
    function cubic(s, t) {
      var u = 1 - t;
      return [u * u * u * s[0][0] + 3 * u * u * t * s[1][0] + 3 * u * t * t * s[2][0] + t * t * t * s[3][0],
              u * u * u * s[0][1] + 3 * u * u * t * s[1][1] + 3 * u * t * t * s[2][1] + t * t * t * s[3][1]];
    }
    // The number of points per segment at which `along` measures a path's length.
    var kSegmentSamples = 150;
    // Returns the point and direction at `perc` of the path's length, measured at kSegmentSamples points per segment.
    function along(perc) {
      var segs = segments(), samples = [], total = 0;
      segs.forEach(function (s) {
        var last = cubic(s, 0);
        for (var k = 1; k <= kSegmentSamples; k += 1) {
          var pt = cubic(s, k / kSegmentSamples);
          var d = Math.sqrt(Math.pow(pt[0] - last[0], 2) + Math.pow(pt[1] - last[1], 2));
          total += d;
          samples.push({ at: total, point: pt, from: last });
          last = pt;
        }
      });
      if (!samples.length) return { point: [0, 0], tangent: [1, 0] };
      var target = Math.max(0, Math.min(1, perc)) * total;
      for (var i = 0; i < samples.length; i += 1) {
        if (samples[i].at >= target) {
          var s = samples[i], dx = s.point[0] - s.from[0], dy = s.point[1] - s.from[1];
          var len = Math.sqrt(dx * dx + dy * dy) || 1;
          return { point: s.point, tangent: [dx / len, dy / len] };
        }
      }
      var e = samples[samples.length - 1];
      return { point: e.point, tangent: [1, 0] };
    }
    p.points = function () { return part(0); };
    p.inTangents = function () { return part(2); };
    p.outTangents = function () { return part(4); };
    p.isClosed = function () { return floats[0] !== 0; };
    p.pointOnPath = function (perc) { return along(perc === undefined ? 0.5 : perc).point; };
    p.tangentOnPath = function (perc) { return along(perc === undefined ? 0.5 : perc).tangent; };
    p.normalOnPath = function (perc) { var t = along(perc === undefined ? 0.5 : perc).tangent; return [-t[1], t[0]]; };
    return p;
  }
  g.createPath = function (points, inTangents, outTangents, closed) {
    var floats = [closed ? 1 : 0, points.length];
    for (var i = 0; i < points.length; i += 1) {
      var inT = inTangents && inTangents[i] ? inTangents[i] : [0, 0];
      var outT = outTangents && outTangents[i] ? outTangents[i] : [0, 0];
      floats.push(points[i][0], points[i][1], points[i][0] + inT[0], points[i][1] + inT[1],
                  points[i][0] + outT[0], points[i][1] + outT[1]);
    }
    return pathOf(floats);
  };

  // The After Effects names for each role's accessor: its display name and match names. Lookups by name accept any of
  // them.
  var roles = $n.roles();
  var names = {
    transform: ['Transform', 'ADBE Transform Group', 'ADBE Vector Transform Group'],
    anchorPoint: ['Anchor Point', 'ADBE Anchor Point', 'ADBE Vector Anchor'],
    position: ['Position', 'ADBE Position', 'ADBE Vector Position'],
    xPosition: ['X Position', 'ADBE Position_0'],
    yPosition: ['Y Position', 'ADBE Position_1'],
    zPosition: ['Z Position', 'ADBE Position_2'],
    scale: ['Scale', 'ADBE Scale', 'ADBE Vector Scale'],
    rotation: ['Rotation', 'Z Rotation', 'ADBE Rotate Z', 'ADBE Vector Rotation'],
    xRotation: ['X Rotation', 'ADBE Rotate X'],
    yRotation: ['Y Rotation', 'ADBE Rotate Y'],
    orientation: ['Orientation', 'ADBE Orientation'],
    opacity: ['Opacity', 'ADBE Opacity', 'ADBE Vector Group Opacity', 'ADBE Vector Fill Opacity',
              'ADBE Vector Stroke Opacity'],
    skew: ['Skew', 'ADBE Vector Skew'],
    skewAxis: ['Skew Axis', 'ADBE Vector Skew Axis'],
    path: ['Path', 'Shape', 'ADBE Vector Shape'],
    size: ['Size', 'ADBE Vector Rect Size', 'ADBE Vector Ellipse Size'],
    roundness: ['Roundness', 'Radius', 'ADBE Vector Rect Roundness', 'ADBE Vector RoundCorner Radius'],
    color: ['Color', 'ADBE Vector Fill Color', 'ADBE Vector Stroke Color'],
    strokeWidth: ['Stroke Width', 'ADBE Vector Stroke Width'],
    start: ['Start', 'Start Point', 'ADBE Vector Trim Start', 'ADBE Vector Grad Start Pt'],
    end: ['End', 'End Point', 'ADBE Vector Trim End', 'ADBE Vector Grad End Pt'],
    offset: ['Offset', 'ADBE Vector Trim Offset'],
    points: ['Points', 'ADBE Vector Star Points'],
    outerRadius: ['Outer Radius'],
    innerRadius: ['Inner Radius'],
    outerRoundness: ['Outer Roundness'],
    innerRoundness: ['Inner Roundness'],
    copies: ['Copies'],
    amount: ['Amount'],
    maskPath: ['Mask Path', 'ADBE Mask Shape'],
    maskOpacity: ['Mask Opacity', 'ADBE Mask Opacity'],
    maskExpansion: ['Mask Expansion', 'ADBE Mask Offset'],
    timeRemap: ['Time Remap', 'ADBE Time Remapping'],
  };
  var accessorOf = [];
  Object.keys(roles).forEach(function (a) { accessorOf[roles[a]] = a; });

  var state = { time: 0 };
  function valueAt(node, seconds, expressed) {
    var v = $n.value(node, seconds, expressed);
    return v !== null && typeof v === 'object' && !isArray(v) ? pathOf(v.$path) : v;
  }
  // Caches a query about a node. The tree does not change, so each query runs once.
  function cached(ask) {
    var answers = [];
    return function (node) {
      var a = answers[node];
      if (a === undefined) {
        a = ask(node);
        answers[node] = a;
      }
      return a;
    };
  }
  var childrenOf = cached($n.children);
  var nameOf = cached($n.name);
  var matchNameOf = cached($n.matchName);
  var roleOf = cached($n.role);
  var kindOf = cached($n.kind);
  var indexOf = cached($n.index);
  function childByRole(node, role) {
    var c = childrenOf(node);
    for (var i = 0; i < c.length; i += 1) if (roleOf(c[i]) === role) return c[i];
    return -1;
  }
  // Returns true if `v` identifies the node by its name, match name or index, or by a name of its role.
  function answersTo(node, v) {
    if (typeof v === 'number') return indexOf(node) === v;
    if (nameOf(node) === v || matchNameOf(node) === v) return true;
    var accessor = accessorOf[roleOf(node)];
    return accessor !== undefined && (accessor === v || (names[accessor] || []).indexOf(v) >= 0);
  }
  // The child that `v` identifies, cached per node.
  var finds = [];
  function find(node, v, skipRole) {
    var byName = finds[node] || (finds[node] = new Map());
    var key = typeof v + ':' + v + ':' + skipRole;
    var found = byName.get(key);
    if (found === undefined) {
      found = findChild(node, v, skipRole);
      byName.set(key, found);
    }
    return found;
  }
  function findChild(node, v, skipRole) {
    var c = childrenOf(node).filter(function (x) { return skipRole === undefined || roleOf(x) !== skipRole; });
    for (var i = 0; i < c.length; i += 1) if (answersTo(c[i], v)) return c[i];
    if (typeof v === 'number') return c[v - 1] === undefined ? -1 : c[v - 1];
    return -1;
  }
  function ancestor(node, n) {
    var at = node;
    for (var i = 0; i < (n === undefined ? 1 : n) && at >= 0; i += 1) at = $n.parent(at);
    return at >= 0 ? interfaceOf(at) : null;
  }
  function common(f, node) {
    Object.defineProperty(f, 'name', { get: function () { return nameOf(node); }, configurable: true });
    f._name = nameOf(node);
    f.nm = f._name;
    f.mn = matchNameOf(node);
    f.$node = node;
    f.propertyIndex = indexOf(node);
    f.numProperties = childrenOf(node).length;
    f.propertyGroup = function (n) { return ancestor(node, n); };
    return f;
  }
  // Accessors for each child by its role's name, such as `transform.position`.
  function accessors(f, node) {
    childrenOf(node).forEach(function (child) {
      var accessor = accessorOf[roleOf(child)];
      if (!accessor || accessor in f) return;
      Object.defineProperty(f, accessor, { get: function () { return interfaceOf(child); }, configurable: true });
      if (accessor === 'start' && !('startPoint' in f))
        Object.defineProperty(f, 'startPoint', { get: function () { return interfaceOf(child); }, configurable: true });
      if (accessor === 'end' && !('endPoint' in f))
        Object.defineProperty(f, 'endPoint', { get: function () { return interfaceOf(child); }, configurable: true });
      if (accessor === 'rotation' && !('zRotation' in f))
        Object.defineProperty(f, 'zRotation', { get: function () { return interfaceOf(child); }, configurable: true });
    });
    return f;
  }
  // A property's value with its interface: a Number, an array or a path. The value's prototype holds the methods, and
  // is made once per node and kind of value.
  var wrappers = [];
  function wrap(node, v) {
    var out = typeof v === 'number' ? new Number(v) : v;
    if (out === null || out === undefined || typeof out !== 'object') return out;
    var base = Object.getPrototypeOf(out);
    var protos = wrappers[node] || (wrappers[node] = []);
    var proto;
    for (var i = 0; i < protos.length && proto === undefined; i += 1)
      if (protos[i].base === base) proto = protos[i].proto;
    if (proto === undefined) {
      proto = wrapperOf(node, base);
      protos.push({ base: base, proto: proto });
    }
    Object.setPrototypeOf(out, proto);
    return out;
  }
  function wrapperOf(node, base) {
    var proto = Object.create(base);
    function define(name, get) { Object.defineProperty(proto, name, { get: get, configurable: true }); }
    function method(name, f) { Object.defineProperty(proto, name, { value: f, configurable: true, writable: true }); }
    define('value', function () { return plain(this); });
    define('$node', function () { return node; });
    define('numKeys', function () { return $n.keys(node); });
    define('name', function () { return nameOf(node); });
    define('_name', function () { return nameOf(node); });
    define('propertyIndex', function () { return indexOf(node); });
    define('velocity', function () { return this.velocityAtTime(state.time); });
    method('valueAtTime', function (t) { return valueAt(node, t, false); });
    method('velocityAtTime', function (t) {
      var a = valueAt(node, t, false), b = valueAt(node, t - 0.001, false);
      return isArray(a) ? a.map(function (x, i) { return (x - b[i]) / 0.001; }) : (a - b) / 0.001;
    });
    method('speedAtTime', function (t) { return length(this.velocityAtTime(t)); });
    method('key', function (i) { return keyOf(node, i); });
    method('nearestKey', function (t) { return nearestKey(node, t); });
    method('propertyGroup', function (n) { return ancestor(node, n); });
    method('loopOut', function (type, n) { return loop(node, plain(this), type, n, false, true); });
    method('loopIn', function (type, n) { return loop(node, plain(this), type, n, false, false); });
    method('loopOutDuration', function (type, d) { return loop(node, plain(this), type, d, true, true); });
    method('loopInDuration', function (type, d) { return loop(node, plain(this), type, d, true, false); });
    method('smooth', function (width, samples) { return smooth(node, plain(this), width, samples); });
    return proto;
  }
  function keyOf(node, i) {
    var t = $n.keyTime(node, i - 1);
    var v = valueAt(node, t, false);
    var k = { time: t, value: v, index: i };
    if (isArray(v)) for (var j = 0; j < v.length; j += 1) k[j] = v[j];
    return k;
  }
  function nearestKey(node, t) {
    var count = $n.keys(node);
    if (count === 0) return { index: 0, time: 0 };
    var best = 0;
    for (var i = 1; i < count; i += 1)
      if (Math.abs($n.keyTime(node, i) - t) < Math.abs($n.keyTime(node, best) - t)) best = i;
    return { index: best + 1, time: $n.keyTime(node, best) };
  }
  function loop(node, current, type, n, byDuration, out) {
    var count = $n.keys(node);
    if (count < 2) return current;
    type = type ? type.toLowerCase() : '';
    var fps = $n.frameRate();
    var now = state.time;
    var first, last, cycle;
    if (out) {
      last = $n.keyTime(node, count - 1);
      if (now <= last) return current;
      if (!byDuration) {
        if (!n || n > count - 1) n = count - 1;
        first = $n.keyTime(node, count - 1 - n);
        cycle = last - first;
      } else {
        cycle = !n ? Math.max(0, last - state.inPoint) : Math.abs(last - n);
        first = last - cycle;
      }
    } else {
      first = $n.keyTime(node, 0);
      if (now >= first) return current;
      if (!byDuration) {
        if (!n || n > count - 1) n = count - 1;
        last = $n.keyTime(node, n);
        cycle = last - first;
      } else {
        cycle = !n ? Math.max(0, state.outPoint - first) : Math.abs(n);
        last = first + cycle;
      }
    }
    if (!(cycle > 0)) return current;
    function at(t) { return valueAt(node, t, false); }
    function combine(a, b, f) {
      return isArray(a) ? Array.prototype.map.call(a, function (x, i) { return f(x, b[i]); }) : f(a, b);
    }
    if (out) {
      if (type === 'pingpong') {
        if (Math.floor((now - first) / cycle) % 2 !== 0) return at(cycle - (now - first) % cycle + first);
      } else if (type === 'offset') {
        var repeats = Math.floor((now - first) / cycle);
        var span = combine(at(last), at(first), function (x, y) { return x - y; });
        return combine(span, at((now - first) % cycle + first), function (x, y) { return x * repeats + y; });
      } else if (type === 'continue') {
        var lastValue = at(last), before = at(last - 0.001 / fps);
        return combine(lastValue, before, function (x, y) { return x + (x - y) * ((now - last) * fps) / 0.001; });
      }
      return at((now - first) % cycle + first);
    }
    if (type === 'pingpong') {
      if (Math.floor((first - now) / cycle) % 2 === 0) return at((first - now) % cycle + first);
    } else if (type === 'offset') {
      var times = Math.floor((first - now) / cycle) + 1;
      var width = combine(at(last), at(first), function (x, y) { return x - y; });
      return combine(at(cycle - (first - now) % cycle + first), width, function (x, y) { return x - y * times; });
    } else if (type === 'continue') {
      var firstValue = at(first), after = at(first + 0.001 / fps);
      return combine(firstValue, after, function (x, y) { return x + (x - y) * (first - now) * fps / 0.001; });
    }
    return at(cycle - ((first - now) % cycle + first));
  }
  function smooth(node, current, width, samples) {
    width = (width || 0.4) * 0.5;
    samples = Math.floor(samples || 5);
    if (samples <= 1 || $n.keys(node) === 0) return current;
    var start = state.time - width;
    var step = (2 * width) / (samples - 1);
    var total = null;
    for (var i = 0; i < samples; i += 1) {
      var s = valueAt(node, start + i * step, false);
      total = total === null ? s : sum(total, s);
    }
    return div(total, samples);
  }

  // The interfaces to After Effects' tree. A group's interface is a function that takes a child's name or number. A
  // node's interface is cached once made. It holds nothing that depends on time, except a property's value.
  var made = [];
  function memo(node, make) {
    var m = made[node];
    if (m === undefined) {
      m = make(node);
      made[node] = m;
    }
    return m;
  }
  function interfaceOf(node) {
    if (node === undefined || node < 0) return null;
    var kind = kindOf(node);
    if (kind === 3) return wrap(node, valueAt(node, state.time, true));
    if (kind === 1) return layerOf(node);
    if (kind === 0) return compOf(node);
    return memo(node, groupInterface);
  }
  function groupInterface(node) {
    var role = roleOf(node);
    if (role === roles.$contents) return contentsOf(node);
    if (role === roles.$shapeGroup) return groupOf(node);
    if (role === roles.$effects) return effectsOf(node);
    if (role === roles.$effect) return effectOf(node);
    if (role === roles.$masks) return masksOf(node);
    return itemOf(node);
  }
  function itemOf(node) {
    return accessors(common(function (v) { return interfaceOf(find(node, v)); }, node), node);
  }
  function contentsOf(node) {
    var f = common(function (v) { return interfaceOf(find(node, v, roles.transform)); }, node);
    var group = $n.parent(node);
    Object.defineProperty(f, 'transform', {
      get: function () { return interfaceOf(childByRole(group, roles.transform)); }, configurable: true,
    });
    f.numProperties = childrenOf(node).length;
    return f;
  }
  function groupOf(node) {
    var contents = childByRole(node, roles.$contents);
    var f = common(function (v) {
      if (v === 'ADBE Vectors Group' || v === 'Contents' || v === 2) return interfaceOf(contents);
      return interfaceOf(childByRole(node, roles.transform));
    }, node);
    Object.defineProperty(f, 'content', { get: function () { return interfaceOf(contents); }, configurable: true });
    Object.defineProperty(f, 'transform', {
      get: function () { return interfaceOf(childByRole(node, roles.transform)); }, configurable: true,
    });
    return f;
  }
  function effectsOf(node) {
    return common(function (v) { return interfaceOf(find(node, v)); }, node);
  }
  function effectOf(node) {
    var f = common(function (v) {
      var child = find(node, v);
      if (child < 0) throw new Error('no effect value ' + v);
      return interfaceOf(child);
    }, node);
    f.enabled = f.active = true;
    return f;
  }
  function masksOf(node) {
    return common(function (v) { return interfaceOf(find(node, v)); }, node);
  }
  function transformOf(layer) { return childByRole(layer, roles.transform); }
  function toComp(layer, point, t) {
    var m = $n.toComp(layer, t === undefined ? state.time : t);
    return [m[0] * point[0] + m[2] * point[1] + m[4], m[1] * point[0] + m[3] * point[1] + m[5], 0];
  }
  function fromComp(layer, point, t) {
    var m = $n.toComp(layer, t === undefined ? state.time : t);
    var det = m[0] * m[3] - m[1] * m[2];
    if (det === 0) return [0, 0, 0];
    var x = point[0] - m[4], y = point[1] - m[5];
    return [(m[3] * x - m[2] * y) / det, (m[0] * y - m[1] * x) / det, 0];
  }
  function layerOf(node) { return memo(node, makeLayer); }
  function makeLayer(node) {
    var contents = childByRole(node, roles.$contents);
    var effects = childByRole(node, roles.$effects);
    var masks = childByRole(node, roles.$masks);
    var f = common(function (v) {
      switch (v) {
        case 'ADBE Root Vectors Group': case 'Contents': case 2:
          return interfaceOf(contents);
        case 1: case 6: case 'Transform': case 'transform': case 'ADBE Transform Group':
          return interfaceOf(transformOf(node));
        case 4: case 'ADBE Effect Parade': case 'effects': case 'Effects':
          return interfaceOf(effects);
        default:
          return null;
      }
    }, node);
    f.content = function (v) {
      var c = interfaceOf(contents);
      return v === undefined || c === null ? c : c(v);
    };
    f.effect = function (v) {
      var e = interfaceOf(effects);
      return v === undefined || e === null ? e : e(v);
    };
    f.mask = function (v) {
      var m = interfaceOf(masks);
      return v === undefined || m === null ? m : m(v);
    };
    Object.defineProperty(f, 'transform', { get: function () { return interfaceOf(transformOf(node)); } });
    ['anchorPoint', 'position', 'scale', 'rotation', 'opacity'].forEach(function (p) {
      Object.defineProperty(f, p, {
        get: function () { var t = interfaceOf(transformOf(node)); return t && t[p]; }, configurable: true,
      });
    });
    f.anchor_point = f.anchorPoint;
    f.index = $n.layerIndex(node);
    f.inPoint = $n.inPoint(node);
    f.outPoint = $n.outPoint(node);
    f.startTime = $n.startTime(node);
    var size = $n.compSize($n.comp(node));
    f.width = size[0];
    f.height = size[1];
    var parent = $n.layerParent(node);
    Object.defineProperty(f, 'hasParent', { get: function () { return parent >= 0; } });
    Object.defineProperty(f, 'parent', { get: function () { return parent >= 0 ? layerOf(parent) : undefined; } });
    Object.defineProperty(f, 'active', {
      get: function () { return state.time >= f.inPoint && state.time < f.outPoint; },
    });
    f.toComp = f.toWorld = function (p, t) { return toComp(node, p, t); };
    f.fromComp = f.fromWorld = function (p, t) { return fromComp(node, p, t); };
    f.toWorldVec = function (p, t) {
      var o = toComp(node, [0, 0], t), q = toComp(node, p, t);
      return [q[0] - o[0], q[1] - o[1], 0];
    };
    f.fromWorldVec = function (p, t) {
      var o = fromComp(node, [0, 0], t), q = fromComp(node, p, t);
      return [q[0] - o[0], q[1] - o[1], 0];
    };
    f.sourceRectAtTime = function () { return { top: 0, left: 0, width: 0, height: 0 }; };
    f.sampleImage = function () { return [1, 1, 1, 1]; };
    return f;
  }
  function compOf(node) { return memo(node, makeComp); }
  function makeComp(node) {
    var size = $n.compSize(node);
    var c = {
      $node: node, width: size[0], height: size[1], frameDuration: 1 / $n.frameRate(), displayStartTime: 0,
      pixelAspect: 1, numLayers: childrenOf(node).length, name: nameOf(node),
    };
    c.layer = function (which, offset) {
      var children = childrenOf(node);
      if (typeof which === 'object' && which !== null && typeof offset === 'number') which = which.index + offset;
      for (var i = 0; i < children.length; i += 1) {
        if (typeof which === 'number' ? $n.layerIndex(children[i]) === which : nameOf(children[i]) === which)
          return layerOf(children[i]);
      }
      return null;
    };
    return c;
  }
  g.comp = function () { return compOf($n.root()); };

  // The names visible to an expression, in one scope shared by every evaluation. Each name reads the evaluation in
  // progress. An expression may also assign to a name, which holds for the rest of that evaluation.
  var cur = null;
  var scope = {};
  function current(name) {
    Object.defineProperty(scope, name, {
      get: function () { return cur[name]; }, set: function (x) { cur[name] = x; }, configurable: true,
    });
  }
  function define(name, get) { Object.defineProperty(scope, name, { get: get, configurable: true }); }
  function constant(name, v) { Object.defineProperty(scope, name, { value: v, writable: true, configurable: true }); }
  ['value', 'time', 'thisProperty', 'thisLayer', 'thisComp'].forEach(current);
  define('transform', function () { return cur.thisLayer && cur.thisLayer.transform; });
  define('$bm_transform', function () { return cur.thisLayer && cur.thisLayer.transform; });
  define('content', function () { return cur.thisLayer && cur.thisLayer.content; });
  define('effect', function () { return cur.thisLayer && cur.thisLayer.effect; });
  define('mask', function () { return cur.thisLayer && cur.thisLayer.mask; });
  ['anchorPoint', 'position', 'scale', 'rotation', 'opacity', 'index', 'inPoint', 'outPoint', 'startTime',
   'hasParent', 'parent', 'toComp', 'fromComp', 'toWorld', 'fromWorld', 'sourceRectAtTime',
   'active'].forEach(function (p) { define(p, function () { return cur.thisLayer && cur.thisLayer[p]; }); });
  define('name', function () { return cur.thisLayer ? cur.thisLayer._name : ''; });
  define('width', function () { return cur.thisLayer ? cur.thisLayer.width : cur.thisComp.width; });
  define('height', function () { return cur.thisLayer ? cur.thisLayer.height : cur.thisComp.height; });
  define('numKeys', function () { return cur.thisProperty.numKeys; });
  define('velocity', function () { return cur.thisProperty.velocityAtTime(cur.time); });
  constant('key', function (i) { return cur.thisProperty.key(i); });
  constant('nearestKey', function (t) { return cur.thisProperty.nearestKey(t); });
  constant('valueAtTime', function (t) { return cur.thisProperty.valueAtTime(t); });
  constant('velocityAtTime', function (t) { return cur.thisProperty.velocityAtTime(t); });
  constant('loopOut', function (t, n) { return cur.thisProperty.loopOut(t, n); });
  constant('loopIn', function (t, n) { return cur.thisProperty.loopIn(t, n); });
  constant('loop_out', scope.loopOut);
  constant('loop_in', scope.loopIn);
  constant('loopOutDuration', function (t, d) { return cur.thisProperty.loopOutDuration(t, d); });
  constant('loopInDuration', function (t, d) { return cur.thisProperty.loopInDuration(t, d); });
  constant('smooth', function (w, s) { return cur.thisProperty.smooth(w, s); });
  constant('posterizeTime', function () {});

  // Smooth noise in [-1, 1) at `x`: values drawn from the hash of `key` at whole numbers, and eased between them.
  function noise(key, x) {
    var i = Math.floor(x), f = x - i;
    var a = mix(key, i) / 2147483648 - 1, b = mix(key, i + 1) / 2147483648 - 1;
    return a + (b - a) * f * f * (3 - 2 * f);
  }
  // After Effects' wiggle at time `t`: `freq` wiggles per second, each up to `amp`. Each of the `octaves` adds detail
  // at twice the frequency and `ampMult` times the amplitude of the octave before. Each property and dimension wiggles
  // independently, and gives the same value at a time however often it is drawn.
  constant('wiggle', function (freq, amp, octaves, ampMult, t) {
    octaves = octaves === undefined ? 1 : Math.max(1, Math.floor(octaves));
    ampMult = ampMult === undefined ? 0.5 : ampMult;
    t = t === undefined ? cur.time : t;
    var value = cur.value;
    function offset(k) {
      var sum = 0, a = 1, f = freq;
      for (var o = 0; o < octaves; o += 1) {
        sum += a * noise(mix(mix(cur.node, k), o), t * f);
        a *= ampMult;
        f *= 2;
      }
      return amp * sum;
    }
    if (!isArray(value)) return plain(value) + offset(0);
    return Array.prototype.map.call(value, function (x, i) { return x + offset(i); });
  });

  // Evaluates an expression's function for the property `node` at `seconds`. `value` is the property's own value.
  return function (fn, node, seconds, value) {
    var layer = node;
    while (layer >= 0 && kindOf(layer) !== 1) layer = $n.parent(layer);
    var own = valueAt(node, seconds, false);
    if (own !== null && typeof own === 'object' && own.$path) value = own;
    var outer = { time: state.time, inPoint: state.inPoint, outPoint: state.outPoint, cur: cur, seed: seed };
    state.time = seconds;
    state.inPoint = layer >= 0 ? $n.inPoint(layer) : 0;
    state.outPoint = layer >= 0 ? $n.outPoint(layer) : 0;
    cur = {
      node: node, value: value, time: seconds, thisProperty: wrap(node, value),
      thisLayer: layer >= 0 ? layerOf(layer) : undefined,
      thisComp: layer >= 0 ? compOf($n.comp(layer)) : compOf($n.root()),
    };
    reseed(0, false);
    try {
      var result = fn(scope);
      if (result === undefined) return undefined;
      if (result !== null && typeof result === 'object' && result.$path) return { $path: result.$path };
      if (typeof result === 'function' || result === null) return undefined;
      if (result instanceof Number || typeof result === 'boolean') return Number(result);
      if (isArray(result)) return Array.prototype.slice.call(result).map(Number);
      return result;
    } finally {
      state.time = outer.time;
      state.inPoint = outer.inPoint;
      state.outPoint = outer.outPoint;
      cur = outer.cur;
      seed = outer.seed;
    }
  };
})
)JS";

} // namespace pr_impl

PR_ASSUME_NONNULL_END

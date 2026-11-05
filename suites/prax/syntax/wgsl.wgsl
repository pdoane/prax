// Particles that a compute pass moves and a render pass draws as points.

struct Particle {
    position: vec2<f32>,
    velocity: vec2<f32>,
    life: f32,
}

struct Params {
    delta: f32,
    gravity: vec2<f32>,
}

const DAMPING: f32 = 0.98;

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read_write> particles: array<Particle>;

@compute @workgroup_size(64)
fn simulate(@builtin(global_invocation_id) id: vec3<u32>) {
    let i = id.x;
    if (i >= arrayLength(&particles)) {
        return;
    }
    var p = particles[i];
    p.velocity = (p.velocity + params.gravity * params.delta) * DAMPING;
    p.position += p.velocity * params.delta;
    p.life = max(p.life - params.delta, 0.0);
    particles[i] = p;
}

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) alpha: f32,
}

@vertex
fn vertexMain(@builtin(vertex_index) index: u32) -> VertexOut {
    let p = particles[index];
    var out: VertexOut;
    out.position = vec4<f32>(p.position, 0.0, 1.0);
    out.alpha = clamp(p.life, 0.0, 1.0);
    return out;
}

@fragment
fn fragmentMain(in: VertexOut) -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 0.6, 0.2, in.alpha);
}

// Reconstruct perspective interpolation from original clip-space vertices and the actual host viewport.
// Behaviour reference: Eden shader_recompiler/ir_opt/position_varying_pass.cpp BarycentricVaryingPass,
// PR 4575, head 22263170810a93c1df30a10bab15d068fc55a189 (Amicuchu, John).
// Independently implemented at the SPIR-V boundary: Ryujinx's separately translated stages stay unchanged.
// SPV_KHR_fragment_shader_barycentric supplies raw vertex values, not the host's interpolated weights.
#include "vk_varying.h"
#include <stdlib.h>
#include <string.h>

enum { EXT = 10, IMPORT = 11, EXTINST = 12, ENTRY = 15, MODE = 16, CAP = 17,
       INT = 21, FLOAT = 22, VECTOR = 23, ARRAY = 28, STRUCT = 30, POINTER = 32, CONSTANT = 43,
       FUNCTION = 54, VARIABLE = 59, LOAD = 61, STORE = 62, ACCESS = 65, INACCESS = 66,
       DECORATE = 71, MEMBER = 72, EXTRACT = 81, CONSTRUCT = 80, FADD = 129, FSUB = 131,
       FMUL = 133, FDIV = 136, LABEL = 248, RETURN = 253,
       INPUT = 1, OUTPUT = 3, PRIVATE = 6, UNIFORM = 2,
       BUILTIN = 11, NOPERSPECTIVE = 13, FLAT = 14, CENTROID = 16, SAMPLE = 17,
       LOCATION = 30, COMPONENT = 31, OFFSET = 35, BLOCK = 2, SET = 34, BINDING = 33,
       POSITION = 0, FRAGCOORD = 15, VIEWPORTINDEX = 10, PERVERTEX = 5285, BARYCENTRIC = 5284 };

typedef struct { uint32_t *p; size_t n, cap; int failed; } Words;
static void append(Words *b, const uint32_t *w, size_t n) {
    if (b->failed) return;
    if (n > SIZE_MAX / sizeof *w - b->n) { b->failed = 1; return; }
    if (b->n + n > b->cap) {
        size_t cap = (b->n + n) * 2 + 64;
        uint32_t *p = realloc(b->p, cap * sizeof *p);
        if (!p) { b->failed = 1; return; }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, w, n * sizeof *w), b->n += n;
}
static void inst(Words *b, unsigned op, const uint32_t *args, size_t n) {
    uint32_t h = (uint32_t)((n + 1) << 16) | op;
    append(b, &h, 1), append(b, args, n);
}
#define E(b, op, ...) do { const uint32_t args_[] = {__VA_ARGS__}; inst((b), (op), args_, sizeof args_ / sizeof *args_); } while (0)
static void string_inst(Words *b, unsigned op, const char *s) {
    size_t n = (strlen(s) + 4) / 4;
    uint32_t data[32] = {0};
    if (n > 32) { b->failed = 1; return; }
    memcpy(data, s, strlen(s));
    inst(b, op, data, n);
}

typedef struct {
    unsigned op, storage, count, flags;
    uint32_t type, element, loc, builtin, member, root;
    size_t at;
} Id;
typedef struct {
    const uint32_t *w; size_t n, first_function;
    uint32_t bound, next, entry, float_t, vec4, uint_t, glsl;
    Id *id;
    Words annotations, declarations;
} Module;

static int parse(Module *m, const uint32_t *w, size_t n, unsigned stage) {
    memset(m, 0, sizeof *m);
    if (n < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 20)) return 0;
    m->w = w, m->n = n, m->bound = m->next = w[3];
    m->id = calloc(m->bound, sizeof *m->id);
    if (!m->id) return 0;
    for (uint32_t i = 0; i < m->bound; i++) m->id[i].loc = m->id[i].builtin = UINT32_MAX;
    unsigned entries = 0;
    for (size_t at = 5; at < n;) {
        const uint32_t *p = w + at;
        unsigned len = p[0] >> 16, op = p[0] & 65535;
        if (!len || len > n - at) return 0;
        if (((op == POINTER || op == VECTOR || op == ARRAY || op == INT || op == VARIABLE || op == CONSTANT) && len < 4) ||
            ((op == ACCESS || op == INACCESS || op == FUNCTION) && len < 5) || (op == FLOAT && len < 3)) return 0;
        uint32_t result = 0;
        if (op >= INT && op <= POINTER && len >= 2) result = p[1];
        else if ((op == VARIABLE || op == ACCESS || op == INACCESS || op == CONSTANT) && len >= 4) result = p[2];
        if (result) {
            if (result >= m->bound) return 0;
            Id *d = &m->id[result];
            d->op = op, d->at = at;
            if (op == POINTER) d->storage = p[2], d->element = p[3];
            if (op == FLOAT && p[2] == 32) m->float_t = result;
            if (op == INT && len == 4 && p[2] == 32 && p[3] == 0) m->uint_t = result;
            if (op == VECTOR || op == ARRAY) d->element = p[2], d->count = p[3];
            if (op == VARIABLE) d->type = p[1], d->storage = p[3];
            if (op == ACCESS || op == INACCESS) d->type = p[1], d->root = p[3];
            if (d->type >= m->bound || d->element >= m->bound || d->root >= m->bound) return 0;
        }
        if (op == ENTRY) {
            if (len < 4 || p[1] != stage || p[2] >= m->bound || !memchr(p + 3, 0, (len - 3) * 4) || ++entries != 1) return 0;
            m->entry = p[2];
        }
        if (op == MODE && len >= 3 && p[2] == 11) return 0; // transform feedback
        if (op == IMPORT && len > 2 && memchr(p + 2, 0, (len - 2) * 4) && !strcmp((const char *)(p + 2), "GLSL.std.450")) m->glsl = p[1];
        if (op == DECORATE && len >= 3 && p[1] < m->bound) {
            Id *d = &m->id[p[1]];
            if (p[2] == LOCATION && len == 4) d->loc = p[3];
            if (p[2] == BUILTIN && len == 4) d->builtin = p[3];
            if (p[2] == NOPERSPECTIVE || p[2] == FLAT || p[2] == CENTROID || p[2] == SAMPLE || p[2] == COMPONENT || p[2] == PERVERTEX)
                d->flags = 1;
        }
        if (op == MEMBER && len >= 5 && p[1] < m->bound && p[3] == BUILTIN && p[4] == POSITION)
            m->id[p[1]].builtin = POSITION, m->id[p[1]].member = p[2];
        if (op == FUNCTION && !m->first_function) m->first_function = at;
        at += len;
    }
    if (!entries || !m->first_function || !m->float_t) return 0;
    for (uint32_t i = 1; i < m->bound; i++)
        if (m->id[i].op == VECTOR && m->id[i].element == m->float_t && m->id[i].count == 4) m->vec4 = i;
    return m->vec4 != 0;
}
static uint32_t fresh(Module *m) { return m->next++; }
static uint32_t pointer(Module *m, uint32_t storage, uint32_t type) {
    for (uint32_t i = 1; i < m->bound; i++)
        if (m->id[i].op == POINTER && m->id[i].storage == storage && m->id[i].element == type) return i;
    for (size_t at = 0; at < m->declarations.n; at += m->declarations.p[at] >> 16) {
        const uint32_t *s = m->declarations.p + at;
        if ((s[0] & 65535) == POINTER && s[2] == storage && s[3] == type) return s[1];
    }
    uint32_t id = fresh(m);
    E(&m->declarations, POINTER, id, storage, type);
    return id;
}
static uint32_t constant(Module *m, uint32_t type, uint32_t bits) {
    uint32_t id = fresh(m);
    E(&m->declarations, CONSTANT, type, id, bits);
    return id;
}
static uint32_t value_type(const Module *m, uint32_t var) {
    uint32_t t = m->id[var].type;
    return t < m->bound && m->id[t].op == POINTER ? m->id[t].element : 0;
}
static unsigned components(const Module *m, uint32_t t) {
    if (t == m->float_t) return 1;
    return t < m->bound && m->id[t].op == VECTOR && m->id[t].element == m->float_t ? m->id[t].count : 0;
}
static int simple_type(const Module *m, uint32_t t) {
    if (!t || t >= m->bound) return 0;
    const Id *d = &m->id[t];
    if (d->op == VECTOR) return d->count <= 4 && simple_type(m, d->element);
    return (d->op == INT || d->op == FLOAT) && m->w[d->at + 2] == 32;
}
static uint32_t load(Module *m, Words *b, uint32_t type, uint32_t ptr) {
    uint32_t id = fresh(m); E(b, LOAD, type, id, ptr); return id;
}
static uint32_t extract(Module *m, Words *b, uint32_t vec, unsigned c) {
    uint32_t id = fresh(m); E(b, EXTRACT, m->float_t, id, vec, c); return id;
}
static uint32_t binary(Module *m, Words *b, unsigned op, uint32_t a, uint32_t c) {
    uint32_t id = fresh(m); E(b, op, m->float_t, id, a, c);
    E(&m->annotations, DECORATE, id, 42); // NoContraction: retain explicit rounding boundaries
    return id;
}
static uint32_t emit_fma(Module *m, Words *b, uint32_t a, uint32_t c, uint32_t d) {
    uint32_t id = fresh(m); E(b, EXTINST, m->float_t, id, m->glsl, 50, a, c, d); return id;
}
static uint32_t indexed_load(Module *m, Words *b, uint32_t ptr_type, uint32_t type, uint32_t var, uint32_t index) {
    uint32_t p = fresh(m); E(b, ACCESS, ptr_type, p, var, index); return load(m, b, type, p);
}

typedef struct { uint32_t old, type, array_var, private_ptr, input_ptr; unsigned count; } Varying;
typedef struct {
    Module v, f;
    Varying vary[32]; unsigned n;
    uint32_t location, position_var, position_member, position_out, position_in, coord, viewport;
    uint32_t *root, *private_ptr;
    Words fbody;
    uint32_t v_index, v_output_ptr;
} Pair;

static int plan(Pair *p, unsigned locations) {
    Module *v = &p->v, *f = &p->f;
    uint32_t used = 0;
    if (!locations || locations > 32) locations = 32;
    for (uint32_t i = 1; i < v->bound; i++) {
        Id *d = &v->id[i];
        if (d->op != VARIABLE || d->storage != OUTPUT) continue;
        if (d->builtin == VIEWPORTINDEX) return 0;
        if (d->loc < 32) {
            if (!simple_type(v, value_type(v, i))) return 0; // arrays/matrices occupy multiple locations
            used |= 1u << d->loc;
        }
        uint32_t t = value_type(v, i);
        if (d->builtin == POSITION && t == v->vec4) p->position_var = i, p->position_member = UINT32_MAX;
        else if (t < v->bound && v->id[t].op == STRUCT && v->id[t].builtin == POSITION) {
            const uint32_t *s = v->w + v->id[t].at;
            unsigned member = v->id[t].member;
            if (member + 2 >= s[0] >> 16 || s[member + 2] != v->vec4) return 0;
            p->position_var = i, p->position_member = member;
        }
    }
    for (uint32_t i = 1; i < f->bound; i++) {
        Id *d = &f->id[i];
        if (d->op != VARIABLE || d->storage != INPUT) continue;
        if (d->builtin == FRAGCOORD && value_type(f, i) == f->vec4) p->coord = i;
        if (d->loc >= 32) continue;
        unsigned count = components(f, value_type(f, i));
        if (!simple_type(f, value_type(f, i))) return 0;
        used |= 1u << d->loc;
        if (!count || count > 4 || d->flags) continue;
        // Both stages must actually declare the location, with the same float type width.
        int found = 0;
        for (uint32_t j = 1; j < v->bound; j++)
            if (v->id[j].op == VARIABLE && v->id[j].storage == OUTPUT && v->id[j].loc == d->loc &&
                !v->id[j].flags && components(v, value_type(v, j)) == count) found = 1;
        if (found) p->vary[p->n++] = (Varying){.old = i, .type = value_type(f, i), .count = count};
    }
    if (!p->n || !p->position_var) return 0;
    for (p->location = 0; p->location < locations; p->location++)
        if (!(used >> p->location & 1)) break;
    if (p->location == locations) return 0;
    return 1;
}

static void declare(Pair *p) {
    Module *v = &p->v, *f = &p->f;
    if (!v->uint_t) { v->uint_t = fresh(v); E(&v->declarations, INT, v->uint_t, 32, 0); }
    if (!f->uint_t) { f->uint_t = fresh(f); E(&f->declarations, INT, f->uint_t, 32, 0); }
    p->v_output_ptr = pointer(v, OUTPUT, v->vec4);
    p->position_out = fresh(v);
    E(&v->declarations, VARIABLE, p->v_output_ptr, p->position_out, OUTPUT);
    E(&v->annotations, DECORATE, p->position_out, LOCATION, p->location);
    if (p->position_member != UINT32_MAX) p->v_index = constant(v, v->uint_t, p->position_member);

    uint32_t length = constant(f, f->uint_t, 3), array = fresh(f);
    E(&f->declarations, ARRAY, array, f->vec4, length);
    uint32_t ptr = pointer(f, INPUT, array);
    p->position_in = fresh(f);
    E(&f->declarations, VARIABLE, ptr, p->position_in, INPUT);
    E(&f->annotations, DECORATE, p->position_in, LOCATION, p->location);
    E(&f->annotations, DECORATE, p->position_in, PERVERTEX);
    if (!p->coord) {
        ptr = pointer(f, INPUT, f->vec4); p->coord = fresh(f);
        E(&f->declarations, VARIABLE, ptr, p->coord, INPUT);
        E(&f->annotations, DECORATE, p->coord, BUILTIN, FRAGCOORD);
    }
    uint32_t block = fresh(f);
    E(&f->declarations, STRUCT, block, f->vec4);
    E(&f->annotations, DECORATE, block, BLOCK);
    E(&f->annotations, MEMBER, block, 0, OFFSET, VK_VARYING_VIEWPORT_OFFSET);
    ptr = pointer(f, UNIFORM, block); p->viewport = fresh(f);
    E(&f->declarations, VARIABLE, ptr, p->viewport, UNIFORM);
    E(&f->annotations, DECORATE, p->viewport, SET, 0);
    E(&f->annotations, DECORATE, p->viewport, BINDING, 0);

    for (unsigned i = 0; i < p->n; i++) {
        Varying *a = &p->vary[i];
        array = fresh(f); E(&f->declarations, ARRAY, array, a->type, length);
        ptr = pointer(f, INPUT, array);
        a->array_var = fresh(f);
        E(&f->declarations, VARIABLE, ptr, a->array_var, INPUT);
        E(&f->annotations, DECORATE, a->array_var, LOCATION, f->id[a->old].loc);
        E(&f->annotations, DECORATE, a->array_var, PERVERTEX);
        a->private_ptr = pointer(f, PRIVATE, a->type);
        a->input_ptr = pointer(f, INPUT, a->type);
        p->root[a->old] = a->old;
        p->private_ptr[a->old] = a->private_ptr;
    }
    // The generated Private variables retain the original ids; every original load/access chain keeps its meaning.
    for (uint32_t i = 1; i < f->bound; i++) {
        Id *d = &f->id[i];
        if (d->op != ACCESS && d->op != INACCESS) continue;
        // Walk the base chain rather than relying on result-id order.
        uint32_t root = d->root;
        for (unsigned depth = 0; root < f->bound && !p->root[root] && f->id[root].root && depth < 64; depth++) root = f->id[root].root;
        if (root < f->bound && p->root[root]) {
            p->root[i] = p->root[root];
            p->private_ptr[i] = pointer(f, PRIVATE, f->id[d->type].element);
        }
    }
}

static void fragment_body(Pair *p) {
    Module *f = &p->f; Words *b = &p->fbody;
    uint32_t index[3];
    for (unsigned i = 0; i < 3; i++) index[i] = constant(f, f->uint_t, i);
    uint32_t input_ptr = pointer(f, INPUT, f->vec4), uniform_ptr = pointer(f, UNIFORM, f->vec4);
    uint32_t coord = load(f, b, f->vec4, p->coord);
    uint32_t vp = indexed_load(f, b, uniform_ptr, f->vec4, p->viewport, index[0]);
    uint32_t two = constant(f, f->float_t, 0x40000000), minus_one = constant(f, f->float_t, 0xbf800000);
    uint32_t q[2];
    for (unsigned c = 0; c < 2; c++) {
        uint32_t pixel = extract(f, b, coord, c), origin = extract(f, b, vp, c), extent = extract(f, b, vp, c + 2);
        uint32_t relative = binary(f, b, FDIV, binary(f, b, FSUB, pixel, origin), extent);
        q[c] = emit_fma(f, b, relative, two, minus_one);
    }
    uint32_t pos[3][3];
    for (unsigned i = 0; i < 3; i++) {
        uint32_t vec = indexed_load(f, b, input_ptr, f->vec4, p->position_in, index[i]);
        for (unsigned c = 0; c < 3; c++) pos[i][c] = extract(f, b, vec, c == 2 ? 3 : c);
    }
    uint32_t edge[3];
    for (unsigned i = 0; i < 3; i++) {
        uint32_t *a = pos[(i + 1) % 3], *d = pos[(i + 2) % 3], cross[3];
        for (unsigned c = 0; c < 3; c++) {
            unsigned j = (c + 1) % 3, k = (c + 2) % 3;
            cross[c] = binary(f, b, FSUB, binary(f, b, FMUL, a[j], d[k]), binary(f, b, FMUL, a[k], d[j]));
        }
        edge[i] = emit_fma(f, b, cross[0], q[0], emit_fma(f, b, cross[1], q[1], cross[2]));
    }
    uint32_t sum = binary(f, b, FADD, edge[0], binary(f, b, FADD, edge[1], edge[2]));
    uint32_t weights[2] = {binary(f, b, FDIV, edge[1], sum), binary(f, b, FDIV, edge[2], sum)};
    for (unsigned i = 0; i < p->n; i++) {
        Varying *a = &p->vary[i]; uint32_t values[3], out[4] = {0};
        for (unsigned j = 0; j < 3; j++) values[j] = indexed_load(f, b, a->input_ptr, a->type, a->array_var, index[j]);
        for (unsigned c = 0; c < a->count; c++) {
            uint32_t x[3];
            for (unsigned j = 0; j < 3; j++) x[j] = a->count == 1 ? values[j] : extract(f, b, values[j], c);
            // Difference form preserves a constant attribute exactly, even when the weights round.
            out[c] = emit_fma(f, b, weights[0], binary(f, b, FSUB, x[1], x[0]), x[0]);
            out[c] = emit_fma(f, b, weights[1], binary(f, b, FSUB, x[2], x[0]), out[c]);
        }
        uint32_t result = out[0];
        if (a->count > 1) {
            result = fresh(f); uint32_t args[6] = {a->type, result, out[0], out[1], out[2], out[3]};
            inst(b, CONSTRUCT, args, a->count + 2);
        }
        E(b, STORE, a->old, result);
    }
}

static int rewrite(Pair *p, Module *m, int fragment, Words *out) {
    const uint32_t *w = m->w;
    append(out, w, 5);
    size_t at = 5;
    while (at < m->n && (w[at] & 65535) == CAP) { unsigned n = w[at] >> 16; append(out, w + at, n); at += n; }
    if (fragment) E(out, CAP, BARYCENTRIC);
    while (at < m->n && (w[at] & 65535) == EXT) { unsigned n = w[at] >> 16; append(out, w + at, n); at += n; }
    if (fragment) string_inst(out, EXT, "SPV_KHR_fragment_shader_barycentric");
    if (fragment && !m->glsl) {
        m->glsl = fresh(m);
        const char *s = "GLSL.std.450"; uint32_t args[5] = {m->glsl, 0, 0, 0, 0};
        memcpy(args + 1, s, strlen(s)); inst(out, IMPORT, args, 4);
    }
    // Generate function instructions first, so all their types/constants and decorations precede OpFunction.
    if (fragment) fragment_body(p);
    Words globals = {0};
    int annotations = 0, declarations = 0, in_entry = 0, entry_label = 0, initialized = 0;
    for (; at < m->n;) {
        const uint32_t *s = w + at; unsigned n = s[0] >> 16, op = s[0] & 65535;
        if (!annotations && op >= 19 && op <= 39) {
            append(out, m->annotations.p, m->annotations.n); annotations = 1;
        }
        if (op == FUNCTION && !declarations) {
            append(out, m->declarations.p, m->declarations.n);
            append(out, globals.p, globals.n); declarations = 1;
        }
        if (op == ENTRY) {
            size_t prefix = 3;
            while (prefix < n && !memchr(s + prefix++, 0, 4)) {}
            Words args = {0}; append(&args, s + 1, prefix - 1);
            for (size_t i = prefix; i < n; i++) {
                if (fragment && s[i] < m->bound && p->root[s[i]] && w[1] < 0x00010400) continue;
                append(&args, s + i, 1);
            }
            if (fragment) {
                append(&args, &p->position_in, 1);
                for (unsigned i = 0; i < p->n; i++) append(&args, &p->vary[i].array_var, 1);
                if (p->coord >= m->bound) append(&args, &p->coord, 1);
                if (w[1] >= 0x00010400) append(&args, &p->viewport, 1);
            } else append(&args, &p->position_out, 1);
            inst(out, ENTRY, args.p, args.n); if (args.failed) out->failed = 1; free(args.p);
        } else if (fragment && op == DECORATE && n >= 3 && s[1] < m->bound && p->root[s[1]] &&
                   (s[2] == LOCATION || s[2] == NOPERSPECTIVE || s[2] == FLAT || s[2] == CENTROID || s[2] == SAMPLE || s[2] == COMPONENT)) {
            // Location and interpolation belong to the new per-vertex input, not its Private copy.
        } else {
            if (op == FUNCTION) in_entry = s[2] == m->entry;
            if (in_entry && op == LABEL) entry_label = 1;
            else if (fragment && in_entry && entry_label && !initialized && op != VARIABLE && op != 8 && op != 317) {
                append(out, p->fbody.p, p->fbody.n); initialized = 1;
            }
            if (fragment && (op == VARIABLE || op == ACCESS || op == INACCESS) && n >= 4 && s[2] < m->bound && p->root[s[2]]) {
                Words args = {0}; append(&args, s + 1, n - 1);
                if (args.failed) { free(globals.p); free(args.p); return 0; }
                args.p[0] = p->private_ptr[s[2]];
                if (op == VARIABLE) args.p[2] = PRIVATE;
                inst(op == VARIABLE && !declarations ? &globals : out, op, args.p, args.n); free(args.p);
            } else {
                if (!fragment && in_entry && op == RETURN) {
                    uint32_t ptr = p->position_var;
                    if (p->position_member != UINT32_MAX) {
                        ptr = fresh(m); E(out, ACCESS, p->v_output_ptr, ptr, p->position_var, p->v_index);
                    }
                    uint32_t pos = load(m, out, m->vec4, ptr);
                    E(out, STORE, p->position_out, pos);
                }
                append(op == VARIABLE && !declarations ? &globals : out, s, n);
            }
        }
        at += n;
    }
    int failed = globals.failed; free(globals.p);
    if (failed || out->failed || m->annotations.failed || m->declarations.failed || (fragment && (!initialized || p->fbody.failed))) return 0;
    out->p[3] = m->next;
    return 1;
}

int vk_varying_rewrite(const uint32_t *vs, size_t vn, const uint32_t *fs, size_t fn, unsigned locations,
                       uint32_t **vout, size_t *vwords, uint32_t **fout, size_t *fwords) {
    *vout = *fout = NULL; *vwords = *fwords = 0;
    Pair p = {0}; Words v = {0}, f = {0}; int result = 0;
    if (!parse(&p.v, vs, vn, 0) || !parse(&p.f, fs, fn, 4) || !plan(&p, locations)) goto done;
    p.root = calloc(p.f.bound, sizeof *p.root), p.private_ptr = calloc(p.f.bound, sizeof *p.private_ptr);
    if (!p.root || !p.private_ptr) goto done;
    declare(&p);
    if (!rewrite(&p, &p.v, 0, &v) || !rewrite(&p, &p.f, 1, &f)) goto done;
    *vout = v.p, *vwords = v.n, *fout = f.p, *fwords = f.n;
    v.p = f.p = NULL; result = (int)p.n;
done:
    free(p.v.id), free(p.f.id), free(p.root), free(p.private_ptr), free(v.p), free(f.p);
    free(p.v.annotations.p), free(p.v.declarations.p), free(p.f.annotations.p), free(p.f.declarations.p);
    free(p.fbody.p);
    return result;
}

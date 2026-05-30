#include "llm.h"
#include "../libc/string.h"
#include "../libc/stdio.h"
#include "../libc/syscall.h"

/* ── Math helpers ── */

static float fast_expf(float x) {
    float n = (float)(int)(x * 1.4426950408889634f + 0.5f);
    float f = x - n * 0.6931471805599453f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (1.0f/6.0f + f * (1.0f/24.0f + f * (1.0f/120.0f)))));
    unsigned int pi = *(unsigned int*)&p;
    int ei = (int)n + 127;
    if (ei >= 255) { unsigned int inf = 0x7F800000; return *(float*)&inf; }
    if (ei <= 0) return 0;
    pi = (pi & 0x807FFFFF) | ((unsigned int)ei << 23);
    return *(float*)&pi;
}

static float fast_logf(float x) {
    unsigned int bx = *(unsigned int*)&x;
    int e = (int)(bx >> 23) - 127;
    unsigned int mb = (bx & 0x007FFFFF) | 0x3F800000;
    float m = *(float*)&mb;
    float y = (m - 1.0f) / (m + 1.0f);
    float y2 = y * y;
    return (float)e * 0.6931471805599453f + y * (2.0f + y2 * (2.0f/3.0f + y2 * (2.0f/5.0f + y2 * (2.0f/7.0f))));
}

static float fast_powf(float a, float b) {
    if (a <= 0.0f) return 0.0f;
    return fast_expf(b * fast_logf(a));
}

static void sincos_pade(float x, float* s, float* c) {
    int q = (int)(x * 0.6366197723675814f + 0.5f);
    float r = x - q * 1.5707963267948966f;
    float rr = r * r;
    *s = r * (1.0f - rr * (1.0f/6.0f - rr * (1.0f/120.0f - rr * (1.0f/5040.0f))));
    *c = 1.0f - rr * (0.5f - rr * (1.0f/24.0f - rr * (1.0f/720.0f - rr * (1.0f/40320.0f))));
    q &= 3;
    if (q == 1) { float t = *s; *s = *c; *c = -t; }
    else if (q == 2) { *s = -*s; *c = -*c; }
    else if (q == 3) { float t = *s; *s = -*c; *c = t; }
}

static float fast_sinf(float x) { float s, c; sincos_pade(x, &s, &c); return s; }
static float fast_cosf(float x) { float s, c; sincos_pade(x, &s, &c); return c; }

static float fast_tanhf(float x) {
    if (x > 5.0f) return 1.0f;
    if (x < -5.0f) return -1.0f;
    float ex = fast_expf(2.0f * x);
    return (ex - 1.0f) / (ex + 1.0f);
}

static float fast_sqrtf(float x) {
    float r; __asm__ volatile("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r;
}

static int rdtsc_int(void) {
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (int)(lo ^ hi);
}

static char* my_strstr(const char* h, const char* n) {
    if (!*n) return (char*)h;
    while (*h) {
        const char* a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char*)h;
        h++;
    }
    return 0;
}

static int my_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static void my_strcpy(char* d, const char* s) {
    while ((*d++ = *s++));
}

/* ── Half-precision ── */

static inline float fp16_to_fp32(unsigned short h) {
    unsigned int bits = (((h >> 15) & 1) << 31) |
                        ((((h >> 10) & 0x1f) + 127 - 15) << 23) |
                        ((h & 0x3ff) << 13);
    return *(float*)&bits;
}

/* ── Q4_0 ── */

static inline float deq_q4_0(const block_q4_0* b, int i) {
    return (float)((int)((b->qs[i/2] >> (4*(i%2))) & 0xF) - 8) * fp16_to_fp32(b->d);
}

static inline unsigned long q4_0_rb(int cols) {
    return ((cols + QK4_0 - 1) / QK4_0) * sizeof(block_q4_0);
}

static float dot_q4_0(const void* w, const float* x, int n) {
    const block_q4_0* b = (const block_q4_0*)w;
    float s = 0; int i = 0;
    while (i + QK4_0 <= n) {
        float d = fp16_to_fp32(b->d);
        for (int j = 0; j < QK4_0; j++)
            s += (float)((int)((b->qs[j/2] >> (4*(j%2))) & 0xF) - 8) * d * x[i + j];
        b++; i += QK4_0;
    }
    if (i < n) {
        float d = fp16_to_fp32(b->d);
        for (int j = 0; i + j < n; j++)
            s += (float)((int)((b->qs[j/2] >> (4*(j%2))) & 0xF) - 8) * d * x[i + j];
    }
    return s;
}

/* ── Vector ops ── */

static float vec_dot(const float* a, const float* b, int n) {
    float s = 0; for (int i = 0; i < n; i++) s += a[i] * b[i]; return s;
}
static void vec_add(float* a, const float* b, int n) { for (int i = 0; i < n; i++) a[i] += b[i]; }
static void vec_cpy(float* d, const float* s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static void vec_scale(float* a, float s, int n) { for (int i = 0; i < n; i++) a[i] *= s; }

/* ── Dequantize a row from either Q4_0 or F32 ── */

static void deq_row_q4_0(const void* w, float* d, int n);

static void deq_row(const void* w, float* d, int n, int type) {
    if (type == 0) { /* F32 */
        vec_cpy(d, (const float*)w, n);
    } else if (type == 1) { /* F16 */
        const unsigned short* h = (const unsigned short*)w;
        for (int i = 0; i < n; i++) d[i] = fp16_to_fp32(h[i]);
    } else { /* Q4_0 */
        deq_row_q4_0(w, d, n);
    }
}

static void deq_row_q4_0(const void* w, float* d, int n) {
    const block_q4_0* b = (const block_q4_0*)w;
    for (int i = 0; i < n; i++) d[i] = deq_q4_0(&b[i/QK4_0], i%QK4_0);
}

/* ── NN primitives ── */

static void rms_norm(float* o, const float* x, const float* w, int n) {
    float ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float s = 1.0f / fast_sqrtf(ss / n + 1e-5f);
    for (int i = 0; i < n; i++) o[i] = x[i] * s * w[i];
}

static void softmax(float* x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0;
    for (int i = 0; i < n; i++) { x[i] = fast_expf(x[i] - mx); sum += x[i]; }
    float is = 1.0f / sum;
    for (int i = 0; i < n; i++) x[i] *= is;
}

static float silu(float x) { return x / (1.0f + fast_expf(-x)); }

/* ── RoPE ── */
#define MAX_HD 128
static float rope_sin[MAX_HD], rope_cos[MAX_HD];
static int rope_cur_pos = -1, rope_cur_d = 0;
static float rope_cur_fb = 0;

static void rope_init(int pos, int d, float fb) {
    if (pos == rope_cur_pos && d == rope_cur_d && fb == rope_cur_fb) return;
    for (int i = 0; i < d; i += 2) {
        float th = (float)pos * fast_powf(fb, -(float)i / (float)d);
        rope_sin[i] = fast_sinf(th); rope_cos[i] = fast_cosf(th);
        rope_sin[i+1] = rope_sin[i]; rope_cos[i+1] = rope_cos[i];
    }
    rope_cur_pos = pos; rope_cur_d = d; rope_cur_fb = fb;
}

static void rope_apply(float* q, float* k, int H, int KVH, int D) {
    for (int h = 0; h < H; h++) {
        float* qh = q + h * D;
        for (int i = 0; i < D; i += 2) {
            float q0 = qh[i], q1 = qh[i+1];
            qh[i] = q0 * rope_cos[i] - q1 * rope_sin[i];
            qh[i+1] = q0 * rope_sin[i] + q1 * rope_cos[i];
        }
    }
    for (int h = 0; h < KVH; h++) {
        float* kh = k + h * D;
        for (int i = 0; i < D; i += 2) {
            float k0 = kh[i], k1 = kh[i+1];
            kh[i] = k0 * rope_cos[i] - k1 * rope_sin[i];
            kh[i+1] = k0 * rope_sin[i] + k1 * rope_cos[i];
        }
    }
}

/* ── Sampling ── */

int sample(float* logits, int n_vocab, float temp, float top_p, int top_k) {
    if (temp < 0.01f) {
        int best = 0;
        for (int i = 1; i < n_vocab; i++) if (logits[i] > logits[best]) best = i;
        return best;
    }
    vec_scale(logits, 1.0f / temp, n_vocab);
    softmax(logits, n_vocab);
    int idx[128]; float val[128];
    int n = top_k < n_vocab ? top_k : n_vocab;
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) { idx[i] = i; val[i] = logits[i]; }
    for (int i = n; i < n_vocab; i++) {
        int mj = 0;
        for (int j = 1; j < n; j++) if (val[j] < val[mj]) mj = j;
        if (logits[i] > val[mj]) { val[mj] = logits[i]; idx[mj] = i; }
    }
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            if (val[j] > val[i]) {
                float tv = val[i]; val[i] = val[j]; val[j] = tv;
                int ti = idx[i]; idx[i] = idx[j]; idx[j] = ti;
            }
    float cum = 0;
    for (int i = 0; i < n; i++) { cum += val[i]; if (cum >= top_p) { n = i+1; break; } }
    return idx[(unsigned)rdtsc_int() % n];
}

/* ── Tokenizer ── */

/* GPT-2 bytes_to_unicode mapping */
static int byte_unicode_map[256];
static int byte_unicode_init = 0;

static void init_byte_unicode(void) {
    if (byte_unicode_init) return;
    int unsafe[256];
    for (int b = 0; b < 256; b++) unsafe[b] = 1;
    for (int b = 33; b <= 126; b++) unsafe[b] = 0;
    for (int b = 161; b <= 172; b++) unsafe[b] = 0;
    for (int b = 174; b <= 255; b++) unsafe[b] = 0;
    int next = 256;
    for (int b = 0; b < 256; b++)
        byte_unicode_map[b] = unsafe[b] ? next++ : b;
    byte_unicode_init = 1;
}

static void model_load_tokenizer(LlmModel* m) {
    init_byte_unicode();
    for (int b = 0; b < 256; b++) m->byte_to_tok[b] = -1;

    /* Walk vocab to build codepoint→token_id table & byte encoding */
    unsigned char* vp = m->vocab_arr_data;
    for (int i = 0; i < m->n_vocab_actual && i < 2000; i++) {
        unsigned long long sl = *(unsigned long long*)vp;
        /* For each byte, check if this token's text matches the UTF-8 encoding of byte_unicode_map[b] */
        for (int b = 0; b < 256; b++) {
            if (m->byte_to_tok[b] >= 0) continue;
            int cp = byte_unicode_map[b];
            /* UTF-8 encode cp into utf8_buf */
            unsigned char ub[4];
            int ul;
            if (cp < 0x80) { ub[0] = cp; ul = 1; }
            else if (cp < 0x800) { ub[0] = 0xC0 | (cp >> 6); ub[1] = 0x80 | (cp & 0x3F); ul = 2; }
            else { ub[0] = 0xE0 | (cp >> 12); ub[1] = 0x80 | ((cp >> 6) & 0x3F); ub[2] = 0x80 | (cp & 0x3F); ul = 3; }
            if (sl == (unsigned long long)ul && memcmp(vp + 8, ub, ul) == 0)
                m->byte_to_tok[b] = i;
        }
        vp += 8 + sl;
    }

    int found = 0;
    for (int b = 0; b < 256; b++) if (m->byte_to_tok[b] >= 0) found++;
    printf("[LLM] Tokenizer: %d/%d byte tokens mapped\n", found, 256);
}

/* Encode: byte-level (no BPE merging for first version) */
TokenArray* encode(LlmModel* m, const char* text) {
    static int buftok[4096];
    static TokenArray r = {buftok, 0, 4096};
    int n = 0, n_byte = 0;
    for (int i = 0; text[i]; i++) {
        unsigned char b = (unsigned char)text[i];
        int tid = m->byte_to_tok[b];
        if (tid >= 0 && n < 4095) { buftok[n++] = tid; }
    }
    r.count = n;
    return &r;
}

/* Decode: look up each token's string in vocab array and concatenate */
char* decode(LlmModel* m, int* tokens, int n_tokens) {
    static char result[4096];
    int rp = 0;
    for (int i = 0; i < n_tokens && rp < 4090; i++) {
        int tid = tokens[i];
        if (tid < 0 || tid >= m->n_vocab_actual) continue;
        unsigned char* p = m->vocab_arr_data;
        for (int j = 0; j < tid; j++) {
            unsigned long long sl = *(unsigned long long*)p;
            p += 8 + sl;
        }
        unsigned long long len = *(unsigned long long*)p;
        const unsigned char* s = p + 8;
        for (unsigned long long k = 0; k < len && rp < 4090; k++)
            result[rp++] = s[k];
    }
    result[rp] = 0;
    return result;
}

/* ── Tensor helpers ── */

static int find_tensor(LlmModel* m, const char* name) {
    for (int i = 0; i < m->tensor_count; i++)
        if (m->tensors[i].name && strcmp(m->tensors[i].name, name) == 0) return i;
    return -1;
}

#define T(m,idx) ((unsigned char*)((m)->mapped_data) + (m)->tensors[idx].offset)
#define TT(m,idx) ((m)->tensors[idx].type)
#define TR(m,idx) ((int)(m)->tensors[idx].rows)
#define TC(m,idx) ((int)(m)->tensors[idx].cols)

/* ── GGUF loader ── */

static int gguf_tsize(int t) {
    if (t <= 1) return 1;
    if (t <= 3) return 2;
    if (t <= 6) return 4;
    if (t >= 10 && t <= 12) return 8;
    return 0;
}

int model_load(LlmModel* m, void* data, unsigned long size) {
    memset(m, 0, sizeof(*m));
    m->mapped_data = data;
    m->mapped_size = size;
    unsigned char* base = (unsigned char*)data;
    if (*(unsigned int*)base != 0x46554747) { printf("[LLM] Bad magic\n"); return -1; }
    unsigned char* c = base + 4;
    unsigned int ver = *(unsigned int*)c; c += 4;
    unsigned long long nt = *(unsigned long long*)c; c += 8;
    unsigned long long nk = *(unsigned long long*)c; c += 8;
    printf("[LLM] GGUF v%u: %llu tensors, %llu KV\n", ver, nt, nk);

    for (unsigned long long i = 0; i < nk && i < 128; i++) {
        unsigned long long kl = *(unsigned long long*)c; c += 8;
        int ki = m->kv_count;
        m->kvs[ki].key = (char*)c; c += kl;
        int t = *(unsigned int*)c; c += 4;
        m->kvs[ki].type = t;
        m->kvs[ki].value = c; m->kvs[ki].arr_len = 0;
        if (t == 8) { unsigned long long sl = *(unsigned long long*)c; c += 8 + sl; }
        else if (t == 9) {
            unsigned int at = *(unsigned int*)c; c += 4;
            unsigned long long al = *(unsigned long long*)c; c += 8;
            m->kvs[ki].arr_len = al; m->kvs[ki].value = c;
            for (unsigned long long j = 0; j < al; j++)
                if (at == 8) { unsigned long long ss = *(unsigned long long*)c; c += 8 + ss; }
                else c += gguf_tsize(at);
        } else c += gguf_tsize(t);
        m->kv_count++;
    }

    for (unsigned long long i = 0; i < nt && m->tensor_count < 256; i++) {
        unsigned int nd = *(unsigned int*)c; c += 4;
        unsigned char qt = *(unsigned char*)c; c += 1;
        unsigned long long nl = *(unsigned long long*)c; c += 8;
        char* tn = (char*)c; c += nl;
        unsigned long long dims[4] = {1,1,1,1};
        for (unsigned int d = 0; d < nd; d++) dims[d] = *(unsigned long long*)c; c += 8;
        unsigned long long off = *(unsigned long long*)c; c += 8;
        int ti = m->tensor_count++;
        m->tensors[ti].type = (int)qt;
        m->tensors[ti].cols = (nd > 0) ? (long long)dims[0] : 1;
        m->tensors[ti].rows = (nd > 1) ? (long long)dims[1] : 1;
        m->tensors[ti].offset = off;
        m->tensors[ti].name = tn;
    }

    while ((unsigned long long)(c - base) % 32 != 0) c++;
    if (m->tensor_count > 0) m->tensors[0].offset = (unsigned long long)(c - base);

    m->config.n_vocab = 151936;
    for (int i = 0; i < m->kv_count; i++) {
        char* k = m->kvs[i].key;
        int t = m->kvs[i].type;
        if (t >= 4 && t <= 6) {
            unsigned int v = *(unsigned int*)m->kvs[i].value;
            if (my_strstr(k, "context_length")) m->config.max_seq_len = (int)v;
            else if (my_strstr(k, "embedding_length")) m->config.n_embd = (int)v;
            else if (my_strstr(k, "block_count")) m->config.n_layer = (int)v;
            else if (my_strstr(k, "head_count") && !my_strstr(k, "head_count_kv")) m->config.n_head = (int)v;
            else if (my_strstr(k, "head_count_kv")) m->config.n_kv_head = (int)v;
            else if (my_strstr(k, "feed_forward_length")) m->config.n_ff = (int)v;
            else if (my_strstr(k, "vocab_size")) m->config.n_vocab = (int)v;
        } else if (t == 6) {
            float v = *(float*)m->kvs[i].value;
            if (my_strstr(k, "rope.freq_base")) m->config.freq_base = v;
        }
    }
#define DF(f, dv) if (m->config.f == 0) m->config.f = dv
    DF(n_embd, 896); DF(n_layer, 24); DF(n_head, 14);
    DF(n_kv_head, 2); DF(n_ff, 4864); DF(max_seq_len, 2048);
#undef DF
    if (m->config.freq_base == 0.0f) m->config.freq_base = 1000000.0f;
    m->config.rope_dim = m->config.n_embd / m->config.n_head;
    printf("[LLM] v=%d e=%d l=%d h=%d kvh=%d ff=%d max=%d rd=%d fb=%g\n",
           m->config.n_vocab, m->config.n_embd, m->config.n_layer,
           m->config.n_head, m->config.n_kv_head, m->config.n_ff,
           m->config.max_seq_len, m->config.rope_dim, m->config.freq_base);

    for (int i = 0; i < m->kv_count; i++) {
        char* k = m->kvs[i].key;
        if (m->kvs[i].type != 9) continue;
        if (my_strstr(k, "tokenizer.ggml.tokens")) {
            m->vocab_arr_data = (unsigned char*)m->kvs[i].value;
            m->vocab_arr_len = m->kvs[i].arr_len;
        } else if (my_strstr(k, "tokenizer.ggml.merges")) {
            m->merge_arr_data = (unsigned char*)m->kvs[i].value;
            m->merge_arr_len = m->kvs[i].arr_len;
        } else if (my_strstr(k, "tokenizer.ggml.scores")) {
            m->score_arr_data = (unsigned char*)m->kvs[i].value;
        } else if (my_strstr(k, "tokenizer.ggml.token_type")) {
            m->type_arr_data = (unsigned char*)m->kvs[i].value;
        }
    }
    m->n_vocab_actual = (int)m->vocab_arr_len;
    printf("[LLM] Tokenizer: %d tokens, %llu merges\n",
           (int)m->vocab_arr_len, m->merge_arr_len);
    if (m->vocab_arr_data) model_load_tokenizer(m);
    return 0;
}

/* ── Quant matmul (dispatch by format) ── */

static void matmul(float* dst, const void* w, const float* x, int r, int c, int type) {
    if (type == 0) {
        const float* f = (const float*)w;
        for (int i = 0; i < r; i++) dst[i] = vec_dot(f + (unsigned long)i * c, x, c);
    } else {
        unsigned long rb = q4_0_rb(c);
        for (int i = 0; i < r; i++)
            dst[i] = dot_q4_0((const char*)w + i * rb, x, c);
    }
}

/* ── KV cache ── */
#define KV_MAX 128

static float kvk[24][KV_MAX][128];
static float kvv[24][KV_MAX][128];

/* ── Forward pass ── */

int model_forward(LlmModel* m, int* tokens, int n_tokens, int n_predict, float* logits) {
    int tt = rdtsc_int();
    int N = m->config.n_embd, L = m->config.n_layer, H = m->config.n_head;
    int K = m->config.n_kv_head, D = m->config.rope_dim;
    int FF = m->config.n_ff, V = m->config.n_vocab, HD = N / H;

    int te_i = find_tensor(m, "token_embd.weight");
    int on_i = find_tensor(m, "output_norm.weight");
    int ow_i = find_tensor(m, "output.weight");
    if (ow_i < 0) ow_i = te_i;

    if (te_i < 0 || on_i < 0) { printf("[LLM] Missing essential tensors\n"); return -1; }

    struct { int an, aq, ak, av, ao, fn, fg, fu, fd; } l[24];
    for (int i = 0; i < L && i < 24; i++) {
        char nb[64];
        int pos = 0;
        const char* pre = "blk.";
        while (*pre) nb[pos++] = *pre++;
        if (i >= 100) nb[pos++] = '0' + i/100;
        if (i >= 10) nb[pos++] = '0' + (i/10)%10;
        nb[pos++] = '0' + i%10;
        nb[pos] = 0;
        int base_len = pos;
        const char* sfx[] = {".attn_norm.weight", ".attn_q.weight", ".attn_k.weight",
                             ".attn_v.weight", ".attn_output.weight", ".ffn_norm.weight",
                             ".ffn_gate.weight", ".ffn_up.weight", ".ffn_down.weight"};
        int* out[] = {&l[i].an, &l[i].aq, &l[i].ak, &l[i].av, &l[i].ao,
                      &l[i].fn, &l[i].fg, &l[i].fu, &l[i].fd};
        for (int s = 0; s < 9; s++) {
            int sp = base_len; const char* sf = sfx[s];
            while (*sf) nb[sp++] = *sf++;
            nb[sp] = 0;
            *out[s] = find_tensor(m, nb);
        }
    }

    int steps = n_tokens + n_predict;
    if (steps > KV_MAX) steps = KV_MAX;

    float x[2048], x_norm[2048], q[2048], k[128], v[128];
    float gate[8192], up[8192], att_out[2048], scores[KV_MAX];

    for (int step = 0; step < steps; step++) {
        int is_pr = step < n_tokens;
        int tok = is_pr ? tokens[step] : 0;
        int pos = is_pr ? step : m->cache_tokens;

        if (!is_pr) {
            tok = sample(logits, V, 0.7f, 0.9f, 40);
            tokens[step] = tok;
            m->cache_tokens++;
        }

        /* --- Embedding --- */
        unsigned long rb = q4_0_rb(TC(m, te_i));
        int te_type = TT(m, te_i);
        if (te_type == 0) {
            vec_cpy(x, (const float*)T(m, te_i) + (unsigned long)tok * TC(m, te_i), N);
        } else {
            const char* te_row = (const char*)T(m, te_i) + (unsigned long)tok * rb;
            deq_row_q4_0(te_row, x, N);
        }

        /* --- Layers --- */
        for (int li = 0; li < L && li < 24; li++) {
            /* Attention norm */
            if (l[li].an >= 0) {
                float w[2048]; deq_row(T(m, l[li].an), w, N, TT(m, l[li].an));
                rms_norm(x_norm, x, w, N);
            } else vec_cpy(x_norm, x, N);

            /* QKV */
            matmul(q, T(m, l[li].aq), x_norm, N, N, TT(m, l[li].aq));
            matmul(k, T(m, l[li].ak), x_norm, K * D, N, TT(m, l[li].ak));
            matmul(v, T(m, l[li].av), x_norm, K * D, N, TT(m, l[li].av));

            /* RoPE */
            rope_init(pos, D, m->config.freq_base);
            rope_apply(q, k, H, K, D);

            /* KV cache */
            if (pos < KV_MAX) {
                vec_cpy(kvk[li][pos], k, K * D);
                vec_cpy(kvv[li][pos], v, K * D);
            }

            /* Attention */
            int kv_len = pos < KV_MAX ? pos + 1 : KV_MAX;
            int gs = H / K; /* group size for GQA */
            for (int h = 0; h < H; h++) {
                float* qh = q + h * HD;
                int kh_idx = h / gs;
                for (int t = 0; t < kv_len; t++)
                    scores[t] = vec_dot(qh, kvk[li][t] + kh_idx * HD, HD) / fast_sqrtf((float)HD);
                softmax(scores, kv_len);
                for (int i = 0; i < HD; i++) {
                    float s = 0;
                    for (int t = 0; t < kv_len; t++)
                        s += scores[t] * kvv[li][t][kh_idx * HD + i];
                    att_out[h * HD + i] = s;
                }
            }

            /* Attention output projection + residual */
            matmul(x_norm, T(m, l[li].ao), att_out, N, N, TT(m, l[li].ao));
            vec_add(x, x_norm, N);

            /* FFN norm */
            if (l[li].fn >= 0) {
                float w[2048]; deq_row(T(m, l[li].fn), w, N, TT(m, l[li].fn));
                rms_norm(x_norm, x, w, N);
            } else vec_cpy(x_norm, x, N);

            /* SwiGLU */
            matmul(gate, T(m, l[li].fg), x_norm, FF, N, TT(m, l[li].fg));
            matmul(up, T(m, l[li].fu), x_norm, FF, N, TT(m, l[li].fu));
            for (int i = 0; i < FF; i++) gate[i] = silu(gate[i]) * up[i];
            matmul(x_norm, T(m, l[li].fd), gate, N, FF, TT(m, l[li].fd));
            vec_add(x, x_norm, N);
        }

        /* Final norm */
        {
            float w[2048]; deq_row(T(m, on_i), w, N, TT(m, on_i));
            rms_norm(x, x, w, N);
        }

        /* Logits */
        matmul(logits, T(m, ow_i), x, V, N, TT(m, ow_i));
    }

    int t1 = rdtsc_int();
    printf("[LLM] %d steps in %d cycles (%.1f ms)\n", steps, t1 - tt, (float)(t1 - tt) / 2.6e6f);
    return 0;
}

void model_free(LlmModel* m) { memset(m, 0, sizeof(*m)); }

/* ── Agent loop ── */

void agent_loop(LlmModel* m, AgentTools* tools, const char* sys) {
    char input[1024];
    (void)sys; (void)tools;
    printf("\n=== AI Agent Ready ===\n");
    printf("Type 'exit' to quit.\n\n");

    while (1) {
        printf("> ");
        int pos = 0;
        while (1) {
            int c = getchar();
            if (c == '\n' || c == '\r') break;
            if ((c == 127 || c == '\b') && pos > 0) { pos--; putchar('\b'); putchar(' '); putchar('\b'); }
            else if (c >= 32 && pos < 1022) { input[pos++] = c; putchar(c); }
        }
        input[pos] = 0; putchar('\n');
        if (strcmp(input, "exit") == 0) break;
        printf("[Agent] Request: %s\n", input);
        printf("[Agent] (Inference engine would generate response)\n");
    }
}

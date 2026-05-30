#ifndef LLM_H
#define LLM_H

#define QK4_0 32
typedef struct { unsigned short d; unsigned char qs[QK4_0/2]; } block_q4_0;
#define QK4_K 32
typedef struct { unsigned short d; unsigned short dmin; unsigned char scales[6]; unsigned char qs[QK4_K/2]; } block_q4_K;

typedef struct {
    int n_vocab, n_embd, n_layer, n_head, n_kv_head, n_ff, max_seq_len;
    int has_rope, rope_dim;
    int tokenizer_type;
    float freq_base;
} ModelConfig;

typedef struct {
    ModelConfig config;
    void* mapped_data;
    unsigned long mapped_size;

    struct {
        int type;
        unsigned long long rows, cols;
        unsigned long long offset;
        char* name;
    } tensors[256];
    int tensor_count;

    int kv_count;
    struct { char* key; int type; void* value; unsigned long long arr_len; } kvs[128];

    int n_vocab_actual;
    unsigned char* vocab_arr_data;
    unsigned char* merge_arr_data;
    unsigned char* score_arr_data;
    unsigned char* type_arr_data;
    unsigned long long vocab_arr_len;
    unsigned long long merge_arr_len;
    int byte_to_tok[256];

    float* k_cache;
    float* v_cache;
    int cache_tokens;
} LlmModel;

typedef struct {
    int* tokens;
    int count;
    int capacity;
} TokenArray;

TokenArray* encode(LlmModel* m, const char* text);
char* decode(LlmModel* m, int* tokens, int n_tokens);
int sample(float* logits, int n_vocab, float temp, float top_p, int top_k);
static float fast_expf(float x);
static float fast_powf(float a, float b);
static float fast_sinf(float x);
static float fast_cosf(float x);
static float fast_tanhf(float x);
static float fast_sqrtf(float x);

int model_load(LlmModel* m, void* data, unsigned long size);
int model_forward(LlmModel* m, int* tokens, int n_tokens, int n_predict, float* logits_out);
void model_free(LlmModel* m);

typedef struct {
    char* (*read_file)(const char* path);
    int (*write_file)(const char* path, const char* content);
    int (*spawn)(const char* path);
    char* (*run_cmd)(const char* cmd);
} AgentTools;

void agent_loop(LlmModel* m, AgentTools* tools, const char* system_prompt);

#endif

// wp2_gen.cpp -- WP2 measurement/gate driver. Minimal llama.cpp generation
// (greedy, deterministic) with a per-layer eval callback that fires the
// residctl pager's consumption signal, plus timing + I/O accounting.
//
// arm A  : run WITHOUT RESIDCTL_CONFIG in the env -> plain mmap, no pager.
// arms C/D/E : RESIDCTL_CONFIG=<file> -> llama-mmap.cpp routes the model
//              file through residctl_llama_mmap (this executable provides it).
#include "llama.h"
#include "ggml.h"
#include "residctl_llama.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <ctime>

static uint64_t proc_io_read_bytes() {
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) return 0;
    char line[128]; uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long x;
        if (sscanf(line, "read_bytes: %llu", &x) == 1) { v = x; break; }
    }
    fclose(f);
    return v;
}

struct cb_state {
    int last_layer = -1;
    long transitions = 0;
};

// Compute-node names are "<opname>-<il>" (llama-context.cpp graph_get_cb:
// ggml_format_name(cur, "%s-%d", name, il)). Parse the trailing -<digits>.
static int node_layer(const char * nm) {
    size_t n = strlen(nm);
    if (n < 2) return -1;
    size_t i = n;
    while (i > 0 && nm[i - 1] >= '0' && nm[i - 1] <= '9') i--;
    if (i == n || i == 0 || nm[i - 1] != '-') return -1;
    return atoi(nm + i);
}

#include <set>
static std::set<std::string> g_seen_names;
static bool eval_cb(struct ggml_tensor * t, bool ask, void * ud) {
    if (getenv("WP2_DBG_NAMES")) {
        std::string k = std::string(ask?"ask  ":"eval ") + t->name;
        if (g_seen_names.insert(k).second) fprintf(stderr, "NODE %s\n", k.c_str());
    }
    if (ask) return true;
    auto * st = (cb_state *) ud;
    const char * nm = t->name;
    int layer = node_layer(nm);
    if (layer >= 0) {
        if (layer != st->last_layer) {
            st->last_layer = layer;
            st->transitions++;
            residctl_llama_notify_layer(layer);
        }
    } else if (!strcmp(nm, "inp_embd")) {
        st->last_layer = -1;
        residctl_llama_notify_role(0);
    } else if (!strcmp(nm, "result_norm")) {
        residctl_llama_notify_role(1);
    } else if (!strcmp(nm, "result_output")) {
        residctl_llama_notify_role(2);
    }
    return true;
}

int main(int argc, char ** argv) {
    std::string model_path, prompt = "The history of computing began";
    int n_predict = 64, n_threads = 8;
    std::string dump_tokens_path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-m" && i + 1 < argc) model_path = argv[++i];
        else if (a == "-n" && i + 1 < argc) n_predict = atoi(argv[++i]);
        else if (a == "-p" && i + 1 < argc) prompt = argv[++i];
        else if (a == "-t" && i + 1 < argc) n_threads = atoi(argv[++i]);
        else if (a == "--dump-tokens" && i + 1 < argc) dump_tokens_path = argv[++i];
        else if (a == "--inventory" && i + 1 < argc) { /* handled after mmap */ }
    }
    if (model_path.empty()) { fprintf(stderr, "usage: %s -m model.gguf [-n N] [-p prompt] [-t threads] [--dump-tokens f]\n", argv[0]); return 2; }

    ggml_time_init();
    ggml_backend_load_all();

    const bool residctl = getenv("RESIDCTL_CONFIG") != nullptr;

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    mp.use_extra_bufts = false;

    uint64_t io0_load = proc_io_read_bytes();
    int64_t t_load0 = ggml_time_us();
    llama_model * model = llama_model_load_from_file(model_path.c_str(), mp);
    int64_t t_load1 = ggml_time_us();
    if (!model) { fprintf(stderr, "wp2_gen: model load failed\n"); return 1; }

    // Optional: dump the inventory that residctl_llama parsed.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--inventory") && i + 1 < argc && residctl)
            residctl_llama_write_inventory(argv[i + 1]);

    const llama_vocab * vocab = llama_model_get_vocab(model);
    int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    std::vector<llama_token> ptoks(n_prompt);
    llama_tokenize(vocab, prompt.c_str(), prompt.size(), ptoks.data(), ptoks.size(), true, true);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = n_prompt + n_predict + 8;
    cp.n_batch = std::max(32, n_prompt);
    cp.n_threads = n_threads;
    cp.n_threads_batch = n_threads;
    cp.no_perf = true;
    cb_state st;
    cp.cb_eval = eval_cb;
    cp.cb_eval_user_data = &st;

    llama_context * ctx = llama_init_from_model(model, cp);
    if (!ctx) { fprintf(stderr, "wp2_gen: ctx init failed\n"); return 1; }

    auto sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler * smpl = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    std::vector<llama_token> out_tokens;
    llama_batch batch = llama_batch_get_one(ptoks.data(), ptoks.size());

    uint64_t io0 = proc_io_read_bytes();
    int64_t t0 = ggml_time_us();
    int64_t t_ttft = 0;
    std::vector<int64_t> tok_us;
    int64_t t_prev = t0;

    int n_decoded = 0;
    for (int n_pos = 0; n_pos + batch.n_tokens < n_prompt + n_predict; ) {
        if (llama_decode(ctx, batch)) { fprintf(stderr, "wp2_gen: decode failed\n"); return 1; }
        n_pos += batch.n_tokens;
        llama_token id = llama_sampler_sample(smpl, ctx, -1);
        int64_t now = ggml_time_us();
        if (t_ttft == 0) t_ttft = now - t0;
        else tok_us.push_back(now - t_prev);
        t_prev = now;
        if (llama_vocab_is_eog(vocab, id)) break;
        out_tokens.push_back(id);
        n_decoded++;
        batch = llama_batch_get_one(&out_tokens.back(), 1);
    }
    int64_t t1 = ggml_time_us();
    uint64_t io1 = proc_io_read_bytes();

    // p99 inter-token latency
    double p99 = 0.0;
    if (!tok_us.empty()) {
        std::vector<int64_t> v = tok_us;
        std::sort(v.begin(), v.end());
        size_t idx = (size_t)(0.99 * (v.size() - 1) + 0.5);
        p99 = v[idx] / 1000.0;
    }
    double wall_s = (t1 - t0) / 1e6;
    double tps = n_decoded / wall_s;

    // token sequence (for the correctness gate)
    std::string toks;
    for (size_t i = 0; i < out_tokens.size(); i++) { toks += std::to_string(out_tokens[i]); if (i + 1 < out_tokens.size()) toks += " "; }
    printf("WP2_TOKENS,%s\n", toks.c_str());
    if (!dump_tokens_path.empty()) {
        FILE * f = fopen(dump_tokens_path.c_str(), "w");
        if (f) { fprintf(f, "%s\n", toks.c_str()); fclose(f); }
    }

    printf("WP2_CSV,arm_mode=%s,n_prompt=%d,n_decoded=%d,layer_transitions=%ld,"
           "load_s=%.3f,ttft_ms=%.2f,p99_inter_ms=%.3f,wall_s=%.3f,tokens_s=%.3f,"
           "io_read_bytes_load=%llu,io_read_bytes_gen=%llu\n",
           residctl ? "residctl" : "mmap", n_prompt, n_decoded, st.transitions,
           (t_load1 - t_load0) / 1e6, t_ttft / 1000.0, p99, wall_s, tps,
           (unsigned long long)(io0 - io0_load), (unsigned long long)(io1 - io0));

    // maxrss
    { FILE * f = fopen("/proc/self/status", "r"); char l[128];
      if (f) { while (fgets(l, sizeof l, f)) { unsigned long kb; if (sscanf(l, "VmHWM: %lu kB", &kb) == 1) { printf("WP2_RSS,vmhwm_kb=%lu\n", kb); break; } } fclose(f); } }

    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);

    residctl_llama_teardown();  // no-op if the pager was never started
    return 0;
}

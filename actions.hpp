#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <stdio.h>
#include <cmath>

#include "llama.h"
#include "json.hpp"
#include "common.h"

/**
 * CCAMA project - A low-level llama.cpp API via JSON
 * https://github.com/ngxson/ccama
 */

using json = nlohmann::json;

#define LOG_JSON(str, ...)                                \
  {                                                       \
    char output[1024];                                    \
    sprintf(output, str.c_str(), __VA_ARGS__);            \
    send_response(json{{"debug" : std::string(output)}}); \
  }

#define ACTION(name)          \
  if (action == #name)        \
  {                           \
    action_##name(app, body); \
    continue;                 \
  }

struct app_t
{
  llama_model *model;
  llama_context *ctx;
  struct llama_sampling_context *ctx_sampling = nullptr;
  llama_batch batch = llama_batch_init(512, 0, 1);
  std::vector<llama_token> tokens;
  // group attention
  int32_t ga_i = 0; // group-attention state
  int32_t ga_n = 0; // group-attention factor
  int32_t ga_w = 0; // group-attention width
  int32_t n_past_self_extension = 0;
};

inline void send_response(json data)
{
  std::cout << data.dump() << "\n";
}

inline std::vector<unsigned int> convert_string_to_int_arr(std::string &input)
{
  std::vector<unsigned int> output;
  unsigned char *input_ptr = (unsigned char *)input.data();
  output.resize(input.length());
  for (size_t i = 0; i < input.length(); i++)
  {
    output[i] = static_cast<unsigned int>(input_ptr[i]);
  }
  return std::move(output);
}

inline static ggml_type kv_cache_type_from_str(const std::string &s)
{
  if (s == "f32")
    return GGML_TYPE_F32;
  if (s == "f16")
    return GGML_TYPE_F16;
  if (s == "q8_0")
    return GGML_TYPE_Q8_0;
  if (s == "q4_0")
    return GGML_TYPE_Q4_0;
  if (s == "q4_1")
    return GGML_TYPE_Q4_1;
  if (s == "q5_0")
    return GGML_TYPE_Q5_0;
  if (s == "q5_1")
    return GGML_TYPE_Q5_1;
  throw std::runtime_error("Invalid cache type: " + s);
}

inline static llama_pooling_type pooling_type_from_str(const std::string &s)
{
  if (s == "LLAMA_POOLING_TYPE_UNSPECIFIED")
    return LLAMA_POOLING_TYPE_UNSPECIFIED;
  if (s == "LLAMA_POOLING_TYPE_NONE")
    return LLAMA_POOLING_TYPE_NONE;
  if (s == "LLAMA_POOLING_TYPE_MEAN")
    return LLAMA_POOLING_TYPE_MEAN;
  if (s == "LLAMA_POOLING_TYPE_CLS")
    return LLAMA_POOLING_TYPE_CLS;
  throw std::runtime_error("Invalid pooling type: " + s);
}

inline static llama_rope_scaling_type rope_scaling_type_from_str(const std::string &s)
{
  if (s == "LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED")
    return LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED;
  if (s == "LLAMA_ROPE_SCALING_TYPE_NONE")
    return LLAMA_ROPE_SCALING_TYPE_NONE;
  if (s == "LLAMA_ROPE_SCALING_TYPE_LINEAR")
    return LLAMA_ROPE_SCALING_TYPE_LINEAR;
  if (s == "LLAMA_ROPE_SCALING_TYPE_YARN")
    return LLAMA_ROPE_SCALING_TYPE_YARN;
  throw std::runtime_error("Invalid RoPE scaling type: " + s);
}

//////////////////////////////////////////
//////////////////////////////////////////
//////////////////////////////////////////

json action_load(app_t &app, json &body)
{
  std::string model_path = body["model_path"];
  auto mparams = llama_model_default_params();
  if (body.contains("use_mmap"))
    mparams.use_mmap = body["use_mmap"];
  if (body.contains("use_mlock"))
    mparams.use_mlock = body["use_mlock"];
  if (body.contains("n_gpu_layers"))
    mparams.n_gpu_layers = body["n_gpu_layers"];
  auto cparams = llama_context_default_params();
  cparams.seed = body["seed"];
  cparams.n_ctx = body["n_ctx"];
  cparams.n_threads = body["n_threads"];
  cparams.n_threads_batch = cparams.n_threads;
  if (body.contains("embeddings"))
    cparams.embeddings = body["embeddings"];
  if (body.contains("offload_kqv"))
    cparams.offload_kqv = body["offload_kqv"];
  if (body.contains("n_batch"))
    cparams.n_batch = body["n_batch"];
  if (body.contains("n_seq_max"))
    cparams.n_seq_max = body["n_seq_max"];
  if (body.contains("pooling_type"))
    cparams.pooling_type = pooling_type_from_str(body["pooling_type"]);
  // context extending: https://github.com/ggerganov/llama.cpp/pull/2054
  if (body.contains("rope_scaling_type"))
    cparams.rope_scaling_type = rope_scaling_type_from_str(body["rope_scaling_type"]);
  if (body.contains("rope_freq_base"))
    cparams.rope_freq_base = body["rope_freq_base"];
  if (body.contains("rope_freq_scale"))
    cparams.rope_freq_scale = body["rope_freq_scale"];
  if (body.contains("yarn_ext_factor"))
    cparams.yarn_ext_factor = body["yarn_ext_factor"];
  if (body.contains("yarn_attn_factor"))
    cparams.yarn_attn_factor = body["yarn_attn_factor"];
  if (body.contains("yarn_beta_fast"))
    cparams.yarn_beta_fast = body["yarn_beta_fast"];
  if (body.contains("yarn_beta_slow"))
    cparams.yarn_beta_slow = body["yarn_beta_slow"];
  if (body.contains("yarn_orig_ctx"))
    cparams.yarn_orig_ctx = body["yarn_orig_ctx"];
  // group attention
  if (body.contains("grp_attn_n"))
    app.ga_n = body["grp_attn_n"];
  if (body.contains("grp_attn_w"))
    app.ga_w = body["grp_attn_w"];
  // optimizations
  if (body.contains("cache_type_k"))
    cparams.type_k = kv_cache_type_from_str(body["cache_type_k"]);
  if (body.contains("cache_type_v"))
    cparams.type_k = kv_cache_type_from_str(body["cache_type_v"]);
  app.model = llama_load_model_from_file(model_path.c_str(), mparams);
  app.ctx = llama_new_context_with_model(app.model, cparams);
  llama_batch_free(app.batch);
  app.batch = llama_batch_init(cparams.n_batch, 0, 1);
  return json{
      {"success", true},
      {"token_bos", llama_token_bos(app.model)},
      {"token_eos", llama_token_eos(app.model)},
  };
}

json action_sampling_init(app_t &app, json &body)
{
  // sampling
  llama_sampling_params sparams;
  if (body.contains("mirostat"))
    sparams.mirostat = body["mirostat"];
  if (body.contains("mirostat_tau"))
    sparams.mirostat_tau = body["mirostat_tau"];
  if (body.contains("mirostat_eta"))
    sparams.mirostat_eta = body["mirostat_eta"];
  if (body.contains("temp"))
    sparams.temp = body["temp"];
  if (body.contains("top_p"))
    sparams.top_p = body["top_p"];
  if (body.contains("top_k"))
    sparams.top_k = body["top_k"];
  if (body.contains("penalty_last_n"))
    sparams.penalty_last_n = body["penalty_last_n"];
  if (body.contains("penalty_repeat"))
    sparams.penalty_repeat = body["penalty_repeat"];
  if (body.contains("penalty_freq"))
    sparams.penalty_freq = body["penalty_freq"];
  if (body.contains("penalty_present"))
    sparams.penalty_present = body["penalty_present"];
  if (body.contains("penalize_nl"))
    sparams.penalize_nl = body["penalize_nl"];
  if (body.contains("dynatemp_range"))
    sparams.dynatemp_range = body["dynatemp_range"];
  if (body.contains("dynatemp_exponent"))
    sparams.dynatemp_exponent = body["dynatemp_exponent"];
  // if (body.contains("samplers_sequence"))
  //   sparams.samplers_sequence = body["samplers_sequence"];
  if (body.contains("grammar"))
    sparams.grammar = body["grammar"];
  if (body.contains("n_prev"))
    sparams.n_prev = body["n_prev"];
  if (body.contains("n_probs"))
    sparams.n_probs = body["n_probs"];
  if (body.contains("min_p"))
    sparams.min_p = body["min_p"];
  if (body.contains("tfs_z"))
    sparams.tfs_z = body["tfs_z"];
  if (body.contains("typical_p"))
    sparams.typical_p = body["typical_p"];
  // logit bias
  if (body.contains("logit_bias"))
  {
    std::vector<json> logit_bias = body["logit_bias"];
    for (json &item : logit_bias)
    {
      llama_token token = item["token"];
      float bias = item["bias"];
      sparams.logit_bias[token] = bias;
    }
  }
  // maybe free before creating a new one
  if (app.ctx_sampling != nullptr)
  {
    llama_sampling_free(app.ctx_sampling);
  }
  app.ctx_sampling = llama_sampling_init(sparams);
  if (body.contains("tokens"))
  {
    std::vector<llama_token> tokens = body["tokens"];
    for (auto id : tokens)
    {
      llama_sampling_accept(app.ctx_sampling, app.ctx, id, false);
    }
  }
  return json{{"success", true}};
}

// get map token ID to vocab (be careful, it is slow!)
json action_get_vocab(app_t &app, json &body)
{
  int32_t max_tokens = llama_n_vocab(app.model);
  std::vector<std::vector<unsigned int> > vocab(max_tokens);
  for (int32_t id = 0; id < max_tokens; id++)
  {
    std::string token_as_str = llama_token_to_piece(app.ctx, id);
    vocab[id] = convert_string_to_int_arr(token_as_str);
  }
  return json{
      {"success", true},
      {"vocab", vocab},
  };
}

// lookup single token (also be able to check if it exists or not)
json action_lookup_token(app_t &app, json &body)
{
  std::string piece = body["piece"];
  int32_t max_tokens = llama_n_vocab(app.model);
  for (int32_t id = 0; id < max_tokens; id++)
  {
    std::string token_as_str = llama_token_to_piece(app.ctx, id);
    if (token_as_str == piece)
    {
      return json{
          {"success", true},
          {"token", id},
      };
    }
  }
  // not found
  return json{{"success", false}};
}

// tokenize an input string
json action_tokenize(app_t &app, json &body)
{
  std::string text = body["text"];
  bool special = body.contains("special");
  std::vector<llama_token> tokens_list;
  tokens_list = ::llama_tokenize(app.model, text, false, special);
  return json{
      {"success", true},
      {"tokens", tokens_list},
  };
}

// detokenize a list of tokens
json action_detokenize(app_t &app, json &body)
{
  std::vector<llama_token> tokens = body["tokens"];
  std::stringstream output;
  for (auto id : tokens)
  {
    output << llama_token_to_piece(app.ctx, id);
  }
  std::string parsed_str = output.str();
  return json{
      {"success", true},
      {"buffer", convert_string_to_int_arr(parsed_str)},
  };
}

// decode an array of tokens
json action_decode(app_t &app, json &body)
{
  std::vector<llama_token> tokens_list = body["tokens"];
  bool skip_logits = body.contains("skip_logits");
  /*bool grp_attn_enabled = app.ga_n > 1;
  if (grp_attn_enabled)
  {
    group_attention_shift_context(app);
  }*/
  size_t i = 0;
  llama_batch_clear(app.batch);
  for (auto id : tokens_list)
  {
    bool grp_attn_enabled = false; // TODO: maybe remove grp_attn
    int32_t n_past = grp_attn_enabled
                         ? app.n_past_self_extension
                         : app.tokens.size();
    llama_batch_add(app.batch, id, n_past, {0}, false);
    app.tokens.push_back(id);
    i++;
    app.n_past_self_extension++;
  }
  // llama_decode will output logits only for the last token of the prompt
  if (!skip_logits)
  {
    app.batch.logits[app.batch.n_tokens - 1] = true;
  }
  if (llama_decode(app.ctx, app.batch) != 0)
  {
    return json{{"error", "llama_decode failed, maybe n_batch is too small?"}};
  }
  else
  {
    return json{
        {"success", true},
        {"n_past", app.tokens.size()},
    };
  }
}

// decode the current logits and sample the new token
json action_sampling_sample(app_t &app, json &body)
{
  int32_t idx = app.batch.n_tokens - 1;
  const llama_token new_token_id = llama_sampling_sample(app.ctx_sampling, app.ctx, NULL, idx);
  std::string piece = llama_token_to_piece(app.ctx, new_token_id);
  return json{
      {"success", true},
      {"piece", convert_string_to_int_arr(piece)},
      {"token", new_token_id},
  };
}

// accept this token
json action_sampling_accept(app_t &app, json &body)
{
  std::vector<llama_token> tokens_list = body["tokens"];
  for (auto id : tokens_list)
  {
    llama_sampling_accept(app.ctx_sampling, app.ctx, id, false);
  }
  return json{{"success", true}};
}

// get softmax-ed probability of logits, can be used for custom sampling. The output is always sorted
json action_get_logits(app_t &app, json &body)
{
  int top_k = body["top_k"]; // if is -1, we take all logits (will be slow!)
  int32_t idx = app.batch.n_tokens - 1;
  float *logits = llama_get_logits_ith(app.ctx, idx);
  int32_t n_vocab = llama_n_vocab(app.model);
  auto sort_fn = [](llama_token_data &a, llama_token_data &b) -> bool
  {
    return b.logit < a.logit;
  };
  // get all candidates and sort
  std::vector<llama_token_data> candidates;
  candidates.reserve(n_vocab);
  float sum = 0.0f; // for softmax
  for (llama_token token_id = 0; token_id < n_vocab; token_id++)
  {
    float exp_val = exp(logits[token_id]);
    candidates.emplace_back(llama_token_data{token_id, logits[token_id], exp_val});
    sum += exp_val;
  }
  for (auto &c : candidates)
  {
    c.p /= sum; // calculate softmax
  }
  std::sort(candidates.begin(), candidates.end(), sort_fn);
  if (top_k >= 0)
  {
    candidates.erase(candidates.begin() + top_k, candidates.end());
  }
  // convert response to json
  std::vector<json> output;
  output.reserve(candidates.size());
  for (auto &c : candidates)
  {
    output.emplace_back(json{c.id, c.p});
  }
  return json{
      {"success", true},
      {"logits", output},
  };
}

// get embeddings, this will call action_decode internally
json action_embeddings(app_t &app, json &body)
{
  std::vector<llama_token> tokens_list = body["tokens"];
  // allocate output
  const int n_embd = llama_n_embd(app.model);
  std::vector<float> embeddings(n_embd, 0); // single seq
  float *out = embeddings.data();
  // decode
  json req = json{{"tokens", tokens_list}};
  json res = action_decode(app, req);
  if (res.contains("error"))
  {
    return res;
  }
  int32_t idx = app.batch.n_tokens - 1;
  const float *embd = llama_get_embeddings_seq(app.ctx, 0);
  if (embd == NULL)
  {
    embd = llama_get_embeddings_ith(app.ctx, idx);
    if (embd == NULL)
    {
      fprintf(stderr, "%s: failed to get embeddings for token %d\n", __func__, idx);
      return json{{"error", "failed to get embeddings"}};
    }
  }
  llama_embd_normalize(embd, out, n_embd);
  return json{
      {"success", true},
      {"embeddings", embeddings},
  };
}

// remove tokens in kv, for context-shifting
json action_kv_remove(app_t &app, json &body)
{
  const int n_keep = body["n_keep"];
  const int n_discard = body["n_discard"];
  const int n_past = app.tokens.size();
  llama_kv_cache_seq_rm(app.ctx, 0, n_keep, n_keep + n_discard);
  llama_kv_cache_seq_add(app.ctx, 0, n_keep + n_discard, n_past, -n_discard);
  app.tokens.erase(
      app.tokens.begin() + n_keep,
      app.tokens.begin() + n_keep + n_discard);
  return json{
      {"success", true},
      {"n_past", app.tokens.size()},
  };
}

// clear all tokens in kv
json action_kv_clear(app_t &app, json &body)
{
  llama_kv_cache_clear(app.ctx);
  app.tokens.clear();
  return json{
      {"success", true},
      {"n_past", app.tokens.size()},
  };
}

// save current session
json action_session_save(app_t &app, json &body)
{
  std::string session_path = body["session_path"];
  std::vector<llama_token> dummy;
  if (!llama_save_session_file(
          app.ctx,
          session_path.c_str(),
          dummy.data(),
          dummy.size()))
  {
    return json{{"error", "action_session_save failed"}};
  }
  return json{
      {"success", true},
      {"tokens", app.tokens},
  };
}

// load a session from disk
json action_session_load(app_t &app, json &body)
{
  std::string session_path = body["session_path"];
  std::vector<llama_token> saved_tokens = body["tokens"];
  auto n_ctx = llama_n_ctx(app.ctx);
  size_t n_token_count_out = 0;
  std::vector<llama_token> dummy;
  if (!llama_load_session_file(
          app.ctx,
          session_path.c_str(),
          dummy.data(),
          dummy.capacity(),
          &n_token_count_out))
  {
    return json{{"error", "llama_load_session_file failed"}};
  }
  // load tokens
  app.tokens.clear();
  app.tokens.reserve(saved_tokens.size());
  for (auto id : saved_tokens)
  {
    app.tokens.push_back(id);
  }
  return json{{"success", true}};
}

// get the current status
json action_current_status(app_t &app, json &body)
{
  return json{
      {"success", true},
      {"tokens", app.tokens},
  };
}

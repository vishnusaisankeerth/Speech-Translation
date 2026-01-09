#include <sndfile.h>

#include <cassert>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// whisper.cpp
#include "whisper.h"

// bergamot-translator
#include "translator/service.h"          // marian::bergamot::AsyncService + parseOptionsFromString
#include "translator/translation_model.h"
#include "translator/response.h"

static std::string to_lower(std::string s) {
  for (char &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string require_arg(int &i, int argc, char **argv, const std::string &flag) {
  if (i + 1 >= argc) throw std::runtime_error("Missing value for " + flag);
  return std::string(argv[++i]);
}

static bool file_exists(const std::filesystem::path &p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
}

static int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void usage(const char *prog) {
  std::cerr <<
R"(Usage:
  )" << prog << R"( --wav input.wav --src en|es --tgt en|es
        --whisper path/to/ggml-*.bin
        --mt_root path/to/models/mt_direct_or_mt
        --piper path/to/piper_binary
        --tts_en path/to/en.onnx --tts_es path/to/es.onnx
        [--tts_en_cfg path/to/en.onnx.json] [--tts_es_cfg path/to/es.onnx.json]
        [--mt_size tiny|base|base-memory]
        [--threads N]
        [--out output.wav]
        [--quiet_whisper 0|1]

Example:
  taskset -c 0,1 env OMP_NUM_THREADS=2 )" << prog << R"( \
    --wav wavs/in.wav --src en --tgt es \
    --threads 2 \
    --whisper models/whisper/ggml-tiny-q8_0.bin \
    --mt_root models/mt_direct --mt_size tiny \
    --piper third_party/piper/piper \
    --tts_en models/tts/en_GB-alan-low.onnx --tts_en_cfg models/tts/en_GB-alan-low.onnx.json \
    --tts_es models/tts/es_ES-carlfm-x_low.onnx --tts_es_cfg models/tts/es_ES-carlfm-x_low.onnx.json \
    --out wavs/out_es.wav
)";
}

struct Args {
  std::string wav_path;

  std::string src_lang;
  std::string tgt_lang;

  int threads = 2;

  std::string whisper_model;
  std::string mt_root;
  std::string mt_size = "tiny";

  std::string piper_bin;

  std::string tts_en;
  std::string tts_es;

  std::string tts_en_cfg;
  std::string tts_es_cfg;

  std::string out_wav = "output.wav";

  bool quiet_whisper = true;
};

static std::vector<float> load_wav_mono_16k(const std::string &path, int &sr_out) {
  SF_INFO info{};
  SNDFILE *sf = sf_open(path.c_str(), SFM_READ, &info);
  if (!sf) throw std::runtime_error("Failed to open WAV: " + path);

  sr_out = info.samplerate;

  if (info.channels != 1) {
    sf_close(sf);
    throw std::runtime_error("WAV must be mono. channels=" + std::to_string(info.channels));
  }

  std::vector<float> data((size_t)info.frames);
  sf_count_t read = sf_readf_float(sf, data.data(), info.frames);
  sf_close(sf);

  if (read != info.frames) {
    throw std::runtime_error("Failed to read full WAV. read=" + std::to_string((long long)read));
  }

  return data;
}

static void whisper_log_silent_callback(enum ggml_log_level, const char *, void *) {
  // intentionally empty
}

struct WhisperEngine {
  whisper_context *ctx = nullptr;

  explicit WhisperEngine(const std::string &model_path) {
    whisper_context_params cparams = whisper_context_default_params();
    ctx = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx) throw std::runtime_error("whisper_init_from_file_with_params failed");
  }

  ~WhisperEngine() {
    if (ctx) whisper_free(ctx);
  }

  std::string transcribe(const std::vector<float> &audio_pcm_f32, int sample_rate,
                         const std::string &lang, int threads) {
    if (sample_rate != 16000) {
      throw std::runtime_error("Expected 16kHz WAV (got " + std::to_string(sample_rate) + ").");
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_timestamps = false;
    params.translate        = false;
    params.language         = lang.c_str();
    params.n_threads        = threads;
    params.greedy.best_of   = 1;

    if (whisper_full(ctx, params, audio_pcm_f32.data(), (int)audio_pcm_f32.size()) != 0) {
      throw std::runtime_error("whisper_full failed");
    }

    const int nseg = whisper_full_n_segments(ctx);
    std::ostringstream oss;
    for (int i = 0; i < nseg; ++i) {
      const char *seg = whisper_full_get_segment_text(ctx, i);
      if (seg) oss << seg;
    }

    std::string out = oss.str();
    while (!out.empty() && std::isspace((unsigned char)out.front())) out.erase(out.begin());
    return out;
  }
};

struct MtFiles {
  std::string model;
  std::string vocab;
  std::string lex;
};

static std::optional<MtFiles> try_layout(const std::filesystem::path &base,
                                        const std::string &pair_dir,
                                        const std::string &pair_code) {
  const auto model = base / pair_dir / ("model." + pair_code + ".intgemm.alphas.bin");
  const auto vocab = base / pair_dir / ("vocab." + pair_code + ".spm");
  const auto lex   = base / pair_dir / ("lex.50.50." + pair_code + ".s2t.bin");

  if (file_exists(model) && file_exists(vocab) && file_exists(lex)) {
    return MtFiles{model.string(), vocab.string(), lex.string()};
  }
  return std::nullopt;
}

static MtFiles resolve_mt_files(const std::string &mt_root, const std::string &mt_size,
                               const std::string &src, const std::string &tgt) {
  const std::string pair_dir  = src + "-" + tgt;
  const std::string pair_code = src + tgt;

  std::filesystem::path root(mt_root);

  {
    auto base = root / "firefox-translations-models" / "models" / mt_size;
    if (auto r = try_layout(base, pair_dir, pair_code)) return *r;
  }
  {
    auto base = root / mt_size;
    if (auto r = try_layout(base, pair_dir, pair_code)) return *r;
  }
  {
    auto base = root;
    if (auto r = try_layout(base, pair_dir, pair_code)) return *r;
  }

  throw std::runtime_error(
      "Could not find Bergamot MT files for " + pair_dir +
      " under mt_root=" + mt_root + " mt_size=" + mt_size +
      " (expected: model.<pair>.intgemm.alphas.bin, vocab.<pair>.spm, lex.50.50.<pair>.s2t.bin)");
}

static std::string make_bergamot_yaml(const MtFiles &f, int cpu_threads) {
  std::ostringstream y;
  y
    << "bergamot-mode: wasm\n"
    << "models:\n"
    << "  - " << f.model << "\n"
    << "vocabs:\n"
    << "  - " << f.vocab << "\n"
    << "  - " << f.vocab << "\n"
    << "shortlist:\n"
    << "  - " << f.lex << "\n"
    << "  - false\n"
    << "beam-size: 1\n"
    << "normalize: 1.0\n"
    << "word-penalty: 0\n"
    << "max-length-break: 128\n"
    << "mini-batch-words: 1024\n"
    << "workspace: 128\n"
    << "max-length-factor: 2.0\n"
    << "skip-cost: true\n"
    << "cpu-threads: " << cpu_threads << "\n"
    << "quiet: true\n"
    << "quiet-translation: true\n"
    << "gemm-precision: int8shiftAlphaAll\n";
  return y.str();
}

struct MtEngine {
  marian::bergamot::AsyncService service;
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<marian::bergamot::TranslationModel>> models;

  explicit MtEngine(int workers)
      : service(make_service(workers)) {}

  static marian::bergamot::AsyncService make_service(int workers) {
    marian::bergamot::AsyncService::Config cfg;
    cfg.numWorkers = (workers < 1) ? 1 : (size_t)workers;
    cfg.cacheSize = 0;
    return marian::bergamot::AsyncService(cfg);
  }

  std::shared_ptr<marian::bergamot::TranslationModel> get_or_create_model(const std::string &key,
                                                                          const std::string &yaml) {
    std::lock_guard<std::mutex> lock(mu);

    auto it = models.find(key);
    if (it != models.end()) return it->second;

    auto options = marian::bergamot::parseOptionsFromString(yaml);
    auto m = service.createCompatibleModel(options);
    models.emplace(key, m);
    return m;
  }

  std::string translate(const std::string &key,
                        const std::string &yaml,
                        const std::string &text) {
    auto model = get_or_create_model(key, yaml);

    std::promise<marian::bergamot::Response> prom;
    auto fut = prom.get_future();

    marian::bergamot::ResponseOptions ro; // keep defaults (no html field in this version)
    service.translate(
        model,
        std::string(text),
        [&prom](marian::bergamot::Response &&resp) mutable {
          prom.set_value(std::move(resp));
        },
        ro
    );

    marian::bergamot::Response resp = fut.get();
    return resp.target.text;
  }
};

static void piper_tts_to_wav(const std::string &piper_bin,
                            const std::string &voice_onnx,
                            const std::optional<std::string> &voice_cfg,
                            const std::string &text,
                            const std::string &out_wav) {
  int inpipe[2];
  if (pipe(inpipe) != 0) throw std::runtime_error("pipe() failed: " + std::string(strerror(errno)));

  pid_t pid = fork();
  if (pid < 0) {
    close(inpipe[0]); close(inpipe[1]);
    throw std::runtime_error("fork() failed");
  }

  if (pid == 0) {
    dup2(inpipe[0], STDIN_FILENO);
    close(inpipe[0]);
    close(inpipe[1]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(piper_bin.c_str()));

    argv.push_back(const_cast<char*>("--model"));
    argv.push_back(const_cast<char*>(voice_onnx.c_str()));

    if (voice_cfg.has_value()) {
      argv.push_back(const_cast<char*>("--config"));
      argv.push_back(const_cast<char*>(voice_cfg->c_str()));
    }

    argv.push_back(const_cast<char*>("--output_file"));
    argv.push_back(const_cast<char*>(out_wav.c_str()));
    argv.push_back(nullptr);

    execv(piper_bin.c_str(), argv.data());
    _exit(127);
  }

  close(inpipe[0]);

  std::string payload = text;
  if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
  (void)write(inpipe[1], payload.data(), payload.size());
  close(inpipe[1]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    throw std::runtime_error("piper failed (exit status not 0)");
  }
}

int main(int argc, char **argv) {
  try {
    Args a;

    for (int i = 1; i < argc; ++i) {
      const std::string flag = argv[i];

      if (flag == "--help" || flag == "-h") {
        usage(argv[0]);
        return 0;
      } else if (flag == "--wav") {
        a.wav_path = require_arg(i, argc, argv, flag);
      } else if (flag == "--src") {
        a.src_lang = to_lower(require_arg(i, argc, argv, flag));
      } else if (flag == "--tgt") {
        a.tgt_lang = to_lower(require_arg(i, argc, argv, flag));
      } else if (flag == "--threads") {
        a.threads = std::stoi(require_arg(i, argc, argv, flag));
      } else if (flag == "--whisper") {
        a.whisper_model = require_arg(i, argc, argv, flag);
      } else if (flag == "--mt_root") {
        a.mt_root = require_arg(i, argc, argv, flag);
      } else if (flag == "--mt_size") {
        a.mt_size = to_lower(require_arg(i, argc, argv, flag));
      } else if (flag == "--piper") {
        a.piper_bin = require_arg(i, argc, argv, flag);
      } else if (flag == "--tts_en") {
        a.tts_en = require_arg(i, argc, argv, flag);
      } else if (flag == "--tts_es") {
        a.tts_es = require_arg(i, argc, argv, flag);
      } else if (flag == "--tts_en_cfg") {
        a.tts_en_cfg = require_arg(i, argc, argv, flag);
      } else if (flag == "--tts_es_cfg") {
        a.tts_es_cfg = require_arg(i, argc, argv, flag);
      } else if (flag == "--out") {
        a.out_wav = require_arg(i, argc, argv, flag);
      } else if (flag == "--quiet_whisper") {
        a.quiet_whisper = (std::stoi(require_arg(i, argc, argv, flag)) != 0);
      } else {
        throw std::runtime_error("Unknown flag: " + flag);
      }
    }

    if (a.wav_path.empty() || a.src_lang.empty() || a.tgt_lang.empty() ||
        a.whisper_model.empty() || a.mt_root.empty() ||
        a.piper_bin.empty() || a.tts_en.empty() || a.tts_es.empty()) {
      usage(argv[0]);
      throw std::runtime_error("Missing required arguments.");
    }

    if (!((a.src_lang == "en" && a.tgt_lang == "es") || (a.src_lang == "es" && a.tgt_lang == "en"))) {
      throw std::runtime_error("Only en<->es supported.");
    }

    if (a.threads < 1) a.threads = 1;

    if (a.quiet_whisper) {
      whisper_log_set(whisper_log_silent_callback, nullptr);
    }

    const int64_t t0_total = now_ms();

    WhisperEngine whisper(a.whisper_model);
    MtEngine mt(a.threads);

    int sr = 0;
    std::vector<float> pcm = load_wav_mono_16k(a.wav_path, sr);

    const int64_t t0_asr = now_ms();
    const std::string asr = whisper.transcribe(pcm, sr, a.src_lang, a.threads);
    const int64_t t1_asr = now_ms();
    std::cout << "[ASR] " << asr << "\n";
    std::cout << "[TIME] ASR  : " << (t1_asr - t0_asr) << " ms\n\n";

    MtFiles files = resolve_mt_files(a.mt_root, a.mt_size, a.src_lang, a.tgt_lang);
    const std::string yaml = make_bergamot_yaml(files, a.threads);
    const std::string key = a.mt_size + ":" + a.src_lang + "-" + a.tgt_lang;

    const int64_t t0_mt = now_ms();
    const std::string tr = mt.translate(key, yaml, asr);
    const int64_t t1_mt = now_ms();
    std::cout << "[MT ] " << tr << "\n";
    std::cout << "[TIME] MT   : " << (t1_mt - t0_mt) << " ms\n\n";

    const bool tgt_is_en = (a.tgt_lang == "en");
    const std::string voice = tgt_is_en ? a.tts_en : a.tts_es;

    std::optional<std::string> cfg;
    if (tgt_is_en && !a.tts_en_cfg.empty()) cfg = a.tts_en_cfg;
    if (!tgt_is_en && !a.tts_es_cfg.empty()) cfg = a.tts_es_cfg;

    if (!cfg.has_value()) {
      std::filesystem::path p = voice;
      std::filesystem::path guess = p;
      guess += ".json";
      if (file_exists(guess)) cfg = guess.string();
    }

    const int64_t t0_tts = now_ms();
    piper_tts_to_wav(a.piper_bin, voice, cfg, tr, a.out_wav);
    const int64_t t1_tts = now_ms();
    std::cout << "[TTS] wrote " << a.out_wav << "\n";
    std::cout << "[TIME] TTS  : " << (t1_tts - t0_tts) << " ms\n\n";

    const int64_t t1_total = now_ms();
    std::cout << "[TIME] TOTAL: " << (t1_total - t0_total) << " ms\n";

    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}

# CAMB2 - Speech Translation Pipeline

A CPU-only offline speech translation system using Whisper (ASR), Bergamot (MT), and Piper (TTS).

## Features

- Speech-to-text transcription (Whisper)
- Neural machine translation (Bergamot)
- Text-to-speech synthesis (Piper)
- CPU-only operation
- Fully offline execution
- English ↔ Spanish translation

## System Requirements

- **OS**: Linux (aarch64 or x86_64)
- **Build tools**: CMake >= 3.16, g++ with C++17 support
- **Audio libraries**: libsndfile1

## Installation

### 1. Install Dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  cmake \
  pkg-config \
  libsndfile1-dev
```

### 2. Clone Repository and Submodules

```bash
git clone <your-repo-url> camb2
cd camb2
git clone https://github.com/ggerganov/whisper.cpp third_party/whisper.cpp
git clone https://github.com/browsermt/bergamot-translator third_party/bergamot-translator
git clone https://github.com/rhasspy/piper third_party/piper
```

### 3. Build Piper

```bash
cd third_party/piper
mkdir build && cd build
cmake ..
make -j
cd ../../..
```

The Piper binary will be located at: `third_party/piper/piper/piper`

### 4. Download Models

#### Whisper ASR Models

```bash
mkdir -p models/whisper
cd models/whisper
wget https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q5_1.bin
cd ../..
```

#### Bergamot MT Models (English → Spanish)

```bash
mkdir -p models/mt_direct/tiny/en-es
cd models/mt_direct/tiny/en-es
```

Place the following files in this directory:
- `model.enes.intgemm.alphas.bin`
- `vocab.enes.spm`
- `lex.50.50.enes.s2t.bin`

```bash
cd ../../../..
```

#### Piper TTS Voice Models

**English Voice (GB - Alan)**

```bash
mkdir -p models/tts/en_GB-alan-low
cd models/tts/en_GB-alan-low
wget https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alan/low/en_GB-alan-low.onnx -O model.onnx
wget https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alan/low/en_GB-alan-low.onnx.json -O model.onnx.json
cd ../../..
```

**Spanish Voice (ES - Carlfm)**

```bash
mkdir -p models/tts/es_ES-carlfm-x_low
cd models/tts/es_ES-carlfm-x_low
wget https://huggingface.co/rhasspy/piper-voices/blob/main/es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx -O model.onnx
wget https://huggingface.co/rhasspy/piper-voices/blob/main/es/es_ES/carlfm/x_low/es_ES-carlfm-x_low.onnx.json -O model.onnx.json
cd ../..
```

### 5. Build Project

```bash
mkdir build
cd build
cmake ..
make -j
```

The executable will be located at: `build/speech_translate`

## Usage

### 2-Core Configuration

```bash
taskset -c 0,1 env OMP_NUM_THREADS=2 ./build/speech_translate \
  --wav wavs/G00550S1025.wav \
  --src en --tgt es \
  --threads 2 \
  --quiet_whisper 1 \
  --whisper models/whisper/ggml-tiny-q5_1.bin \
  --mt_root models/mt_direct \
  --mt_size tiny \
  --piper third_party/piper/piper/piper \
  --tts_en models/tts/en_GB-alan-low/model.onnx \
  --tts_en_cfg models/tts/en_GB-alan-low/model.onnx.json \
  --tts_es models/tts/es_ES-carlfm-x_low/model.onnx \
  --tts_es_cfg models/tts/es_ES-carlfm-x_low/model.onnx.json \
  --out wavs/out_es_2core.wav
```

### 4-Core Configuration

```bash
taskset -c 0-3 env OMP_NUM_THREADS=4 ./build/speech_translate \
  --wav wavs/G00550S1025.wav \
  --src en --tgt es \
  --threads 4 \
  --quiet_whisper 1 \
  --whisper models/whisper/ggml-tiny-q5_1.bin \
  --mt_root models/mt_direct \
  --mt_size tiny \
  --piper third_party/piper/piper/piper \
  --tts_en models/tts/en_GB-alan-low/model.onnx \
  --tts_en_cfg models/tts/en_GB-alan-low/model.onnx.json \
  --tts_es models/tts/es_ES-carlfm-x_low/model.onnx \
  --tts_es_cfg models/tts/es_ES-carlfm-x_low/model.onnx.json \
  --out wavs/out_es_4core.wav
```

## Important Notes

- **Input audio must be mono 16 kHz WAV format**
- Only English ↔ Spanish translation is currently supported
- CPU-only execution (no GPU required)
- All processing happens offline (no internet required)

## License

[Add your license information here]

## Acknowledgments

- [Whisper.cpp](https://github.com/ggerganov/whisper.cpp) - Speech recognition
- [Bergamot Translator](https://github.com/browsermt/bergamot-translator) - Neural machine translation
- [Piper](https://github.com/rhasspy/piper) - Text-to-speech synthesis
```

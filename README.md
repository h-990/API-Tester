# API-Tester (C++ GUI)

Native desktop GUI app (C++) API tester to audit many LLM API keys in one run.

It measures:
- Auth/model-list reachability
- Rate/quota/limit headers returned by providers
- Available model list + sampled catalog
- Working vs failing models (real probe requests)
- Prompt test quality for reasoning, coding, and AX UI-tree interpretation
- Request-level latency, status, snippets, and raw logs
- Full export reports (TXT + JSON)

## Providers Included
- OpenRouter
- Google AI Studio
- Mistral
- Vercel AI Gateway
- Groq
- Cohere
- AI21
- GitHub Models PATs: `chatgpt`, `chatgpt5`, `deepseek`, `jamba`

## Project Layout
- `src/main.cpp`: GUI + key management + run/export controls
- `src/audit_engine.*`: provider audit logic and measurements
- `src/report_writer.*`: TXT/JSON report generation
- `config/api_keys.json`: saved keys (created at runtime)
- `reports/`: exported reports

## Build Requirements
- CMake 3.21+
- Ninja (recommended)
- C++20 compiler
- `curl` dev package
- Internet access for FetchContent dependencies (raylib + nlohmann/json)

## Native Build
```bash
cmake --preset native-release
cmake --build --preset build-native-release
./build/native-release/api_tester
```

## Cross Compile (with Zig)
Install Zig, then use presets:

```bash
cmake --preset zig-linux-x86_64
cmake --build --preset build-zig-linux-x86_64

cmake --preset zig-linux-aarch64
cmake --build --preset build-zig-linux-aarch64

cmake --preset zig-windows-x86_64
cmake --build --preset build-zig-windows-x86_64

cmake --preset zig-macos-aarch64
cmake --build --preset build-zig-macos-aarch64
```

If you need a different target triple, pass your own:

```bash
cmake -S . -B build/custom \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/zig-toolchain.cmake \
  -DZIG_SYSTEM_NAME=Linux \
  -DZIG_TARGET=x86_64-linux-musl
cmake --build build/custom
```

## Runtime Notes
- On startup, set and apply a **Working Directory** in the GUI.
- Everything is loaded/saved under that selected directory:
  - `config/api_keys.json`
  - `reports/llm_api_audit_*.txt`
  - `reports/llm_api_audit_*.json`
  - `logs/llm_api_runlog_*.log`
- `Run Full Audit` performs live API calls and writes a run log automatically.
- Report files include API keys in plaintext by design (for full traceability). Keep them private.

## Security Recommendation
Rotate any key that was shared in chat or saved into report files you distribute.

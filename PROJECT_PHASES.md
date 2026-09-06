# OWN-AI — Phase-Wise Development Plan

> Development roadmap and engineering log for the `pro-upgrade` branch.
> This file is updated as each phase is implemented and verified.

## Project Goal

Build OWN-AI into a practical local AI assistant with a reliable chat experience, RAG/document capabilities, persistent conversations, a polished UI, and resource usage that is practical on lower-RAM machines.

---

## Phase 1 — Baseline & Core Stability
**Status: ✅ Completed**

### Focus
- Establish a working local OWN-AI baseline.
- Verify the C++ backend, web UI, Ollama integration, and local server workflow.
- Preserve a recoverable Git workflow while making upgrades.

### Outcome
- OWN-AI runs locally through the C++ server and Ollama.
- Development continues on the `pro-upgrade` branch.

---

## Phase 2 — Unicode & Message/Input Reliability
**Status: ✅ Completed**

### Issues addressed
- Unicode/text rendering and handling problems.
- Message/input behavior that needed to work reliably from the chat UI.

### Outcome
- Unicode handling improved.
- Message input behavior improved at the code level.

---

## Phase 3 — Chat UI & Conversation Experience
**Status: ✅ Completed / Integrated**

### Improvements
- Improved the chat UI and composer experience.
- Fixed the multi-message input problem caused by duplicate `id="messageInput"` elements.
- Added frontend conversation state through `chatMessages[]`.
- Added conversation-history transmission to `/doc/ask`.
- Added conversation-history persistence using `localStorage`.
- Added history loading/saving behavior for conversations.

### Verification
- First prompt works.
- Subsequent prompts can be sent.
- Enter works for subsequent messages.
- Follow-up questions can use earlier conversation context.

---

## Phase 4 — Performance, RAM & Ollama Optimization
**Status: ✅ Completed & Tested**

### Problems identified
- The original `llama3` model is approximately 4.7 GB on disk and was too heavy for an 8 GB Mac when combined with the OS, IDE, backend, and context/KV-cache usage.
- New Ollama connections were created repeatedly for requests.
- Ollama's context window could consume substantial additional RAM.
- The generation model was hardcoded rather than easily configurable.
- Backend thread usage was not explicitly capped.
- Embeddings were duplicated in the BruteForce fallback beyond the point where HNSW was used.

### Changes implemented
- Reused persistent Ollama HTTP clients instead of creating a fresh client for every request.
- Added `num_ctx=2048` to generation configuration.
- Added `num_predict=512` to cap generation length.
- Added environment-variable configuration:
  - `OWN_AI_GEN_MODEL`
  - `OWN_AI_EMBED_MODEL`
  - `OWN_AI_NUM_CTX`
  - `OWN_AI_NUM_PREDICT`
- Capped BruteForce embedding storage at the HNSW transition threshold.
- Added an explicit 4-thread server thread pool.
- Added startup RAM guidance for different memory capacities.

### Testing
The default heavy model was replaced for testing with:

`llama3.2:3b`

Current tested configuration:

- Generation model: `llama3.2:3b`
- Embedding model: `nomic-embed-text`
- Context: `2048`
- Max prediction: `512`

Observed results on the 8 GB Mac:

- Response time improved compared with the previous setup.
- Short responses took roughly 1–2 seconds in the manual test.
- A more complex comparison response took roughly 7 seconds.
- The laptop remained usable during the test.
- Multi-turn conversation history worked in controlled testing.

### Git checkpoint
- `18d20c8` — checkpoint before performance optimization
- `fff211f` — optimize Ollama performance and RAM usage

---

## Phase 5 — Conversation History, Save History Toggle & Memory System
**Status: ✅ Completed & Tested**

### Problems identified
- The Phase 3 conversation sidebar only stored title strings without message content, conversation IDs, or click event handlers.
- Clicking an item in the conversation sidebar did nothing because no restore logic existed.
- Message context (`chatMessages[]`) was lost whenever `newChat()` ran or the browser was reloaded.
- No user setting existed to disable conversation persistence (disappearing messages).
- No distinction existed between short-term chat history and long-term memory notes.

### Changes implemented
- **Full Conversation Persistence**: Every chat gets a unique ID (`conv_<timestamp>_<rand>`) and stores all user and assistant messages with timestamps in `localStorage['own-ai-chats']` (capped at 50 conversations).
- **Interactive Restoration**: Clicking any sidebar conversation loads all past messages in order, rebuilds `chatMessages[]` for Ollama context, highlights the active chat, and allows multi-turn follow-ups.
- **Save History Toggle**: Added a user toggle (`💾 Save History [ON/OFF]`) stored in `localStorage['own-ai-settings']`. When OFF, new conversations behave as disappearing chats and are not written to storage.
- **Long-Term Memory Drawer**: Added a lightweight slide-in Memory drawer (`🧠 Memory`) for storing persistent user notes across chats (`localStorage['own-ai-memory']`), completely decoupled from chat history.
- **Title Generation**: Automatically generates conversation titles from the first 60 characters of the first user message.
- **Real-Time Sidebar Search**: Wired `#sbSearch` to filter conversation titles and message contents live.
- **Zero-RAM Impact**: Persistence and memory are 100% local browser storage logic, adding zero memory overhead to Ollama or the C++ server.

### Testing performed
- **New Chat**: Starts clean state without mixing messages.
- **Message Sequence & Restoration**: Verified multi-turn conversations save and restore full message history upon clicking sidebar items.
- **Save History Toggle**: Verified disabling "Save History" prevents new chats from persisting while existing saved chats remain untouched.
- **Delete Conversation**: Verified deleting a chat removes it permanently from storage and UI.
- **Long-Term Memory**: Verified adding and removing memory items in the drawer.
- **Regression Checks**: Verified backend compilation (`g++ -std=c++17 -O2 main.cpp -o server`), Phase 4 low-RAM Ollama model configuration (`llama3.2:3b`), and document RAG pipelines remain intact.

### Limitations
- Storage is local to the browser's `localStorage` (clearing browser data clears history).
- Search currently performs case-insensitive substring matching over stored conversations.

---

## Phase 6 — Streaming & Response UX
**Status: ✅ Completed & Tested**

### Problems addressed
- Previous responses were delivered in a single monolithic payload, causing multi-second delays before any output appeared.
- Users lacked real-time visual feedback ("Generating..." status) while Ollama was inferring responses.
- No mechanism existed to stop or cancel long generation requests midway.
- Repeated keypresses or prompt submissions could launch duplicate in-flight requests.

### Changes implemented
- **Real-Time Token Streaming**: Added `OllamaClient::generateStream` in `main.cpp` using `"stream": true` and HTTP chunk parsing, paired with a new `POST /doc/ask/stream` Server-Sent Events (SSE) endpoint.
- **Progressive UI Rendering**: Updated `sendMessage()` in `index.html` to consume SSE streams using browser `ReadableStream`, progressively rendering tokens via `renderMarkdown()` as they arrive.
- **Interactive Stop / Cancel Generation**: Added a glowing red `⏹ Stop` button during streaming. Triggering `stopGeneration()` invokes `AbortController.abort()`, cleanly closing the HTTP socket and stopping Ollama inference immediately.
- **Accidental Submission Prevention**: Disabled composer inputs and blocked `Enter` key execution while `isLoading = true`.
- **History & Context Integration**: Partial or complete streamed outputs are cleanly saved to `chatMessages[]` and `localStorage['own-ai-chats']` (if `Save History` is ON).
- **Backward Compatibility**: Kept original `/doc/ask` endpoint intact while adding `/doc/ask/stream`.

### Files changed
- [`main.cpp`](file:///Users/ayanthara/Desktop/OWN-AI/main.cpp) — Added `OllamaClient::generateStream()` and `POST /doc/ask/stream` endpoint.
- [`index.html`](file:///Users/ayanthara/Desktop/OWN-AI/index.html) — Updated `sendMessage()`, added `stopGeneration()`, `setGeneratingUI()`, and `.send-btn.stop-btn` CSS.

### Testing performed
- **Backend Compilation**: Compiled cleanly with `g++ -std=c++17 -O2 main.cpp -o server`.
- **JS Syntax & Unit Tests**: Verified zero syntax errors and passed automated mock unit suite for streaming lifecycle, cancellation, and history serialization.
- **Stop Generation**: Verified cancelling stream preserves partial response in chat history without corrupting sequence.
- **Phase 4 & 5 Regression**: Verified low-RAM Ollama config (`llama3.2:3b`), HNSW RAG context retrieval, Save History toggle, and memory drawer remain fully operational.

### Corrective Fix & Pipeline Hardening

#### Root Causes Identified
1. **Buffered Token Delivery (No visible streaming)**: `CPPHTTPLIB_TCP_NODELAY` was `false` by default in `httplib.h`. OS Nagle algorithm buffered small ~20-byte SSE token packets e.g. `"data: {\"token\":\"the\"}\n\n"` in socket buffers until 4KB accumulated or generation ended, causing responses to appear all at once.
2. **Stop Button Ineffective & Backend Lock**: When client aborted, `tokenCb` returned `false`, but the underlying `genCli_` persistent HTTP connection was not explicitly reset (`genCli_.stop()`). Ollama continued inferring tokens in the background while holding `genMu_`, blocking subsequent user requests.

#### Fixes Implemented
- **Socket Low-Latency Optimization**: Enabled `set_tcp_nodelay(true)` on `httplib::Server` and all `OllamaClient` HTTP connections (`embedCli_`, `genCli_`, `checkCli_`), forcing immediate OS socket packet transmission per token.
- **Immediate Inference Cancel**: Added `genCli_.stop()` call in `OllamaClient::generateStream` whenever `tokenCb` returns `false` (on client abort). This closes the TCP socket to Ollama on port 11434 instantly, terminating Ollama's GPU/CPU inference loop and releasing `genMu_` immediately.

#### End-to-End Testing Performed (Live Server Verification)
- **Test A (Progressive Token Streaming)**: Verified real-time SSE token delivery over HTTP socket with individual token arrival timestamps (`+267ms`, `+294ms`, `+320ms`).
- **Test B (Stop Generation)**: Verified clicking Stop halts stream reception instantly and triggers backend socket reset.
- **Test C (Immediate Follow-Up)**: Verified sending a prompt immediately after Stop succeeds instantly without thread/mutex lockup.
- **Test D (Sequential Prompts)**: Verified multiple back-to-back streaming conversations execute cleanly.
- **Test E (Save History ON)**: Verified full/partial response stream persists to `localStorage['own-ai-chats']`.
- **Test F (Save History OFF)**: Verified disappearing chat mode operates without persistence.
- **Test G (Old Chat Restoration)**: Verified `loadConversation()` loads multi-turn history accurately after streaming.
- **Test H (RAG Pipeline)**: Verified document embedding, HNSW retrieval, and streaming SSE pipeline function end-to-end.

#### Known Limitations
- Network latency or heavy local CPU load can affect per-token inter-arrival intervals.

---

## Phase 6 — RAG & Document Intelligence Hardening
**Status: ⏳ Planned**

### Goals
- Thoroughly test document ingestion and retrieval.
- Verify embedding, HNSW/KD-Tree/BruteForce search behavior.
- Improve answer grounding and fallback behavior.
- Test multiple documents and follow-up questions against document context.

### Verification targets
- Documents can be ingested reliably.
- Relevant chunks are retrieved.
- Answers use retrieved context when appropriate.
- Non-document questions still work normally.

---

## Phase 7 — Coding Accuracy & Code Verification
**Status: ✅ Completed & Tested**

### Overview
Addressed C++ code correctness, compilation error detection, test-case execution, sandboxing, resource limits, and automated self-correction feedback loop for coding/DSA problems using local `llama3.2:3b`.

### Key Achievements
- **`CodeVerifier` Engine**: Built safe compilation and execution engine in `main.cpp` using POSIX `fork()`, `execv()`, and `setrlimit()`.
- **POSIX Sandboxing & Resource Limits**:
  - `RLIMIT_CPU`: 2.0 seconds execution limit.
  - `RLIMIT_AS`: 512 MB virtual memory cap.
  - `RLIMIT_NPROC`: Child process fork bomb prevention.
  - Hard `SIGKILL` timer after 2.0s timeout.
- **Iterative Self-Correction Loop**: `POST /doc/ask/code` queries Ollama, extracts code, compiles/runs, and if compilation/runtime fails, feeds compiler/test error back to model for automatic correction (up to 3 retries).
- **Interactive UI Verification**: Added `▶ Run & Verify` button to C++ code blocks in `index.html` calling `POST /code/verify`. Displays `✓ Compiled & Verified (Exit 0)` or `⚠ Execution Failed` badges with logs.

### Files Changed
- [`main.cpp`](file:///Users/ayanthara/Desktop/OWN-AI/main.cpp) — Added `CodeVerifier` class, POSIX sandboxing, `POST /code/verify` and `POST /doc/ask/code` endpoints.
- [`index.html`](file:///Users/ayanthara/Desktop/OWN-AI/index.html) — Added `▶ Run & Verify` button, verification badges, log drawer, and CSS styles.

### Verification Performed
- **Valid C++ Code Execution**: Verified `POST /code/verify` returns `verified: true`, exit code 0.
- **Compilation Error Handling**: Verified compiler errors (`g++ -Wall -O2`) are captured.
- **Sandboxed Timeout Enforcement**: Verified infinite loops (`while(true);`) are killed after 2.0 seconds with `Runtime exit code -2: Execution timed out`.
- **Automated Coding Pipeline**: Verified `POST /doc/ask/code` generates C++ code, compiles, executes, and verifies answer output.

---

## Phase 8 — Reliability, Testing & Error Handling
**Status: ⏳ Planned**

### Goals
- Build a repeatable test checklist instead of relying only on manual exploratory testing.
- Test server/Ollama unavailable states.
- Test malformed input and empty messages.
- Test repeated prompts and longer conversations.
- Verify recovery after restarting Ollama or OWN-AI.

### Verification targets
- No crashes on expected failure cases.
- Clear user-facing errors.
- Reliable restart behavior.
- No accidental automated prompt submission.

---

## Phase 9 — Packaging & Deployment
**Status: ⏳ Planned**

### Goals
- Make OWN-AI easier for another person to run.
- Document prerequisites and model choices.
- Provide sensible low-RAM defaults/configuration.
- Separate development/debugging instructions from normal user instructions.

### Verification targets
- A new user can follow the setup documentation.
- Model configuration does not require recompilation.
- 6–8 GB machines have a documented lightweight configuration.
- The project starts with a predictable workflow.

---

## Phase 10 — Final Polish & Release Candidate
**Status: ⏳ Planned**

### Goals
- Full end-to-end testing.
- UI polish.
- Performance review.
- Documentation cleanup.
- Final GitHub release preparation.

### Release checklist
- [ ] Chat works across multiple turns.
- [ ] Enter and Send button both work.
- [ ] Unicode works.
- [ ] Conversation history persists correctly.
- [ ] RAG/document workflow works.
- [ ] Streaming works.
- [ ] Ollama/model configuration is documented.
- [ ] 8 GB performance is acceptable.
- [ ] Error handling is reliable.
- [ ] Setup documentation is complete.
- [ ] Final end-to-end test passes.

---

## Development Rule

Each phase should follow this workflow:

1. **Identify the problem.**
2. **Create a clear implementation plan.**
3. **Make the smallest justified code changes.**
4. **Compile/build and check for errors.**
5. **Test the affected functionality.**
6. **Record the result in this file.**
7. **Create a Git checkpoint/commit.**
8. **Push the verified state to GitHub.**
9. **Only then move to the next phase.**

This prevents the project from becoming difficult to test or recover while multiple unrelated changes are being made at once.

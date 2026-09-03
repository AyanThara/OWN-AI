# OWN-AI — Phase-Wise Development Plan

> Development roadmap and engineering log for the `pro-upgrade` branch.
> This file is updated as each phase is implemented and verified.

## Project Goal

Build OWN-AI into a practical local AI assistant with a reliable chat experience, RAG/document capabilities, persistent conversations, useful memory controls, a polished UI, and resource usage that is practical on lower-RAM machines.

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
- Added the initial sidebar history UI.

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
- Multi-turn conversation context worked in controlled testing.

### Git checkpoint
- `18d20c8` — checkpoint before performance optimization
- `fff211f` — optimize Ollama performance and RAM usage

---

## Phase 5 — Conversation Persistence, History & Memory
**Status: ✅ Completed / Manually Verified**

### Problems identified

The Phase 3 sidebar history was initially cosmetic. It stored conversation titles but did not store complete message sequences, conversation IDs, or a reliable restoration path. Clicking an old chat therefore did not reopen the conversation.

During Phase 5 implementation/testing, additional UI and persistence issues were identified and fixed, including history persistence, old-chat restoration, Save History behavior, and the Memory drawer boundary/layout issue.

### Changes implemented
- Replaced title-only history with complete conversation records.
- Added unique conversation IDs and timestamps.
- Persisted user and assistant message sequences locally.
- Added click-to-restore behavior for previous conversations.
- Rebuilt `chatMessages[]` when restoring a conversation so follow-up questions retain the restored context.
- Added separate New Chat/conversation boundaries.
- Added conversation deletion.
- Added Save History ON/OFF control.
- Added temporary/disappearing conversation behavior when Save History is OFF.
- Added a separate long-term Memory system using local browser storage.
- Added Memory management UI and the ability to remove saved memories.
- Added live history search across conversation titles/message content.
- Fixed the Memory drawer so it respects the existing sidebar/application boundaries.
- Kept the implementation local to the browser with no new backend endpoints or Ollama calls.

### Data separation

Chat history and long-term memory are intentionally separate:

- **History** stores actual conversation messages.
- **Memory** stores explicitly saved useful information independently of individual conversations.
- Disabling history does not turn ordinary chat messages into permanent memory.

### Manual verification performed
- New Chat creates a clean conversation boundary.
- Multiple user/assistant turns remain associated with the correct conversation.
- Old conversations can be clicked and restored.
- Restored conversations can continue with the existing multi-turn context.
- Saved conversations survive browser refresh.
- Save History ON persists conversations.
- Save History OFF prevents new temporary conversations from being persisted.
- Memory remains available separately from conversation history.
- Existing RAG/chat behavior remains available after the Phase 5 changes.

### Remaining verification before Phase 6
- Perform one final end-to-end pass covering deletion, temporary chat, refresh, memory, and multiple conversations together.
- Confirm the final working tree and GitHub branch are synchronized.

### Git checkpoint
- `a73f02e` — `feat: Phase 5 – conversation history, save toggle, memory system`

---

## Phase 6 — Streaming & Response UX
**Status: ⏳ Planned**

### Goals
- Stream generated responses instead of waiting for the entire answer.
- Make long responses feel faster through incremental rendering.
- Improve loading/typing-state feedback.
- Preserve conversation persistence while introducing streaming.

### Verification targets
- First token appears quickly.
- Tokens render progressively.
- Stop/cancel behavior is reliable.
- Conversation history stores the completed response correctly.
- RAM usage remains practical on the 8 GB machine.

---

## Phase 7 — RAG & Document Intelligence Hardening
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
- [ ] Old conversations can be restored.
- [ ] Temporary Chat works.
- [ ] Explicit memory works.
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

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

## Phase 5 — Streaming & Response UX
**Status: ⏳ Planned**

### Goals
- Stream generated responses instead of waiting for the entire answer.
- Make long responses feel faster through incremental rendering.
- Improve loading/typing-state feedback.
- Preserve the existing multi-turn chat behavior while introducing streaming.

### Verification targets
- First token appears quickly.
- Tokens render progressively.
- Stop/cancel behavior is reliable.
- Conversation history stores the completed response correctly.
- RAM usage remains practical on the 8 GB machine.

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

## Phase 7 — Conversation & History Productization
**Status: ⏳ Planned**

### Goals
- Turn the current conversation-history implementation into a complete user-facing history system.
- Make previous conversations easy to select and restore.
- Verify persistence across browser/page restarts.
- Handle new-chat and conversation boundaries cleanly.

### Verification targets
- New Chat starts a clean conversation.
- Previous conversations can be restored.
- History survives page reloads.
- Current conversation is not accidentally mixed with another conversation.

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

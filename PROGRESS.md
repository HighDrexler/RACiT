# NA 1.20 keyboard menu integration

The update branch combines its version-specific game and worker support with the Mac keyboard-menu implementation. Frontend and pause-menu publishing use the verified update addresses. Previously qualified Mac and update builds are the prerequisites for this source checkpoint. The combined candidate still needs normal generation, compilation and actual update gameplay qualification.

- [x] Preserve update generation, host bindings, SPU mappings, audio and the pipeline-publication correction.
- [x] Integrate menu archive preparation, read-only selection and the shared keyboard/controller prompt decision.
- [x] Add fingerprint-bound update frontend and pause hooks matching the installed local implementation.
- [x] Reapply the complete SDK patch to its pinned clean base and reproduce the intended source tree.
- [ ] Run normal update patching and confirm repeat invocation preserves its output.
- [ ] Build the combined native update and observe title, frontend and pause prompts.

### **2026-11-12 00:15 -05:00 — Update-specific menu source checkpoint**

The update and Mac menu source histories are combined. The only merge conflict affected the serialized SDK patch. Reconstructed source comparison established that the menu SDK already contains the update's complete input changes. The resolved patch reproduces that exact menu SDK tree. Its five-file difference above the qualified update SDK adds protected reads and read-only menu archive selection. Audio and Vulkan backend bytes match the qualified update.

The update recipe adds two checked function edits. The frontend hook uses `004C76D0` with owner offset 4. The pause hook uses `004AE1C8` with offsets 8 and 4. Its consumer adds the shared header only to participating chunks and preserves validation of every function before writing output. Four Python entry/helper sources parse and the recipe contains 79 unique function entries. No game archive, generated game body or build output is included. Next run normal update patching and the retained incremental build, followed by actual update menu observations. [Operational handoff](HANDOFF.md).

## Earlier Mac and menu implementation history

This branch combines the Mac port with keyboard labels for image-based menu prompts. Source implementation includes archive preparation and selection through the ordinary player entry point. The combined source preserves the Mac audio, rendering and queued-pipeline corrections. Native build and gameplay qualification of this combined branch remain pending.

- [x] Implement frame publishing for the BCUS98124 disc 1.00 frontend and in-game screen manager.
- [x] Preserve the original controller artwork and title formatting while fitting keyboard labels to their existing space.
- [x] Include the protected guest-read prerequisite and source-derived menu actions.
- [x] Implement archive preparation and selection through the normal player entry point. A tiny synthetic archive checks block-table growth, neighboring data preservation and repeated invocation.
- [x] Compose the Mac and menu SDK sources and reproduce the merged tree by applying the published patch to a fresh pinned SDK. This checks source composition.
- [ ] Build and observe this exact branch in the game.

### **2026-11-11 23:33 -0500 — Mac and menu prompt sources combined**

The Mac contribution and menu branch merge into a separate qualification branch. Combining the actual SDK sources resolved the serialized patch conflict, and a fresh patch application reproduced the combined tree exactly. The resulting source retains the Mac audio, safe depth shaders, queue and platform changes alongside desktop input, protected reads and read-only archive selection. The existing qualified Mac checkout and build cache remain available for the owner-controlled build step. This combined branch has not been compiled or run.

### **2026-11-11 23:28 -0500 — Separate archive preparation and ordinary player selection implemented**

The preparation tool reads the owner's original archive and writes a separate prepared file in one streaming pass. It preserves existing movie tags, adjusts archive offsets when menu actions require more blocks, and verifies each changed movie after writing. A matching receipt permits safe reuse. The player checks the prepared file header before guest boot and redirects only read-only opens of the target archive across its three file-reading paths. Installation metadata and writes continue using the original paths.

A small synthetic archive passed block-growth, manifest and neighbor preservation, unchanged-original and repeated-invocation checks. Both Python sources parse. The combined SDK patch applies using the setup script's whitespace handling. Actual game archives, the extracted native build and gameplay remain unverified on this branch. [Build usage](BUILDING.md).

### **2026-11-11 22:55 -0500 — Menu prompt source checkpoint prepared**

The publisher uses the existing desktop input mode and configured bindings. The menu actions replace button images while keyboard input is active and restore their original visibility for controller input. Text fitting uses the rendered text width. The original game archive and game assets are absent from this branch. Local development candidates have displayed keyboard labels in the title and main menu, while the latest pause extension still requires gameplay verification. Those observations do not qualify this extracted branch.

The next step is archive preparation and runtime mapping, followed by the ordinary build and gameplay checks. [Operational handoff](HANDOFF.md).

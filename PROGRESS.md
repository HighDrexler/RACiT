# Keyboard menu prompts

This branch extends the desktop input contribution with keyboard labels for image-based menu prompts. Source implementation now includes archive preparation and selection through the ordinary player entry point. Native build and gameplay qualification of this branch remain pending.

- [x] Implement frame publishing for the BCUS98124 disc 1.00 frontend and in-game screen manager.
- [x] Preserve the original controller artwork and title formatting while fitting keyboard labels to their existing space.
- [x] Include the protected guest-read prerequisite and source-derived menu actions.
- [x] Implement archive preparation and selection through the normal player entry point. A tiny synthetic archive checks block-table growth, neighboring data preservation and repeated invocation.
- [ ] Build and observe this exact branch in the game.

### **2026-11-11 23:28 -0500 — Separate archive preparation and ordinary player selection implemented**

The preparation tool reads the owner's original archive and writes a separate prepared file in one streaming pass. It preserves existing movie tags, adjusts archive offsets when menu actions require more blocks, and verifies each changed movie after writing. A matching receipt permits safe reuse. The player checks the prepared file header before guest boot and redirects only read-only opens of the target archive across its three file-reading paths. Installation metadata and writes continue using the original paths.

A small synthetic archive passed block-growth, manifest and neighbor preservation, unchanged-original and repeated-invocation checks. Both Python sources parse. The combined SDK patch applies using the setup script's whitespace handling. Actual game archives, the extracted native build and gameplay remain unverified on this branch. [Build usage](BUILDING.md).

### **2026-11-11 22:55 -0500 — Menu prompt source checkpoint prepared**

The publisher uses the existing desktop input mode and configured bindings. The menu actions replace button images while keyboard input is active and restore their original visibility for controller input. Text fitting uses the rendered text width. The original game archive and game assets are absent from this branch. Local development candidates have displayed keyboard labels in the title and main menu, while the latest pause extension still requires gameplay verification. Those observations do not qualify this extracted branch.

The next step is archive preparation and runtime mapping, followed by the ordinary build and gameplay checks. [Operational handoff](HANDOFF.md).

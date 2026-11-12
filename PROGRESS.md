# Keyboard menu prompts

This branch extends the desktop input contribution with keyboard labels for image-based menu prompts. The source checkpoint includes the movie publisher and menu code. Preparing and selecting a patched copy of the owner's game archive remains unfinished.

- [x] Implement frame publishing for the BCUS98124 disc 1.00 frontend and in-game screen manager.
- [x] Preserve the original controller artwork and title formatting while fitting keyboard labels to their existing space.
- [x] Include the protected guest-read prerequisite and source-derived menu actions.
- [ ] Integrate archive preparation and selection through the normal player entry point.
- [ ] Build and observe this exact branch in the game.

### **2026-11-11 22:55 -0500 — Menu prompt source checkpoint prepared**

The publisher uses the existing desktop input mode and configured bindings. The menu actions replace button images while keyboard input is active and restore their original visibility for controller input. Text fitting uses the rendered text width. The original game archive and game assets are absent from this branch. Local development candidates have displayed keyboard labels in the title and main menu, while the latest pause extension still requires gameplay verification. Those observations do not qualify this extracted branch.

The next step is archive preparation and runtime mapping, followed by the ordinary build and gameplay checks. [Operational handoff](HANDOFF.md).

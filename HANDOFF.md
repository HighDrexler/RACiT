# Keyboard menu prompt source checkpoint

The branch starts at desktop input `e3919cd1ef68d7c69fbb9f35f9fa9291648fba91` and merges protected reads `34b12ccf8590872164e1e56a225db8e056890ead`. The merge conflict affected only the loader patch's blob identity line. Both sets of source hunks remain present. Patch syntax parses, but applying this combined patch to a fresh SDK and compiling it remain pending.

`game/menu_keycaps.h` includes the prerequisite's `ppu_stack_read.h` through the existing SDK include directory. It preserves the complete guest context and continuation around each movie variable assignment. The disc frontend hook is `004C6F6C`, with the movie at owner plus 4. The in-game screen manager hook is `004ADB3C`, with its primary movie at owner plus 8 and optional companion at plus 4. These are disc 1.00 addresses. Update 1.20 hooks belong in the separate version-support integration.

`scripts/patch_lift.py` inserts these hooks through its ordinary generated-source patch pass. The AS2 action must also be inserted after each participating movie's first-frame class definitions and before its first ShowFrame tag. That archive preparation step and a runtime mapping to the prepared archive are still missing. This branch alone therefore does not deliver visible menu keycaps.

`game/menu_keycaps.actions.bin` contains only compiled actions from the adjacent owned AS2 source. It contains no extracted game tags, images, fonts or sounds. FFDec 26.4.0 compiled a minimal synthetic SWF containing one frame, and the resulting DoAction payload was extracted. The compilation used `-importScript seed.swf compiled.swf scripts`, with the source under `scripts/frame_1/DoAction.as`. The seed container is disposable and is excluded from this branch. Source changes require recompiling this payload before archive preparation.

The source SHA-256 is `a647e066c7611e4725b6f25c4d20746e6854618450bf392549d90a75042a9e15`. The 3,443-byte action payload SHA-256 is `2edc51a2e75270b7f2df9850fbca6245ad8841307a4d4d7013132cae26e991d3`. The compiler JAR SHA-256 is `80f8ced0bfde98b434c28e9807e91fe5558255b2f712051a0e65221519bd392a`. The source hash matches the retained compilation receipt.

Checks here cover source inspection, patch syntax and whitespace only. This exact branch has no compile or game result. No background processes or build outputs were created for this checkpoint. Preserve the branch while completing the archive integration. [Owner progress](PROGRESS.md).

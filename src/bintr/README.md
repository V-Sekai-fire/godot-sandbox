# Binary translations

Files put into this folder are automatically embedded into the Godot Sandbox extension

- `*.cpp` are compiled with the extension (for example the self-registering
  form a Sandbox emits).
- `<platform>.<arch>/bintr-*.o` are linked as they are into that target's
  build only: objects compiled ahead of time for one target, such as
  `web.wasm32/bintr-<HASH>.<i>.o` units and the `bintr-<HASH>.register.o` that
  registers them. Each target needs its own, because the host's machine
  layout is part of the translation and its hash. Put only objects there,
  never a registration source: every `*.cpp` here is compiled for every target.

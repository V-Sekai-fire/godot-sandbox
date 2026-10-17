# Binary translations

Files put into this folder are automatically embedded into the Godot Sandbox extension

- `*.cpp` are compiled with the extension (for example the self-registering
  form a Sandbox emits).
- `bintr-*.o` are linked as they are: objects compiled ahead of time for one
  target, such as a web build's `bintr-<HASH>.<i>.o` units and the
  `bintr-<HASH>.register.o` that registers them. Put only objects here for
  such a translation, never its registration source: CMake compiles every
  `*.cpp` here, and a web-only registration fails a native link.

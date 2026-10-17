# Binary translations

Files put into this folder are automatically embedded into the Godot Sandbox extension

- `*.cpp` are compiled with the extension (for example the self-registering
  form a Sandbox emits).
- `bintr-*.o` are linked as they are: units compiled ahead of time for one target,
  such as a web build's translation units plus their `*_register.cpp`.

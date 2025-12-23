#pragma once

#include "script_elf.h"
#include "../sandbox.h"

using namespace godot;

/// GDEScript - A unique script type for .gde extension files
/// .gde files contain textual GDScript code that is compiled to ELF and executed in the sandbox
/// Extends ELFScript but handles compilation from GDScript source
class GDEScript : public ELFScript {
	GDCLASS(GDEScript, ELFScript);

protected:
	static void _bind_methods();
	String gdscript_source_code;  // The original GDScript source
	PackedByteArray compiler_elf_buffer;  // Cached compiler ELF
	static String compiler_elf_path;  // Runtime-configurable path to compiler ELF

	/// Compile GDScript source to ELF binary
	PackedByteArray _compile_gdscript_to_elf(const String &p_gdscript_code);
	
	/// Load compiler ELF buffer
	void _load_compiler_elf();
	
	/// Set the path to the compiler ELF (runtime override)
	static void set_compiler_elf_path(const String &p_path);
	
	/// Get the current compiler ELF path
	static String get_compiler_elf_path();

public:
	/// Get the original GDScript source code
	String get_gdscript_source() const { return gdscript_source_code; }
	
	/// Set the GDScript source code (will be compiled on reload)
	void set_gdscript_source(const String &p_source);
	
	/// Override set_file to handle textual GDScript
	void set_file(const String &p_path);

	GDEScript() {}
	~GDEScript() {}
};


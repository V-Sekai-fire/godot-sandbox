#include "script_gde.h"
#include "script_instance.h"
#include "../sandbox.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/binder_common.hpp>

// Static member initialization
String GDEScript::compiler_elf_path = "res://addons/gdscript_compiler/unittests.elf";

void GDEScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_gdscript_source"), &GDEScript::get_gdscript_source);
	ClassDB::bind_method(D_METHOD("set_gdscript_source", "source"), &GDEScript::set_gdscript_source);
	ClassDB::bind_static_method("GDEScript", D_METHOD("set_compiler_elf_path", "path"), &GDEScript::set_compiler_elf_path);
	ClassDB::bind_static_method("GDEScript", D_METHOD("get_compiler_elf_path"), &GDEScript::get_compiler_elf_path);
}

void GDEScript::set_gdscript_source(const String &p_source) {
	gdscript_source_code = p_source;
	// Compile and update the ELF content
	PackedByteArray compiled_elf = _compile_gdscript_to_elf(p_source);
	if (!compiled_elf.is_empty()) {
		source_code = compiled_elf;
		source_version++;
	}
}

void GDEScript::set_file(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}
	
	// Store the path
	this->path = String(p_path);
	CharString resless_path = p_path.replace("res://", "").utf8();
	this->std_path = std::string(resless_path.ptr(), resless_path.length());
	
	// Read the file as text (GDScript source)
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (!file.is_valid()) {
		ERR_FAIL_MSG("GDEScript::set_file: Failed to load file '" + p_path + "'");
		return;
	}
	
	// Read as text (GDScript source code)
	gdscript_source_code = file->get_as_text();
	file->close();
	
	if (gdscript_source_code.is_empty()) {
		ERR_FAIL_MSG("GDEScript::set_file: File '" + p_path + "' is empty");
		return;
	}
	
	// Compile GDScript to ELF
	_load_compiler_elf();
	PackedByteArray compiled_elf = _compile_gdscript_to_elf(gdscript_source_code);
	
	if (compiled_elf.is_empty()) {
		ERR_FAIL_MSG("GDEScript::set_file: Failed to compile GDScript from '" + p_path + "'");
		return;
	}
	
	// Store the compiled ELF as the source code
	source_code = compiled_elf;
	
	// Set global name
	global_name = "Sandbox_" + path.get_basename().replace("res://", "").replace("/", "_").replace("-", "_").capitalize().replace(" ", "");
	
	// Get function info from compiled ELF
	Sandbox::BinaryInfo info = Sandbox::get_program_info_from_binary(source_code);
	this->function_names = std::move(info.functions);
	this->functions.clear();
	
	this->elf_programming_language = "gdscript";  // Mark as compiled from GDScript
	this->elf_api_version = info.version;
	
	// Update sandbox instances
	for (Sandbox *sandbox : sandbox_map[path]) {
		sandbox->set_program(Ref<ELFScript>(this));
	}
	
	// Update instance methods
	if (functions.is_empty()) {
		for (ELFScriptInstance *instance : this->instances) {
			instance->update_methods();
		}
	}
}

void GDEScript::_load_compiler_elf() {
	if (!compiler_elf_buffer.is_empty()) {
		return;  // Already loaded
	}
	
	// Load from the configured path (can be overridden at runtime)
	String compiler_path = compiler_elf_path.is_empty() ? "res://addons/gdscript_compiler/unittests.elf" : compiler_elf_path;
	Ref<FileAccess> file = FileAccess::open(compiler_path, FileAccess::READ);
	if (file.is_valid()) {
		compiler_elf_buffer = file->get_buffer(file->get_length());
		file->close();
	} else {
		ERR_PRINT("GDEScript: Failed to load compiler ELF from " + compiler_path);
		ERR_PRINT("GDEScript: Please ensure the compiler ELF is available at the configured path");
		ERR_PRINT("GDEScript: You can set a custom path using GDEScript.set_compiler_elf_path(path)");
	}
}

void GDEScript::set_compiler_elf_path(const String &p_path) {
	compiler_elf_path = p_path;
	// Clear any cached compiler buffer so it will be reloaded from the new path
	// Note: This affects all GDEScript instances, but that's fine since they share the compiler
}

String GDEScript::get_compiler_elf_path() {
	return compiler_elf_path;
}

PackedByteArray GDEScript::_compile_gdscript_to_elf(const String &p_gdscript_code) {
	if (p_gdscript_code.is_empty()) {
		return PackedByteArray();
	}
	
	// Ensure compiler is loaded
	_load_compiler_elf();
	if (compiler_elf_buffer.is_empty()) {
		ERR_PRINT("GDEScript: Compiler ELF not available");
		return PackedByteArray();
	}

	// Create a temporary sandbox to compile the GDScript
	Sandbox *compiler_sandbox = memnew(Sandbox);
	compiler_sandbox->load_buffer(compiler_elf_buffer);
	
	// Compile the GDScript to ELF using vmcall
	// vmcall takes: first arg is function name, rest are function arguments
	Variant function_name = Variant("compile_to_elf");
	Variant gdscript_arg = Variant(p_gdscript_code);
	const Variant *args[2] = { &function_name, &gdscript_arg };
	GDExtensionCallError error;
	Variant result = compiler_sandbox->vmcall(args, 2, error);
	
	PackedByteArray compiled_elf;
	if (error.error == GDEXTENSION_CALL_OK && result.get_type() == Variant::PACKED_BYTE_ARRAY) {
		compiled_elf = result;
	} else {
		ERR_PRINT("GDEScript: compile_to_elf failed or did not return a PackedByteArray. Error: " + String::num_int64(error.error));
	}
	
	// Clean up the compiler sandbox
	memdelete(compiler_sandbox);
	
	return compiled_elf;
}


#include "resource_saver_elf.h"
#include "../register_types.h"
#include "script_elf.h"
#include "script_gde.h"
#include "script_language_elf.h"
#include <godot_cpp/classes/file_access.hpp>

Error ResourceFormatSaverELF::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<ELFScript> elf_model = Object::cast_to<ELFScript>(p_resource.ptr());
	if (elf_model.is_null()) {
		return Error::OK;
	}
	
	// Check if this is a GDEScript and we're saving to a .gde file
	Ref<GDEScript> gde_model = Object::cast_to<GDEScript>(p_resource.ptr());
	String ext = p_path.get_extension().to_lower();
	
	if (gde_model.is_valid() && ext == "gde") {
		// Save the GDScript source code for .gde files
		String source_code = gde_model->get_gdscript_source();
		if (source_code.is_empty()) {
			// If source is empty, try to reload from file first
			gde_model->set_file(p_path);
			gde_model->reload(true);
			source_code = gde_model->get_gdscript_source();
		}
		
		if (!source_code.is_empty()) {
			Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
			if (file.is_valid()) {
				file->store_string(source_code);
				file->close();
				// Update the file reference
				gde_model->set_file(p_path);
				return Error::OK;
			} else {
				return Error::ERR_FILE_CANT_WRITE;
			}
		}
	}
	
	// For .elf files or if source is not available, revert instead of saving
	elf_model->set_file(p_path);
	elf_model->reload(true);
	return Error::OK;
}
Error ResourceFormatSaverELF::_set_uid(const String &p_path, int64_t p_uid) {
	return Error::OK;
}
bool ResourceFormatSaverELF::_recognize(const Ref<Resource> &p_resource) const {
	// Recognize both ELFScript and GDEScript
	return Object::cast_to<ELFScript>(p_resource.ptr()) != nullptr;
}
PackedStringArray ResourceFormatSaverELF::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray array;
	if (Object::cast_to<ELFScript>(p_resource.ptr()) == nullptr)
		return array;
	array.push_back("elf");
	array.push_back("gde");  // Support .gde extension files
	return array;
}
bool ResourceFormatSaverELF::_recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
	return Object::cast_to<ELFScript>(p_resource.ptr()) != nullptr;
}

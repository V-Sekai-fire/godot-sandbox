// Engine-module entry: loads the godot-sandbox GDExtension that SCsub linked into the engine.
// GDExtension::initialize_gdextensions() runs after the modules' core level, so the extension is
// loaded at the servers level; the manager then brings it up to the current level.
#include "../register_types.h"

#include "core/extension/gdextension_manager.h"

extern "C" GDExtensionBool riscv_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization);

static const char *SANDBOX_EXTENSION_PATH = "libgodot://sandbox";

void initialize_sandbox_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}
	static const GDExtensionInitializationFunction init = riscv_library_init;
	GDExtensionManager::LoadStatus status = GDExtensionManager::get_singleton()->load_extension_from_function(
			SANDBOX_EXTENSION_PATH, &init);
	ERR_FAIL_COND_MSG(status != GDExtensionManager::LOAD_STATUS_OK, "Could not load the sandbox extension.");
}

void uninitialize_sandbox_module(ModuleInitializationLevel p_level) {
}

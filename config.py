# Engine-module build of godot-sandbox (SCsub beside this file).
#
# The same tree builds two ways. SConstruct makes the GDExtension library; when the tree is copied
# into an engine as modules/sandbox, these files compile the extension and godot-cpp into the
# engine and load it through GDExtensionManager.load_extension_from_function, so the sources need
# no engine-only code path.


def can_build(env, platform):
    if platform in ("android", "ios", "visionos"):
        return False
    if env.get("use_asan", False):
        return False
    if env.get("module_mono_enabled", False):
        return False
    # libriscv's Windows support needs MinGW.
    return platform != "windows" or env.get("use_mingw", False)


def configure(env):
    # libriscv throws on guest faults, so the engine is built with exceptions.
    env["disable_exceptions"] = False


def get_doc_classes():
    # The extension registers its classes and carries its own class reference.
    return []


def get_doc_path():
    return "doc_classes"

#pragma once

#include <godot_cpp/classes/control.hpp>

#include <deque>
#include <godot_cpp/core/binder_common.hpp>
#include <libriscv/machine.hpp>
#include <optional>

using namespace godot;
#define RISCV_ARCH riscv::RISCV64
using gaddr_t = riscv::address_type<RISCV_ARCH>;
using machine_t = riscv::Machine<RISCV_ARCH>;
#include "elf/script_elf.h"
#include "vmcallable.h"
#include "vmproperty.h"

/**
 * @brief The Sandbox class is a Godot node that provides a safe environment for running untrusted code.
 *
 * The sandbox is constructed with a program, which is a 64-bit RISC-V ELF executable file that contains functions and code to be executed.
 * Programs are loaded into the sandbox using the `set_program` method.
 * Upon setting a program, the sandbox will load the program into memory and initialize the RISC-V machine in several steps:
 * 1. Remove old machine instance, if any.
 * 2. Create a new machine instance with the given program.
 * 3. Set up system calls, native heap and native memory syscalls.
 * 4. Set up the Linux environment for the program.
 * 5. Run the program through to its main() function.
 * 6. Read the program's properties. These will be visible to the Godot editor.
 * 7. Pre-cache some public functions. These will be available to call from GDScript.
 **/
class Sandbox : public Node {
	GDCLASS(Sandbox, Node);

protected:
	static void _bind_methods();

	String _to_string() const;

public:
	static constexpr unsigned MAX_INSTRUCTIONS = 16; // Billions
	static constexpr unsigned MAX_HEAP = 16ul; // MBs
	static constexpr unsigned MAX_VMEM = 16ul; // MBs
	static constexpr unsigned MAX_LEVEL = 8; // Maximum call recursion depth
	static constexpr unsigned EDITOR_THROTTLE = 8; // Throttle VM calls from the editor
	static constexpr unsigned MAX_PROPERTIES = 16; // Maximum number of sandboxed properties

	Sandbox();
	~Sandbox();

	// -= VM function calls =-

	/// @brief Make a function call to a function in the guest by its name.
	/// @param args The arguments to pass to the function, where the first argument is the name of the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcall(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	/// @brief Make a function call to a function in the guest by its name. Always use Variant values for arguments.
	/// @param args The arguments to pass to the function, where the first argument is the name of the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcallv(const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);
	/// @brief Make a function call to a function in the guest by its name.
	/// @param function The name of the function to call.
	/// @param args The arguments to pass to the function.
	/// @param arg_count The number of arguments.
	/// @return The return value of the function call.
	Variant vmcall_fn(const StringName &function, const Variant **args, GDExtensionInt arg_count);
	/// @brief Make a function call to a function in the guest by its guest address.
	/// @param address The address of the function to call.
	/// @param args The arguments to pass to the function.
	/// @param arg_count The number of arguments.
	/// @param error The error code, if any.
	/// @return The return value of the function call.
	Variant vmcall_address(gaddr_t address, const Variant **args, GDExtensionInt arg_count, GDExtensionCallError &error);

	/// @brief Make a function call to a function in the guest by its name.
	/// @param function The name of the function to call.
	/// @param args The arguments to pass to the function.
	/// @return The return value of the function call.
	/// @note The extra arguments are saved in the callable object, and will be passed to the function when it is called
	/// in front of the arguments passed to the call() method. So, as an example, if you have a function that takes 3 arguments,
	/// and you call it with 2 arguments, you can later call the callable object with one argument, which turns into the 3rd argument.
	Variant vmcallable(String function, Array args);
	Variant vmcallable_address(uint64_t address, Array args);

	/// @brief Set whether to prefer register values for VM function calls.
	/// @param use_native_args True to prefer register values, false to prefer Variant values.
	void set_use_native_args(bool use_native_args) { m_use_native_args = use_native_args; }
	/// @brief Get whether to prefer register values for VM function calls.
	/// @return True if register values are preferred, false if Variant values are preferred.
	bool get_use_native_args() const { return m_use_native_args; }

	// -= Sandbox Properties =-

	uint32_t get_max_refs() const { return m_max_refs; }
	void set_max_refs(uint32_t max) { m_max_refs = max; }
	void set_memory_max(uint32_t max) { m_memory_max = max; }
	uint32_t get_memory_max() const { return m_memory_max; }
	void set_instructions_max(int64_t max) { m_insn_max = max; }
	int64_t get_instructions_max() const { return m_insn_max; }
	void set_heap_usage(int64_t) {} // Do nothing (it's a read-only property)
	int64_t get_heap_usage() const;
	void set_budget_overruns(unsigned budget) {} // Do nothing (it's a read-only property)
	unsigned get_budget_overruns() const { return m_budget_overruns; }
	void set_calls_made(unsigned calls) {} // Do nothing (it's a read-only property)
	unsigned get_calls_made() const { return m_calls_made; }

	static uint64_t get_global_budget_overruns() { return m_global_budget_overruns; }
	static uint64_t get_global_exceptions() { return m_global_exceptions; }
	static uint64_t get_global_calls_made() { return m_global_calls_made; }

	// -= Address Lookup =-

	gaddr_t address_of(std::string_view name) const;
	gaddr_t cached_address_of(int64_t hash) const;
	gaddr_t cached_address_of(int64_t hash, const String &name) const;

	/// @brief Check if a function exists in the guest program.
	/// @param p_function The name of the function to check.
	/// @return True if the function exists, false otherwise.
	bool has_function(const StringName &p_function) const;

	// -= Call State Management =-

	/// @brief Get the current call state.
	/// @return The current call state.
	/// @note The call state is a stack of states, with the current state stored in m_current_state.
	auto &state() const { return *m_current_state; }
	auto &state() { return *m_current_state; }

	/// @brief Set the current tree base, which is the node that the sandbox will use for accessing the node tree.
	/// @param tree_base The tree base node.
	/// @note The tree base is the owner node that the sandbox will use to access the node tree. When scripts
	/// try to access the node path ".", they will be accessing this node, and navigating relative to it.
	void set_tree_base(godot::Node *tree_base) { this->m_tree_base = tree_base; }
	godot::Node *get_tree_base() const { return this->m_tree_base; }

	// -= Scoped objects and variants =-

	/// @brief Add a scoped variant to the current state.
	/// @param var The variant to add.
	/// @return The index of the added variant, passed to and used by the guest.
	unsigned add_scoped_variant(const Variant *var) const;

	/// @brief Create a new scoped variant, storing it in the current state.
	/// @param var The variant to add.
	/// @return The index of the added variant, passed to and used by the guest.
	unsigned create_scoped_variant(Variant &&var) const;

	/// @brief Get a scoped variant by its index.
	/// @param idx The index of the variant to get.
	/// @return The variant, or an empty optional if the index is invalid.
	std::optional<const Variant *> get_scoped_variant(unsigned idx) const noexcept;

	/// @brief Get a mutable scoped variant by its index.
	/// @param idx The index of the variant to get.
	/// @return The variant.
	Variant &get_mutable_scoped_variant(unsigned idx);

	/// @brief Add a scoped object to the current state.
	/// @param ptr The pointer to the object to add.
	void add_scoped_object(const void *ptr);

	/// @brief Remove a scoped object from the current state.
	/// @param ptr The pointer to the object to remove.
	void rem_scoped_object(const void *ptr) { state().scoped_objects.erase(std::remove(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)), state().scoped_objects.end()); }

	/// @brief Check if an object is scoped in the current state.
	/// @param ptr The pointer to the object to check.
	/// @return True if the object is scoped, false otherwise.
	bool is_scoped_object(const void *ptr) const noexcept { return state().scoped_objects.end() != std::find(state().scoped_objects.begin(), state().scoped_objects.end(), reinterpret_cast<uintptr_t>(ptr)); }

	// -= Sandboxed Properties =-
	// These are properties that are exposed to the Godot editor, provided by the guest program.

	/// @brief Add a property to the sandbox.
	/// @param name The name of the property.
	/// @param vtype The type of the property.
	/// @param setter The guest address of the setter function.
	/// @param getter The guest address of the getter function.
	/// @param def The default value of the property.
	void add_property(const String &name, Variant::Type vtype, uint64_t setter, uint64_t getter, const Variant &def = "") const;

	/// @brief Set a property in the sandbox.
	/// @param name The name of the property.
	/// @param value The new value to set.
	bool set_property(const StringName &name, const Variant &value);

	/// @brief Get a property from the sandbox.
	/// @param name The name of the property.
	/// @param r_ret The current value of the property.
	bool get_property(const StringName &name, Variant &r_ret);

	/// @brief Find a property in the sandbox, or return null if it does not exist.
	/// @param name The name of the property.
	/// @return The property, or null if it does not exist.
	const SandboxProperty *find_property_or_null(const StringName &name) const;

	/// @brief Get all sandboxed properties.
	/// @return The array of sandboxed properties.
	std::vector<SandboxProperty> &get_properties() { return m_properties; }
	const std::vector<SandboxProperty> &get_properties() const { return m_properties; }

	// -= Program management & public functions =-

	/// @brief Check if a program has been loaded into the sandbox.
	/// @return True if a program has been loaded, false otherwise.
	bool has_program_loaded() const;
	/// @brief Set the program to run in the sandbox.
	/// @param program The program to load and run.
	void set_program(Ref<ELFScript> program);
	/// @brief Get the program loaded into the sandbox.
	/// @return The program loaded into the sandbox.
	Ref<ELFScript> get_program();

	/// @brief Get the public functions available to call in the guest program.
	/// @return Array of public callable functions.
	PackedStringArray get_functions() const;
	struct BinaryInfo {
		String language;
		PackedStringArray functions;
		int version = 0;
	};
	/// @brief Get information about the program from the binary.
	/// @param binary The binary data.
	/// @return An array of public callable functions and programming language.
	static BinaryInfo get_program_info_from_binary(const PackedByteArray &binary);

	// -= Self-testing and internal functions =-

	void assault(const String &test, int64_t iterations);
	void print(std::string_view text);
	Variant vmcall_internal(gaddr_t address, const Variant **args, int argc);
	machine_t &machine() { return *m_machine; }
	const machine_t &machine() const { return *m_machine; }

private:
	void load(PackedByteArray &&vbuf, const std::vector<std::string> *argv = nullptr);
	void read_program_properties(bool editor) const;
	void handle_exception(gaddr_t);
	void handle_timeout(gaddr_t);
	void print_backtrace(gaddr_t);
	void initialize_syscalls();
	GuestVariant *setup_arguments(gaddr_t &sp, const Variant **args, int argc);
	void setup_arguments_native(gaddr_t arrayDataPtr, GuestVariant *v, const Variant **args, int argc);

	Ref<ELFScript> m_program_data;
	machine_t *m_machine = nullptr;
	godot::Node *m_tree_base = nullptr;
	PackedByteArray m_binary;
	uint32_t m_max_refs = 100;
	uint32_t m_memory_max = MAX_VMEM;
	int64_t m_insn_max = MAX_INSTRUCTIONS;

	mutable std::unordered_map<int64_t, gaddr_t> m_lookup;

	bool m_last_newline = false;
	uint8_t m_throttled = 0;
	uint8_t m_level = 1; // Current call level (0 is for initialization)
	bool m_use_native_args = false;

	// Stats
	unsigned m_budget_overruns = 0;
	unsigned m_exceptions = 0;
	unsigned m_calls_made = 0;

	struct CurrentState {
		std::vector<Variant> variants;
		std::vector<const Variant *> scoped_variants;
		std::vector<uintptr_t> scoped_objects;

		void reset(unsigned max_refs) {
			variants.reserve(max_refs);
			variants.clear();
			scoped_variants.clear();
			scoped_objects.clear();
		}
	};
	CurrentState *m_current_state = nullptr;
	// State stack, with the permanent (initial) state at index 0.
	// That means eg. static Variant values are held stored in the state at index 0,
	// so that they can be accessed by future VM calls, and not lost when a call ends.
	std::array<CurrentState, MAX_LEVEL+1> m_states;

	// Properties
	mutable std::vector<SandboxProperty> m_properties;

	// Global statistics
	static inline uint64_t m_global_budget_overruns = 0;
	static inline uint64_t m_global_exceptions = 0;
	static inline uint64_t m_global_calls_made = 0;
};

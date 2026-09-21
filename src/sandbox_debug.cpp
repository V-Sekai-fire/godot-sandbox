#include "sandbox.h"

#include <godot_cpp/variant/utility_functions.hpp>

Array Sandbox::get_general_registers() const {
	Array ret;
	for (int i = 0; i < 32; i++) {
		ret.push_back(m_machine->cpu.reg(i));
	}
	return ret;
}

Array Sandbox::get_floating_point_registers() const {
	Array ret;
	for (int i = 0; i < 32; i++) {
		auto &freg = m_machine->cpu.registers().getfl(i);
		// We suspect that it's a 32-bit float if the upper 32 bits are zero
		if (freg.i32[1] == 0) {
			ret.push_back(freg.f32[0]);
		} else {
			ret.push_back(freg.f64);
		}
	}
	return ret;
}

void Sandbox::set_argument_registers(Array args) {
	if (args.size() > 8) {
		ERR_PRINT("set_argument_registers() can only set up to 8 arguments.");
		return;
	}
	for (int i = 0; i < args.size(); i++) {
		m_machine->cpu.reg(i + 10) = args[i].operator int64_t();
	}
}

String Sandbox::get_current_instruction() const {
	std::string instr = m_machine->cpu.current_instruction_to_string();
	return String(instr.c_str());
}

void Sandbox::make_resumable() {
	if (!m_machine->memory.binary().empty()) {
		ERR_PRINT("Sandbox: Cannot make resumable after initialization.");
		return;
	}
	this->m_resumable_mode = true;
}

bool Sandbox::resume(uint64_t max_instructions) {
	if (!this->m_resumable_mode) {
		ERR_PRINT("Sandbox: Cannot resume after initialization.");
		return false;
	}
	if (this->m_current_state != &this->m_states[0]) {
		ERR_PRINT("Sandbox: Cannot resume while in a call.");
		this->m_resumable_mode = false; // Disable resumable mode
		return false;
	}

	const gaddr_t address = m_machine->cpu.pc();
	try {
		const bool stopped = m_machine->resume(max_instructions);

		if (stopped) {
			// If the machine stopped, we are leaving resumable mode
			// It's not available for VM calls, only during startup
			this->m_resumable_mode = false;
		}

		return stopped;

	} catch (const std::exception &e) {
		this->m_resumable_mode = false;
		this->handle_exception(address);
		return true; // Can't (shouldn't) be resumed anymore
	}
}

PackedByteArray Sandbox::save_state() const {
	PackedByteArray result;
	if (this->m_machine == nullptr) {
		ERR_PRINT("Sandbox: No machine to save.");
		return result;
	}
	// libriscv refuses to serialize a flat read-write arena. Say which setting
	// is responsible rather than letting the exception read as a bug.
	if (riscv::flat_readwrite_arena) {
		ERR_PRINT("Sandbox: Cannot save state: built with RISCV_FLAT_RW_ARENA. "
				  "A paged machine is required for serialization.");
		return result;
	}

	std::vector<uint8_t> image;
	try {
		this->m_machine->serialize_to(image);
	} catch (const std::exception &e) {
		ERR_PRINT(String("Sandbox: serialize_to failed: ") + String(e.what()));
		return result;
	}

	// An image with no pages restores into a machine with no memory, which runs
	// wrong instead of failing. Refuse to hand one out.
	if (image.empty()) {
		ERR_PRINT("Sandbox: serialize_to produced an empty image.");
		return result;
	}

	result.resize(image.size());
	std::memcpy(result.ptrw(), image.data(), image.size());
	return result;
}

bool Sandbox::restore_state(const PackedByteArray &p_image) {
	if (this->m_machine == nullptr) {
		ERR_PRINT("Sandbox: No machine to restore into.");
		return false;
	}
	if (p_image.is_empty()) {
		ERR_PRINT("Sandbox: Cannot restore from an empty image.");
		return false;
	}
	if (this->m_current_state != &this->m_states[0]) {
		ERR_PRINT("Sandbox: Cannot restore while in a call.");
		return false;
	}

	const std::vector<uint8_t> image(p_image.ptr(), p_image.ptr() + p_image.size());
	int rc = -1;
	try {
		rc = this->m_machine->deserialize_from(image);
	} catch (const std::exception &e) {
		ERR_PRINT(String("Sandbox: deserialize_from failed: ") + String(e.what()));
		return false;
	}
	if (rc != 0) {
		ERR_PRINT(String("Sandbox: deserialize_from rejected the image, code ") + String::num_int64(rc));
		return false;
	}

	// The image carries no host state, so anything host-side that referred to
	// the old machine is now stale and must go rather than be reused.
	for (auto &state : this->m_states) {
		state.reset();
	}
	this->m_current_state = &this->m_states[0];
	return true;
}

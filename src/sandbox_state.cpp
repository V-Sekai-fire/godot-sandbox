#include "sandbox.h"

#include <cstring>
#include <godot_cpp/variant/utility_functions.hpp>

// Whole-machine snapshots. A bare libriscv image says nothing about which
// program produced it, so the envelope below binds one to its program; without
// that, restoring over unrelated code runs garbage instead of refusing.

namespace {

constexpr uint32_t STATE_MAGIC = 0x53425853; // "SBXS"
constexpr uint32_t STATE_VERSION = 1;

struct StateHeader {
	uint32_t magic;
	uint32_t version;
	uint64_t program_hash;
};

// FNV-1a over the loaded ELF: identity, not a security boundary.
uint64_t hash_bytes(const uint8_t *data, size_t len) {
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= uint64_t(data[i]);
		h *= 1099511628211ULL;
	}
	return h;
}

} // namespace

uint64_t Sandbox::state_program_hash() const {
	if (this->m_machine == nullptr) {
		return 0;
	}
	const auto &binary = machine().memory.binary();
	if (binary.empty()) {
		return 0;
	}
	return hash_bytes(reinterpret_cast<const uint8_t *>(binary.data()), binary.size());
}

bool Sandbox::can_save_state() const {
	if (this->m_machine == nullptr) {
		return false;
	}
	if (machine().memory.binary().empty()) {
		return false;
	}
	// A suspended coroutine's frame refers to live Godot ObjectIDs, which no
	// image can carry, so it is refused rather than silently losing them.
	if (this->get_coroutine_count() > 0) {
		return false;
	}
	// The call-state stack is host-side and would disagree after a restore.
	if (this->m_current_state != &this->m_states[0]) {
		return false;
	}
	// Permanent handles live in guest memory as slot+generation indices into
	// this process's table. An image carrying them cannot be rebound elsewhere:
	// the indices would resolve to different objects rather than fail.
	if (!this->m_states[0].scoped_variants.empty()) {
		return false;
	}
	return true;
}

PackedByteArray Sandbox::save_state() const {
	PackedByteArray result;
	if (this->m_machine == nullptr) {
		ERR_PRINT("Sandbox: No machine to save.");
		return result;
	}
	if (machine().memory.binary().empty()) {
		ERR_PRINT("Sandbox: Cannot save state: no program is loaded.");
		return result;
	}
	if (this->get_coroutine_count() > 0) {
		ERR_PRINT("Sandbox: Cannot save state while coroutines are suspended: "
				  "their frames refer to host objects that no image can carry.");
		return result;
	}
	if (this->m_current_state != &this->m_states[0]) {
		ERR_PRINT("Sandbox: Cannot save state while inside a call.");
		return result;
	}
	if (!this->m_states[0].scoped_variants.empty()) {
		ERR_PRINT("Sandbox: Cannot save state while permanent handles are held: "
				  "the guest stores them as indices into this process's table, "
				  "which would resolve to different objects after a restore.");
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

	StateHeader header{};
	header.magic = STATE_MAGIC;
	header.version = STATE_VERSION;
	header.program_hash = this->state_program_hash();

	result.resize(int64_t(sizeof(header) + image.size()));
	std::memcpy(result.ptrw(), &header, sizeof(header));
	std::memcpy(result.ptrw() + sizeof(header), image.data(), image.size());
	return result;
}

bool Sandbox::restore_state(const PackedByteArray &p_image) {
	if (this->m_machine == nullptr) {
		ERR_PRINT("Sandbox: No machine to restore into.");
		return false;
	}
	if (size_t(p_image.size()) <= sizeof(StateHeader)) {
		ERR_PRINT("Sandbox: Cannot restore: image is too small to hold a header.");
		return false;
	}
	if (this->m_current_state != &this->m_states[0]) {
		ERR_PRINT("Sandbox: Cannot restore while inside a call.");
		return false;
	}
	if (this->get_coroutine_count() > 0) {
		ERR_PRINT("Sandbox: Cannot restore while coroutines are suspended.");
		return false;
	}
	if (!this->m_states[0].scoped_variants.empty()) {
		ERR_PRINT("Sandbox: Cannot restore while permanent handles are held.");
		return false;
	}

	StateHeader header{};
	std::memcpy(&header, p_image.ptr(), sizeof(header));
	if (header.magic != STATE_MAGIC) {
		ERR_PRINT("Sandbox: Cannot restore: not a sandbox state image.");
		return false;
	}
	if (header.version != STATE_VERSION) {
		ERR_PRINT(String("Sandbox: Cannot restore: image version ") + String::num_uint64(header.version) +
				  String(", expected ") + String::num_uint64(STATE_VERSION) + String("."));
		return false;
	}
	// The decisive check. Without it an image loads over an unrelated program
	// and the guest runs garbage rather than refusing.
	const uint64_t current = this->state_program_hash();
	if (header.program_hash != current) {
		ERR_PRINT("Sandbox: Cannot restore: image was saved from a different program.");
		return false;
	}

	const uint8_t *body = p_image.ptr() + sizeof(header);
	const size_t body_size = size_t(p_image.size()) - sizeof(header);
	const std::vector<uint8_t> image(body, body + body_size);

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

	// Per-call states only: level 0 was required empty above, so nothing the
	// host established is discarded here.
	for (auto &state : this->m_states) {
		state.reset();
	}
	this->m_perm_slots.clear();
	this->m_current_state = &this->m_states[0];
	this->m_program_generation++;
	return true;
}

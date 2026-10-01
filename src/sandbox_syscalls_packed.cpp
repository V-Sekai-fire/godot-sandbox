// Bulk access to a Packed*Array for compiled GDScript, after JNI's
// Get<Type>ArrayElements / Release<Type>ArrayElements pair:
//
//   ECALL_PACKED_ACQUIRE(GuestVariant *subject, int type, gaddr_t desc, int64_t expected)
//     Copies the array `subject` refers to into guest memory (the native heap
//     arena) in one call and fills the descriptor at `desc`. Returns 0 when it
//     did, 1 when it declined because `expected` element accesses do not pay
//     for copying the array (expected < 0 means "unknown, always copy"), 2 when
//     the Variant is not a Packed*Array of `type`, and 3 when there is no room.
//     Anything but 0 leaves the descriptor untouched: the guest keeps using the
//     per-element calls. (A host without these calls answers -ENOSYS, which the
//     guest treats the same way.)
//
//   ECALL_PACKED_RELEASE(GuestVariant *subject, int type, gaddr_t desc)
//     If the guest wrote the copy -- PACKED_WRITTEN in `type`, or the dirty word
//     of the descriptor set (JNI_COMMIT) -- writes it back into the
//     SAME host array, resizing it first when the length differs. That is the
//     array's own storage, reached through the Variant's internal pointer, so
//     every Variant that shares it -- another variable, a container slot, the
//     caller of vmcall() -- sees the writes exactly as if they had been made one
//     element at a time; ECALL_VSTORE would instead install a new array in this
//     Variant only. A clean copy (JNI_ABORT) is just dropped. The copy is freed
//     either way and the descriptor cleared, so a second release is harmless.
//
// PACKED_WRITTEN (0x100) may be or'ed into `type` of either call: the guest
// says up front that it writes the copy, so it needs no store to the dirty word
// per element, and an exception that unwinds the region stores it too.
//
// Descriptor, guest memory, little-endian, 32 bytes:
//   +0 data pointer, +8 element count, +16 identity, +24 dirty flag
// The identity is a keyed hash of the shared storage: equal for two Variants
// that alias one array, unrelated otherwise, and no host address is exposed.
// The guest compares identities to refuse a fast path in which an array it
// writes could alias another one it copied.
//
// An exception (a host call failing, a guest assert, the instruction limit)
// unwinds a region without its release. The host remembers every copy it has
// handed out and not had back, and handle_exception() stores the dirty ones
// and frees them all (commit_packed_acquisitions), so the arrays end up as
// element-by-element access would have left them and the arena does not leak.
#include "guest_datatypes.h"
#include "syscalls.h"

#include <godot_cpp/variant/variant.hpp>
#include <cstring>
#include <random>
#include "syscalls_helpers.hpp"

// As in sandbox_syscalls.cpp: charge the instruction budget unless profiling.
#define PENALIZE(x) \
	if (!emu.get_profiling()) { \
		machine.penalize(x); \
	}

namespace riscv {

namespace {

static constexpr int64_t MAX_PACKED_ELEMENTS = 16'777'216;
// Copying an element costs a small fraction of one host call: copy the array
// when at least one access per this many elements is expected.
static constexpr int64_t ELEMENTS_PER_ACCESS = 64;

struct PackedDescriptor {
	uint64_t data;
	uint64_t size;
	uint64_t identity;
	uint64_t dirty;
};
static_assert(sizeof(PackedDescriptor) == 32, "PackedDescriptor is guest ABI");

uint64_t packed_identity(const void *storage) {
	static const uint64_t key = [] {
		std::random_device rd;
		return (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ 0x9E3779B97F4A7C15ull;
	}();
	uint64_t x = uint64_t(uintptr_t(storage)) ^ key; // splitmix64 finalizer
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

// The engine's own Packed*Array inside a Variant, shared by every copy of that
// Variant, or null when the Variant is not of `type`.
void *packed_storage(const Variant &var, int type) {
	if (int(variant_type(var)) != type) {
		return nullptr;
	}
	const GDExtensionVariantGetInternalPtrFunc getter =
			internal::gdextension_interface_variant_get_ptr_internal_getter(GDExtensionVariantType(type));
	return getter((GDExtensionVariantPtr)var._native_ptr());
}

template <typename Packed, typename Element>
int64_t acquire_typed(machine_t &machine, void *storage, gaddr_t desc, int64_t expected, PackedDescriptor &out) {
	const Packed &array = *reinterpret_cast<const Packed *>(storage);
	const int64_t size = array.size();
	if (expected >= 0 && size > expected * ELEMENTS_PER_ACCESS) {
		return 1;
	}
	if (size > MAX_PACKED_ELEMENTS || !machine.has_arena()) {
		return 3;
	}
	const size_t bytes = size_t(size) * sizeof(Element);
	gaddr_t data = 0;
	if (bytes > 0) {
		data = machine.arena().malloc(bytes);
		if (data == 0) {
			return 3;
		}
		machine.memory.memcpy(data, array.ptr(), bytes);
	}
	out = PackedDescriptor{ data, uint64_t(size), packed_identity(storage), 0 };
	machine.memory.memcpy(desc, &out, sizeof(out));
	return 0;
}

template <typename Packed, typename Element>
void release_typed(machine_t &machine, void *storage, const PackedDescriptor &descriptor) {
	if (descriptor.size > uint64_t(MAX_PACKED_ELEMENTS)) {
		throw std::runtime_error("PackedArray release: too many elements");
	}
	Packed &array = *reinterpret_cast<Packed *>(storage);
	if (uint64_t(array.size()) != descriptor.size) {
		array.resize(int64_t(descriptor.size));
	}
	if (descriptor.size > 0) {
		// ptrw() on the shared storage: the engine's copy-on-write applies exactly
		// as it does for a single element set.
		machine.memory.memcpy_out(array.ptrw(), descriptor.data, size_t(descriptor.size) * sizeof(Element));
	}
}

#define PACKED_TYPES(X)                                            \
	X(Variant::PACKED_BYTE_ARRAY, PackedByteArray, uint8_t)        \
	X(Variant::PACKED_INT32_ARRAY, PackedInt32Array, int32_t)      \
	X(Variant::PACKED_INT64_ARRAY, PackedInt64Array, int64_t)      \
	X(Variant::PACKED_FLOAT32_ARRAY, PackedFloat32Array, float)    \
	X(Variant::PACKED_FLOAT64_ARRAY, PackedFloat64Array, double)   \
	X(Variant::PACKED_VECTOR2_ARRAY, PackedVector2Array, Vector2)  \
	X(Variant::PACKED_VECTOR3_ARRAY, PackedVector3Array, Vector3)  \
	X(Variant::PACKED_VECTOR4_ARRAY, PackedVector4Array, Vector4)  \
	X(Variant::PACKED_COLOR_ARRAY, PackedColorArray, Color)

// Write a dirty copy into `storage`, the array of `type` it was taken from.
void store_typed(machine_t &machine, void *storage, int type, const PackedDescriptor &descriptor) {
	switch (type) {
#define RELEASE_CASE(m_type, m_packed, m_element)                     \
	case m_type:                                                      \
		release_typed<m_packed, m_element>(machine, storage, descriptor); \
		return;
		PACKED_TYPES(RELEASE_CASE)
#undef RELEASE_CASE
		default:
			throw std::runtime_error("PackedArray release: unsupported Packed*Array type");
	}
}

} // namespace

APICALL(api_packed_acquire) {
	auto [g_subject, flagged_type, desc, expected] = machine.sysargs<const GuestVariant *, int, gaddr_t, int64_t>();
	const bool written = (flagged_type & PACKED_WRITTEN) != 0;
	const int type = flagged_type & ~PACKED_WRITTEN;
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("packed_acquire", g_subject, type, desc, expected);

	if (int(g_subject->type) != type) {
		machine.set_result(2);
		return;
	}
	const Variant *var = g_subject->toVariantPtr(emu);
	void *storage = var != nullptr ? packed_storage(*var, type) : nullptr;
	if (storage == nullptr) {
		machine.set_result(2);
		return;
	}
	PackedDescriptor descriptor{};
	int64_t status = 2;
	switch (type) {
#define ACQUIRE_CASE(m_type, m_packed, m_element)                                                \
	case m_type:                                                                                 \
		status = acquire_typed<m_packed, m_element>(machine, storage, desc, expected, descriptor); \
		break;
		PACKED_TYPES(ACQUIRE_CASE)
#undef ACQUIRE_CASE
		default:
			break;
	}
	if (status == 0) {
		emu.packed_acquired(Sandbox::PackedAcquisition{ &emu.state(), storage, type, desc,
				gaddr_t(descriptor.data), descriptor.size, written });
	}
	machine.set_result(status);
}

APICALL(api_packed_release) {
	auto [g_subject, flagged_type, desc] = machine.sysargs<const GuestVariant *, int, gaddr_t>();
	const bool written = (flagged_type & PACKED_WRITTEN) != 0;
	const int type = flagged_type & ~PACKED_WRITTEN;
	Sandbox &emu = riscv::emu(machine);
	PENALIZE(10'000);
	SYS_TRACE("packed_release", g_subject, type, desc);

	// Forgotten first: if the store below throws, handle_exception() must not try
	// it a second time.
	emu.packed_released(desc);
	PackedDescriptor descriptor;
	machine.memory.memcpy_out(&descriptor, desc, sizeof(descriptor));
	if (written || descriptor.dirty != 0) {
		const Variant *var = int(g_subject->type) == type ? g_subject->toVariantPtr(emu) : nullptr;
		void *storage = var != nullptr ? packed_storage(*var, type) : nullptr;
		if (storage == nullptr) {
			throw std::runtime_error("PackedArray release: the Variant is no longer that Packed*Array");
		}
		store_typed(machine, storage, type, descriptor);
	}
	if (descriptor.data != 0 && machine.has_arena()) {
		machine.arena().free(descriptor.data);
	}
	const PackedDescriptor cleared{ 0, 0, descriptor.identity, 0 };
	machine.memory.memcpy(desc, &cleared, sizeof(cleared));
	machine.set_result(0);
}

} // namespace riscv

void Sandbox::packed_acquired(const PackedAcquisition &acquisition) {
	// A descriptor slot is reused only after its release, except by a guest that
	// never releases: then the newer copy replaces the record, and the older one
	// is freed rather than leaked.
	for (auto it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end(); ++it) {
		if (it->descriptor == acquisition.descriptor) {
			if (it->data != 0 && it->data != acquisition.data && machine().has_arena()) {
				machine().arena().free(it->data);
			}
			m_packed_acquisitions.erase(it);
			break;
		}
	}
	m_packed_acquisitions.push_back(acquisition);
}

void Sandbox::packed_released(gaddr_t descriptor) {
	for (auto it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end(); ++it) {
		if (it->descriptor == descriptor) {
			m_packed_acquisitions.erase(it);
			return;
		}
	}
}

void Sandbox::commit_packed_acquisitions() {
	if (m_packed_acquisitions.empty()) {
		return;
	}
	// This call level and deeper: an error in a nested vmcall() leaves the regions
	// of the calls that are still running alone.
	std::vector<PackedAcquisition> unwound;
	for (auto it = m_packed_acquisitions.begin(); it != m_packed_acquisitions.end();) {
		if (it->level >= m_current_state) {
			unwound.push_back(*it);
			it = m_packed_acquisitions.erase(it);
		} else {
			++it;
		}
	}
	for (auto it = unwound.rbegin(); it != unwound.rend(); ++it) {
		try {
			riscv::PackedDescriptor descriptor;
			machine().memory.memcpy_out(&descriptor, it->descriptor, sizeof(descriptor));
			// The descriptor still describes this copy unless the guest overwrote it.
			if (descriptor.data == it->data && descriptor.size == it->size &&
					(it->written || descriptor.dirty != 0)) {
				riscv::store_typed(machine(), it->storage, it->type, descriptor);
			}
		} catch (const std::exception &e) {
			ERR_PRINT(("PackedArray: could not store a region unwound by an exception: " + std::string(e.what())).c_str());
		}
		if (it->data != 0 && machine().has_arena()) {
			try {
				machine().arena().free(it->data);
			} catch (const std::exception &) {
			}
		}
	}
}

#undef PACKED_TYPES

void Sandbox::initialize_syscalls_packed() {
	using namespace riscv;
	machine_t::install_syscall_handlers({
			{ ECALL_PACKED_ACQUIRE, api_packed_acquire },
			{ ECALL_PACKED_RELEASE, api_packed_release },
	});
}

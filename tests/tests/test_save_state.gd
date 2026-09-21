extends GutTest

# save_state()/restore_state() round trip: the guest's memory and registers are
# carried, so a restored machine continues rather than restarts.
#
# The interesting assertion is not that a round trip succeeds -- an empty image
# would also "succeed" if nothing checked it. It is that state written *before*
# the snapshot is visible *after* the restore, and that a machine which moved on
# in between is pulled back.
#
# Under RISCV_FLAT_RW_ARENA libriscv refuses to serialize at all, so every test
# here is pending rather than failing: that is a build configuration, not a bug,
# and a suite that went red on it would only teach people to skip the file.

const SOURCE := """
var counter: int = 0

func bump():
	counter += 1
	return counter

func peek():
	return counter
"""

var _sandbox: Sandbox


func before_each() -> void:
	_sandbox = Sandbox.new()
	var script := SafeGDScript.new()
	script.source_code = SOURCE
	script.reload()
	_sandbox.set_program(script)


func after_each() -> void:
	if _sandbox:
		_sandbox.queue_free()
		_sandbox = null


func _serializable() -> bool:
	# One probe decides it for the whole file: an empty image means this build
	# cannot serialize, which is the flat-arena configuration.
	return not _sandbox.save_state().is_empty()


func test_round_trip_restores_guest_memory() -> void:
	if not _serializable():
		pending("built with RISCV_FLAT_RW_ARENA; serialization is disabled")
		return

	_sandbox.vmcall("bump")
	_sandbox.vmcall("bump")
	assert_eq(_sandbox.vmcall("peek"), 2, "guest counted two bumps before the snapshot")

	var image: PackedByteArray = _sandbox.save_state()
	assert_gt(image.size(), 0, "image carries bytes")

	# Move on, so a restore that silently did nothing would be visible.
	_sandbox.vmcall("bump")
	assert_eq(_sandbox.vmcall("peek"), 3, "guest moved on after the snapshot")

	assert_true(_sandbox.restore_state(image), "restore accepted the image")
	assert_eq(_sandbox.vmcall("peek"), 2, "restore pulled the guest back to the snapshot")


func test_restored_guest_continues_rather_than_restarts() -> void:
	if not _serializable():
		pending("built with RISCV_FLAT_RW_ARENA; serialization is disabled")
		return

	_sandbox.vmcall("bump")
	var image: PackedByteArray = _sandbox.save_state()
	assert_true(_sandbox.restore_state(image))
	# A restart would answer 1 here; continuing answers 2.
	assert_eq(_sandbox.vmcall("bump"), 2, "guest resumed from its snapshot state")


# -- negative controls: each must be refused, or the checks above prove nothing --


func test_empty_image_is_refused() -> void:
	assert_false(_sandbox.restore_state(PackedByteArray()), "an empty image is not a state")


func test_truncated_image_is_refused() -> void:
	if not _serializable():
		pending("built with RISCV_FLAT_RW_ARENA; serialization is disabled")
		return

	var image: PackedByteArray = _sandbox.save_state()
	var truncated := image.slice(0, image.size() / 2)
	assert_false(_sandbox.restore_state(truncated), "half an image is refused, not half-loaded")


func test_garbage_image_is_refused() -> void:
	var garbage := PackedByteArray()
	garbage.resize(256)
	garbage.fill(0x41)
	assert_false(_sandbox.restore_state(garbage), "a bad magic is refused")

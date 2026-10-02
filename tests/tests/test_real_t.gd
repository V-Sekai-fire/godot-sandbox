extends GutTest

# Values a guest reads through the host come back as they went in whatever real_t the build
# uses, and a guest whose Variant is the other real_t's size is refused at load. On a double
# build tests.elf has to be built double too (the C++ API's DOUBLE_PRECISION option).

var Sandbox_TestsTests = load("res://tests/tests.elf")
var host_double: bool = Vector2(1.0 / 3.0, 0.0).x == 1.0 / 3.0


func test_numbers_through_a_cpp_guest():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	assert_eq(s.vmcall("test_real_t_numbers", {"i": 42, "f": 1.5}, [42, 1.5]), [42, 1.5, 42, 1.5, 42, 1.5],
			"Dictionary.get, Array.at and a method's return value")
	s.free()


func test_keys_through_to_vector():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	assert_eq(s.vmcall("test_real_t_keys", {"alpha": 1, "beta": 2.5, "gamma": "c"}), [3, "alpha", "beta", "gamma"])
	s.free()


func test_vectors_as_unboxed_arguments():
	var s = Sandbox.FromProgram(Sandbox_TestsTests)
	var third := 1.0 / 3.0
	var v2 := Vector2(third, -2.5)
	var v3 := Vector3(third, 2.0, -third)
	var v4 := Vector4(third, 2.0, 3.0, -third)
	var p := Plane(Vector3(third, 2.0, 3.0), -third)
	assert_eq(s.vmcall("test_real_t_vectors", v2, v3, v4, p, 42), [v2, v3, v4, p, 42])
	s.free()

func test_numbers_through_a_compiled_gdscript():
	var script := SafeGDScript.new()
	script.set_source_code("extends Node\n\nfunc read(d: Dictionary, a: Array) -> Array:\n" +
			"\treturn [d.get(\"i\"), d.get(\"f\"), a[0], a[1], a.front(), a.back()]\n")
	var node := Node.new()
	node.set_script(script)
	assert_eq(node.read({"i": 42, "f": 1.5}, [42, 1.5]), [42, 1.5, 42, 1.5, 42, 1.5])
	node.free()


func test_a_guest_whose_variant_is_the_other_size_is_refused():
	var bytes: PackedByteArray = Sandbox_TestsTests.get_content()
	var at := _variant_size_at(bytes)
	assert_gt(at, -1, "the C++ API names its Variant's size")
	var own := 40 if host_double else 24
	assert_eq(bytes.decode_u32(at + 4), own, "tests.elf is built for this host's real_t")
	var other := bytes.duplicate()
	other.encode_u32(at + 4, 64 - own)
	var refused = Sandbox.FromBuffer(other)
	assert_false(refused.has_program_loaded(), "a guest whose Variant is the other size is refused")
	assert_engine_error("Variant is")
	refused.free()
	var loaded = Sandbox.FromBuffer(bytes)
	assert_true(loaded.has_program_loaded(), "the same guest, unchanged, loads")
	loaded.free()


# The .sandbox_variant section: "SBXV" and the guest's Variant size.
func _variant_size_at(bytes: PackedByteArray) -> int:
	var at := bytes.find(0x53)
	while at != -1 and at + 8 <= bytes.size():
		if bytes[at + 1] == 0x42 and bytes[at + 2] == 0x58 and bytes[at + 3] == 0x56:
			return at
		at = bytes.find(0x53, at + 1)
	return -1

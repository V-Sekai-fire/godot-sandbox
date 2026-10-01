# Differential runs for packed array regions and the loop rewrite: a reference
# GDScript, other GDScript texts of it (the rewritten ones) and compiled ELFs, all
# called with fresh arguments built the same way. The answer, the arguments
# afterwards (the caller's view of in-place writes) and the extra aliases are
# compared with var_to_bytes. build_matrix.sh makes the ELFs and texts; then
#   godot --headless --path . --script res://run_packed.gd -- cases=packed
#       ref=res://packed_cases.gd gd:<label>=res://<text>.gd <label>=<path>.elf ...
# A compiled call that raised returns null where GDScript returns the typed
# default: that is the sandbox's error convention (with or without these
# changes), so for such a call only the arguments are compared, and the row
# says so.
extends SceneTree

var _fails := 0


func _floats_from_bits(bits: Array) -> PackedFloat32Array:
	var raw := PackedByteArray()
	raw.resize(bits.size() * 4)
	for i in bits.size():
		raw.encode_u32(i * 4, bits[i])
	return raw.to_float32_array()


# name -> Callable returning [args, extras]; extras are aliases the caller keeps.
func _cases(which: String) -> Array:
	return _rewrite_cases() if which == "rewrite" else _packed_cases()


func _rewrite_cases() -> Array:
	var cases := []
	cases.append(["walk_sum", func():
		var v := PackedFloat32Array()
		for i in 200:
			v.append(sin(i * 0.05) * 3.0)
		return [[v], []]])
	cases.append(["walk_double", func():
		var v := PackedInt32Array()
		for i in 50:
			v.append(i * 7 - 100)
		return [[v], [{"alias": v}]]])
	cases.append(["walk_vectors", func():
		var p := PackedVector2Array()
		for i in 40:
			p.append(Vector2(i * 0.5, 20.0 - i))
		return [[p], []]])
	cases.append(["walk_untyped packed", func():
		var v := PackedFloat64Array()
		for i in 30:
			v.append(i * 1.25)
		return [[v], []]])
	cases.append(["walk_untyped array", func():
		return [[[1, 2.5, 3, 4.75]], []]])
	cases.append(["scan_while", func():
		var v := PackedInt64Array()
		for i in 64:
			v.append(i * i)
		return [[v, 1000], []]])
	cases.append(["scan_while none", func():
		return [[PackedInt64Array([1, 2, 3]), 99], []]])
	cases.append(["grow_while", func():
		return [[PackedInt32Array([3, 5, 2])], []]])
	cases.append(["walk_member", func():
		return [[PackedFloat64Array([1.0, 2.0, 4.0, 8.0])], []]])
	cases.append(["nested_walks", func():
		return [[PackedInt32Array([1, 2, 3, 4]), PackedInt32Array([5, -6, 7])], []]])
	return cases


func _packed_cases() -> Array:
	var cases := []
	cases.append(["scale", func():
		var src := PackedFloat32Array()
		for i in 1000:
			src.append(i * 0.37 - 5.0)
		return [[src, 1.25], []]])
	cases.append(["bump", func():
		var a := PackedInt32Array()
		for i in 50:
			a.append(i * i - 300)
		var alias := {"k": a}
		return [[a, 50], [alias]]])
	cases.append(["shift_into distinct", func():
		var a := PackedInt32Array()
		var b := PackedInt32Array()
		for i in 30:
			a.append(i)
			b.append(1000 + i)
		return [[a, b], []]])
	cases.append(["shift_into aliased", func():
		var a := PackedInt32Array()
		for i in 30:
			a.append(i)
		return [[a, a], []]])
	cases.append(["ends", func():
		var a := PackedInt64Array()
		for i in 20:
			a.append((i - 10) * 922337203685477)
		return [[a], []]])
	cases.append(["overrun in range", func():
		var a := PackedInt32Array()
		a.resize(10)
		return [[a, 10], []]])
	cases.append(["overrun past end", func():
		var a := PackedInt32Array()
		a.resize(10)
		var alias := [a]
		return [[a, 12], [alias]]])
	cases.append(["overread past end", func():
		var a := PackedFloat64Array()
		for i in 10:
			a.append(i * 0.5)
		return [[a, 11], []]])
	cases.append(["grow", func():
		return [[PackedInt32Array([3, 2, 0, 1])], []]])
	cases.append(["through_holder", func():
		var a := PackedInt32Array()
		a.resize(10)
		return [[a, [a]], []]])
	cases.append(["lift", func():
		var p := PackedVector2Array()
		for i in 30:
			p.append(Vector2(i * 0.25, -i * 1.5))
		var c := PackedColorArray()
		for i in 7:
			c.append(Color(i * 0.1, 1.0 - i * 0.1, 0.3, 0.9))
		return [[p, c], []]])
	cases.append(["bytes", func():
		var b := PackedByteArray()
		for i in 64:
			b.append((i * 37) % 256)
		return [[b], []]])
	cases.append(["ints_into_floats", func():
		var f32 := PackedFloat32Array()
		f32.resize(10)
		var f64 := PackedFloat64Array()
		f64.resize(10)
		return [[f32, f64], []]])
	cases.append(["copy_floats", func():
		return [[_floats_from_bits([0x7fa00001, 0xffc00123, 0x80000000, 0x7f800000, 0xff800000,
			0x00000001, 0x007fffff, 0x3f800000, 0x7f7fffff, 0x7fc00000])], []]])
	cases.append(["scale_doubles", func():
		var d := PackedFloat64Array([1e300, -1e-300, 3.4028235677973366e38, 1.0 / 3.0, -0.0, 2.5e-45])
		return [[d], []]])
	cases.append(["walk", func():
		var a := PackedFloat32Array()
		for i in 100:
			a.append(sin(i * 0.1))
		return [[a], []]])
	cases.append(["early", func():
		var a := PackedInt32Array()
		a.resize(10)
		return [[a, 4], []]])
	cases.append(["nested", func():
		var rows := PackedFloat32Array()
		for i in 64:
			rows.append(cos(i * 0.3))
		return [[rows, 8], []]])
	cases.append(["nested empty", func():
		return [[PackedFloat32Array(), 8], []]])
	cases.append(["with_call", func():
		var a := PackedFloat32Array()
		for i in 20:
			a.append(i * 0.5)
		return [[a], []]])
	cases.append(["with_value_calls", func():
		var a := PackedFloat32Array()
		for i in 20:
			a.append(i * 0.05)
		return [[a, PackedInt32Array()], []]])
	return cases


func _run(target, name: String, args: Array):
	if target is Sandbox:
		return target.callv("vmcall", [name] + args)
	return target.callv(name, args)


func _initialize() -> void:
	var which := "packed"
	var reference := "res://packed_cases.gd"
	var targets := []
	for spec in OS.get_cmdline_user_args():
		var parts: PackedStringArray = spec.split("=", true, 1)
		if parts[0] == "cases":
			which = parts[1]
		elif parts[0] == "ref":
			reference = parts[1]
		elif parts[0].begins_with("gd:"):
			var script = load(parts[1])
			if script == null or not script.can_instantiate():
				print("run_packed: %s does not load" % parts[1])
				quit(2)
				return
			targets.append([parts[0].substr(3), script.new()])
		else:
			var sb = ClassDB.instantiate("Sandbox")
			sb.memory_max = 64
			sb.references_max = 4096
			sb.execution_timeout = 1 << 24
			root.add_child(sb)
			sb.load_buffer(FileAccess.get_file_as_bytes(parts[1]))
			targets.append([parts[0], sb])
	var gd = load(reference).new()
	for case in _cases(which):
		var label: String = case[0]
		var name: String = label.split(" ")[0]
		var made: Array = case[1].call()
		var want_ret = _run(gd, name, made[0])
		var want := var_to_bytes([want_ret, made[0], made[1]])
		var want_state := var_to_bytes([made[0], made[1]])
		var line := "%-22s" % label
		for target in targets:
			var fresh: Array = case[1].call()
			var compiled: bool = target[1] is Sandbox
			var before: int = target[1].get_exceptions() if compiled else 0
			var got_ret = _run(target[1], name, fresh[0])
			var raised: bool = compiled and target[1].get_exceptions() > before
			var same: bool
			if raised:
				same = var_to_bytes([fresh[0], fresh[1]]) == want_state
			else:
				same = var_to_bytes([got_ret, fresh[0], fresh[1]]) == want
			if not same:
				_fails += 1
				print("  %s %s: want %s / %s" % [label, target[0], str(want_ret), str(made[0])])
				print("  %s %s: got  %s / %s" % [label, target[0], str(got_ret), str(fresh[0])])
			line += "  %s=%s%s" % [target[0], "same" if same else "DIFF", " (raised: arguments compared)" if raised else ""]
		print(line)
	print("run_packed: %s cases, %d difference(s)" % [which, _fails])
	for target in targets:
		if not (target[1] is Sandbox):
			target[1].free()
	gd.free()
	quit(1 if _fails > 0 else 0)

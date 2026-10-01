# One ELF of bench.gd in a Sandbox against bench.gd run as GDScript: min of --reps interleaved
# calls per kernel, and whether the answers are identical. --translate looks for a native
# translation in res://bintr, made for that ELF with these Sandbox settings, and fails without
# one. An addon build with Sandbox.get_host_call_counts() also gets the host calls of one call.
#   godot --headless --path . --script res://run_bench.gd -- --elf <path> [--translate] [--reps 15] [--n 200000]
extends SceneTree

const Bench = preload("res://bench.gd")


func _counts() -> Variant:
	if ClassDB.class_has_method("Sandbox", "get_host_call_counts"):
		return ClassDB.class_call_static("Sandbox", "get_host_call_counts")
	return null


func _initialize() -> void:
	var a := OS.get_cmdline_user_args()
	var elf := ""
	var translate := false
	var reps := 15
	var n := 200000
	var i := 0
	while i < a.size():
		if a[i] == "--elf":
			elf = a[i + 1]
			i += 1
		elif a[i] == "--translate":
			translate = true
		elif a[i] == "--reps":
			reps = int(a[i + 1])
			i += 1
		elif a[i] == "--n":
			n = int(a[i + 1])
			i += 1
		i += 1
	if translate:
		ProjectSettings.set_setting("sandbox/binary_translation/cache_dir", "res://bintr")
		ProjectSettings.set_setting("sandbox/binary_translation/enabled", true)
	var sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = 1024
	sb.references_max = 4096
	sb.execution_timeout = 1 << 24
	sb.allocations_max = 1000000
	sb.load_buffer(FileAccess.get_file_as_bytes(elf))
	print("bench: elf=%s translated=%s hash=%08X n=%d reps=%d" % [elf.get_file(), sb.is_binary_translated(), sb.get_translation_hash(), n, reps])
	if translate and not sb.is_binary_translated():
		print("bench: FAIL asked to translate and no library was used")
		quit(1)
		return
	var g = Bench.new()

	var vf := PackedFloat32Array()
	vf.resize(n)
	for k in n:
		vf[k] = float((k * 37) % 1000) * 0.125
	var v2 := PackedVector2Array()
	v2.resize(n / 2)
	for k in n / 2:
		v2[k] = Vector2(float(k % 100) * 0.5, float(k % 7) - 3.0)
	var vi := PackedInt32Array()
	vi.resize(n)
	for k in n:
		vi[k] = k
	var vg := vf.duplicate()
	var vs := vf.duplicate()

	var kernels := [
		["scalar (10 N, no array)", func(): return g.scalar(n * 10), func(): return sb.vmcall("scalar", n * 10)],
		["sum: for i in v.size()", func(): return g.sum(vf), func(): return sb.vmcall("sum", vf)],
		["fill: out[i] = float(i)", func(): return g.fill(n), func(): return sb.vmcall("fill", n)],
		["copy_vec2: floats -> Vector2s", func(): return g.copy_vec2(vf), func(): return sb.vmcall("copy_vec2", vf)],
		["walk_sum: for x in v", func(): return g.walk_sum(vf), func(): return sb.vmcall("walk_sum", vf)],
		["walk_vec2: for p in v (Vector2)", func(): return g.walk_vec2(v2), func(): return sb.vmcall("walk_vec2", v2)],
		["walk_untyped: for x in values", func(): return g.walk_untyped(vf), func(): return sb.vmcall("walk_untyped", vf)],
		["scan_while: while i < v.size()", func(): return g.scan_while(vi, n), func(): return sb.vmcall("scan_while", vi, n)],
		["scale_in_place: v[i] = v[i] * k", func(): g.scale_in_place(vg, 1.0), func(): return sb.vmcall("scale_in_place", vs, 1.0)],
	]

	var fails := 0
	var same := {}
	for k in kernels:
		var x = k[1].call()
		var y = k[2].call()
		same[k[0]] = var_to_bytes(x) == var_to_bytes(y)
	# The caller sees the guest's writes: both sides scale their own copy by 2, then compare.
	var wg := vf.duplicate()
	var ws := vf.duplicate()
	g.scale_in_place(wg, 2.0)
	sb.vmcall("scale_in_place", ws, 2.0)
	var written: bool = wg == ws and wg != vf
	same["scale_in_place: v[i] = v[i] * k"] = same["scale_in_place: v[i] = v[i] * k"] and written

	var best := {}
	for r in reps:
		for k in kernels:
			var t0 := Time.get_ticks_usec()
			k[1].call()
			var tg := Time.get_ticks_usec() - t0
			t0 = Time.get_ticks_usec()
			k[2].call()
			var ts := Time.get_ticks_usec() - t0
			var b: Array = best.get(k[0], [1 << 62, 1 << 62])
			best[k[0]] = [mini(b[0], tg), mini(b[1], ts)]

	for k in kernels:
		var b: Array = best[k[0]]
		if not same[k[0]]:
			fails += 1
		var line := "bench: %-34s gd %8d us  sgd %8d us  sgd/gd %6.2f  identical %s" % [k[0], b[0], b[1], float(b[1]) / maxf(float(b[0]), 1.0), "yes" if same[k[0]] else "NO"]
		var before = _counts()
		if before != null:
			k[2].call()
			var after = _counts()
			var total := 0
			var top := []
			for s in after.size():
				var d: int = after[s] - before[s]
				if d > 0:
					total += d
					top.append([d, s])
			top.sort()
			top.reverse()
			var parts := PackedStringArray()
			for t in top.slice(0, 4):
				parts.append("%d:%d" % [t[1], t[0]])
			line += "  host calls %d [%s]" % [total, ", ".join(parts)]
		print(line)
	print("bench: %s (%d kernels, %d differ)" % ["PASS" if fails == 0 else "FAIL", kernels.size(), fails])
	g.free()
	sb.free()
	quit(1 if fails else 0)

# bench.gd against its two rewritten texts, all three run as GDScript, on the same inputs.
#   godot --headless --path . --script res://check_rewritten.gd [-- res://<text>.gd ...]
extends SceneTree


func _answers(script: GDScript) -> Array:
	var g = script.new()
	var n := 20000
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
	var scaled := vf.duplicate()
	g.scale_in_place(scaled, 2.0)
	var out := [g.scalar(n), g.sum(vf), g.fill(n), g.copy_vec2(vf), g.walk_sum(vf), g.walk_vec2(v2),
			g.walk_untyped(vf), g.walk_untyped([1, 2.5, 3]), g.walk_untyped("abc".to_utf8_buffer()),
			g.scan_while(vi, n / 2), g.scan_while(vi, n), scaled]
	g.free()
	return out


func _initialize() -> void:
	var authored := _answers(load("res://bench.gd"))
	var fails := 0
	var paths: Array = Array(OS.get_cmdline_user_args())
	if paths.is_empty():
		paths = ["res://bench_rw_fa.gd", "res://bench_rw_nofa.gd"]
	for path in paths:
		var script = load(path)
		if script == null or not script.can_instantiate():
			print("check_rewritten: FAIL %s does not load" % path)
			fails += 1
			continue
		var answers := _answers(script)
		var differ := 0
		for i in authored.size():
			if var_to_bytes(authored[i]) != var_to_bytes(answers[i]):
				differ += 1
				print("check_rewritten: %s answer %d differs" % [path, i])
		print("check_rewritten: %s %d answers, %d differ" % [path, authored.size(), differ])
		fails += differ
	print("check_rewritten: %s" % ("PASS" if fails == 0 else "FAIL"))
	quit(1 if fails else 0)

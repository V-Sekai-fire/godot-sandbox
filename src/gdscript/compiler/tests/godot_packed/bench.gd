extends Node

func scalar(n: int) -> int:
	var s := 0
	for i in n:
		s += (i * i) % 7
	return s

func sum(v: PackedFloat32Array) -> float:
	var s := 0.0
	for i in v.size():
		s += v[i]
	return s

func fill(n: int) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(n)
	for i in n:
		out[i] = float(i)
	return out

func copy_vec2(v: PackedFloat32Array) -> PackedVector2Array:
	var out := PackedVector2Array()
	out.resize(v.size() / 2)
	for i in out.size():
		out[i] = Vector2(v[i * 2], v[i * 2 + 1])
	return out

func walk_sum(v: PackedFloat32Array) -> float:
	var s := 0.0
	for x in v:
		s += x
	return s

func walk_vec2(v: PackedVector2Array) -> Vector2:
	var acc := Vector2()
	for p in v:
		acc += p
	return acc

func walk_untyped(values) -> float:
	var s := 0.0
	for x in values:
		s += float(x)
	return s

func scan_while(v: PackedInt32Array, limit: int) -> int:
	var i := 0
	while i < v.size() and v[i] < limit:
		i += 1
	return i

func scale_in_place(v: PackedFloat32Array, k: float) -> void:
	for i in v.size():
		v[i] = v[i] * k

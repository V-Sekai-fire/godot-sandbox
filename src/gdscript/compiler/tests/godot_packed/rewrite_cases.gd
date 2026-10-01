# Differential cases for the GDScript rewrite of packed array loops. Each runs as
# authored GDScript, as the rewritten GDScript (Godot's interpreter, both), and
# compiled in every configuration; run_packed.gd compares them bit for bit.
extends Node

const SCALE := 0.5


# A typed walk that only reads: indexed (fast arrays) or an Array copy (without).
func walk_sum(v: PackedFloat32Array) -> float:
	var total := 0.0
	for x in v:
		total = total * SCALE + x
	return total


# A typed walk that writes the array it walks: indexed, never copied.
func walk_double(v: PackedInt32Array) -> int:
	var total := 0
	var i := 0
	for x in v:
		v[i] = x * 2
		total += v[i]
		i += 1
	return total


# A walk over vectors, building a value with a method of a local value.
func walk_vectors(points: PackedVector2Array) -> Vector2:
	var acc := Vector2.ZERO
	for p in points:
		acc = acc.lerp(p, 0.25) + Vector2(p.y, -p.x) * 0.125
	return acc


# Untyped: the walk is guarded by typeof, the authored loop is the fallback.
func walk_untyped(values) -> float:
	var total := 0.0
	for x in values:
		total += float(x)
	return total


# while over size(): read once before the loop.
func scan_while(v: PackedInt64Array, limit: int) -> int:
	var i := 0
	var found := -1
	while i < v.size() and found < 0:
		if v[i] > limit:
			found = i
		i += 1
	return found


# The body resizes the array: left as written.
func grow_while(v: PackedInt32Array) -> int:
	var i := 0
	while i < v.size():
		if v[i] > 1 and v.size() < 30:
			v.append(v[i] - 1)
		i += 1
	return v.size()


# A member read in the body (it could have a getter): left as written.
var bias := 1.5

func walk_member(v: PackedFloat64Array) -> float:
	var total := 0.0
	for x in v:
		total += x * bias
	return total


# Nested: an inner walk inside an outer index loop.
func nested_walks(rows: PackedInt32Array, cols: PackedInt32Array) -> int:
	var total := 0
	for r in rows:
		for c in cols:
			total += r * c
	return total

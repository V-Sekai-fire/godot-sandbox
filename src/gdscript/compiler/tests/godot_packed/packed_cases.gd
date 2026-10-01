# Differential cases for packed array regions: each runs as interpreted GDScript
# and as compiled .sgd (every config), and run_packed.gd compares the answers and
# the arguments afterwards, bit for bit (var_to_bytes).
extends Node


# The plain case: read one array, write another.
func scale(src: PackedFloat32Array, k: float) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(src.size())
	for i in src.size():
		out[i] = src[i] * k
	return out


# Writes into its parameter: the host caller (and any alias it holds) must see them.
func bump(a: PackedInt32Array, n: int) -> int:
	for i in n:
		a[i] = a[i] + i * 3
	return a.size()


# `a` and `b` may be the same array: the identity guard must take the fallback.
# Shared, each element depends on the one just written; two copies would not see it.
func shift_into(a: PackedInt32Array, b: PackedInt32Array) -> int:
	var total := 0
	for i in range(1, a.size()):
		a[i] = b[i - 1] * 2 + 1
		total += a[i]
	return total


# Negative indices count from the end, natively.
func ends(a: PackedInt64Array) -> int:
	var total := 0
	for i in range(1, a.size() + 1):
		total += a[-i] * i
		a[-i] = a[-i] - 1
	return total


# An index past the end: the host reports it, and the writes before it stay.
func overrun(a: PackedInt32Array, stop: int) -> int:
	for i in stop:
		a[i] = 100 + i
	return 1


func overread(a: PackedFloat64Array, stop: int) -> float:
	var total := 0.0
	for i in stop:
		total += a[i]
	return total


# The body resizes the array: not a region, the original loop runs.
func grow(a: PackedInt32Array) -> int:
	var i := 0
	while i < a.size():
		if a[i] > 0 and a.size() < 40:
			a.append(a[i] - 1)
		i += 1
	return a.size()


# Another view of the array through a container: the copy must not hide writes.
func through_holder(a: PackedInt32Array, holder: Array) -> int:
	var total := 0
	for i in a.size():
		a[i] = i * 7
		var view: PackedInt32Array = holder[0]
		total += view[i]
	return total


# Vector and Color elements, Vector3 built from Vector2 and Color.
func lift(p: PackedVector2Array, c: PackedColorArray) -> PackedVector3Array:
	var out := PackedVector3Array()
	out.resize(p.size())
	for i in p.size():
		var v := p[i]
		var col := c[i % c.size()]
		out[i] = Vector3(v.x * col.r, v.y + col.g, col.b - col.a)
		c[i % c.size()] = col.lerp(Color(1, 1, 1, 1), 0.25)
	return out


# Bytes: ints wrap to 8 bits when stored.
func bytes(b: PackedByteArray) -> int:
	var total := 0
	for i in b.size():
		total += b[i]
		b[i] = b[i] * 3 + 7
	return total


# Ints stored into float arrays round once (int64 -> float).
func ints_into_floats(f32: PackedFloat32Array, f64: PackedFloat64Array) -> float:
	var big := 16777217
	for i in f32.size():
		f32[i] = big + i * 3
		f64[i] = big * big + i
	return f32[0] + f64[0]


# Float32 round trips keep NaN payloads and signs as the engine does.
func copy_floats(src: PackedFloat32Array) -> PackedFloat32Array:
	var dst := PackedFloat32Array()
	dst.resize(src.size())
	for i in src.size():
		dst[i] = src[i]
	return dst


func scale_doubles(src: PackedFloat64Array) -> PackedFloat32Array:
	var dst := PackedFloat32Array()
	dst.resize(src.size())
	for i in src.size():
		dst[i] = src[i] * 1.5
	return dst


# The walk `for x in a`.
func walk(a: PackedFloat32Array) -> float:
	var total := 0.0
	for x in a:
		total = total * 0.5 + x
	return total


# A return inside the region stores the copy first.
func early(a: PackedInt32Array, at: int) -> int:
	for i in a.size():
		a[i] = -i
		if i == at:
			return i
	return -1


# A nested loop and size()/is_empty() on the copy.
func nested(rows: PackedFloat32Array, width: int) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	if rows.is_empty():
		return out
	out.resize(rows.size())
	var h := rows.size() / width
	for y in h:
		for x in width:
			var s := 0.0
			for k in 3:
				var xx := clampi(x + k - 1, 0, width - 1)
				s += rows[y * width + xx]
			out[y * width + x] = s / 3.0
	return out


# A guest call inside the loop: not a region (the callee could see the array).
func _twice(v: float) -> float:
	return v * 2.0


func with_call(a: PackedFloat32Array) -> float:
	var total := 0.0
	for i in a.size():
		a[i] = _twice(a[i])
		total += a[i]
	return total


# A method on a value (Color.lerp) and on another array type (append) do not
# reach a Float32 array: still a region.
func with_value_calls(a: PackedFloat32Array, out: PackedInt32Array) -> float:
	var total := 0.0
	var c := Color(0.5, 0.25, 0.125, 1.0)
	for i in a.size():
		c = c.lerp(Color(a[i], 0.0, 0.0, 1.0), 0.5)
		total += c.r
		out.append(i)
	return total

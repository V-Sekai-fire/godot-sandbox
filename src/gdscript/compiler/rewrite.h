#pragma once
#include "ast.h"
#include <string>
#include <vector>

namespace gdscript {

// GDScript to GDScript: loops over packed arrays rewritten into equivalent
// GDScript that costs fewer host calls once compiled (and is still GDScript:
// Godot runs the rewritten text exactly as it runs the authored one). The
// rewritten source is a build artifact -- gdscript_to_riscv --emit-rewritten
// writes it out for inspection; it is never meant to replace the authored file.
//
// Each rule is applied only where the authored AST proves it safe. Where the
// proof needs a fact only the run knows (the type of an untyped array), the
// rewritten loop is guarded by a test of that fact and the authored loop is
// kept, unchanged, as the fallback:
//
//   walk     `for x in v` over a packed array the body cannot resize:
//              for _sgd_i0 in v.size():
//                  var x: T = v[_sgd_i0]
//            One host call an element (the element) instead of two (size and
//            element). A walk that also never writes v becomes
//            `for _sgd_e0 in Array(v)`: Array walks are fetched sixteen elements
//            a host call. An untyped `v` gets the index walk under
//            `if typeof(v) >= TYPE_PACKED_BYTE_ARRAY and typeof(v) <= TYPE_PACKED_VECTOR4_ARRAY:`
//            with the authored loop in its `else:`.
//   size     `while ... v.size() ...` over a typed packed array the body cannot
//            resize: `var _sgd_n0 := v.size()` before the loop, and the test
//            reads it. One host call a pass fewer.
//
// With fast arrays only the untyped walk is rewritten: a region walks a typed
// array and reads its size from the copy, which beats both other rules.
//
// "Cannot resize": in the loop, `v` appears only as `v[i]`, `v[i] = x`,
// `v.size()` and `v.is_empty()`, and nothing in it can run code that could
// reach `v` -- no call but to math builtins and value constructors, no method
// but on locals of value types or on packed arrays of another element type, no
// member variable (a property may have a setter or getter), no await, no lambda.
struct RewriteOptions {
	// The code generator's packed array regions run, so typed arrays are left to them.
	bool fast_arrays = true;
};

struct RewriteResult {
	// The rewritten text; the authored text when nothing applied.
	std::string source;
	// line_map[n] is the authored line that rewritten line n (1-based) came from;
	// an inserted line maps to the loop it belongs to. Empty when nothing applied.
	std::vector<int> line_map;
	// One line per rewrite, "line N: rule: what".
	std::vector<std::string> notes;
	int applied = 0;
};

RewriteResult rewrite_packed_loops(const std::string& source, const Program& program,
	const RewriteOptions& options = {});

} // namespace gdscript

#include "rewrite.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace gdscript {
namespace {

struct Line {
	std::string text; // without its ending
	std::string ending; // "\n", "\r\n" or "" for the last line
};

std::vector<Line> split_lines(const std::string& source) {
	std::vector<Line> lines;
	size_t start = 0;
	while (start <= source.size()) {
		const size_t newline = source.find('\n', start);
		if (newline == std::string::npos) {
			if (start < source.size()) {
				lines.push_back({ source.substr(start), "" });
			}
			break;
		}
		size_t end = newline;
		std::string ending = "\n";
		if (end > start && source[end - 1] == '\r') {
			end--;
			ending = "\r\n";
		}
		lines.push_back({ source.substr(start, end - start), ending });
		start = newline + 1;
	}
	return lines;
}

std::string leading_whitespace(const std::string& text) {
	size_t end = 0;
	while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) {
		end++;
	}
	return text.substr(0, end);
}

// Blank, or only a comment: belongs to no statement.
bool is_filler(const std::string& text) {
	const size_t first = text.find_first_not_of(" \t");
	return first == std::string::npos || text[first] == '#';
}

const std::unordered_map<std::string, std::string>& packed_elements() {
	static const std::unordered_map<std::string, std::string> elements = {
		{ "PackedByteArray", "int" }, { "PackedInt32Array", "int" }, { "PackedInt64Array", "int" },
		{ "PackedFloat32Array", "float" }, { "PackedFloat64Array", "float" },
		{ "PackedStringArray", "String" }, { "PackedVector2Array", "Vector2" },
		{ "PackedVector3Array", "Vector3" }, { "PackedVector4Array", "Vector4" },
		{ "PackedColorArray", "Color" },
	};
	return elements;
}

bool is_packed(const std::string& type) {
	return packed_elements().count(type) != 0;
}

// Values whose methods run engine code only: no script, no reference to an array.
bool is_value_type(const std::string& type) {
	static const std::unordered_set<std::string> values = {
		"bool", "int", "float", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i",
		"Color", "Rect2", "Rect2i", "Plane", "Quaternion", "Basis", "Transform2D", "Transform3D",
		"AABB", "Projection",
	};
	return values.count(type) != 0;
}

// Globals that run no script code and change no array.
bool is_pure_builtin(const std::string& name) {
	static const std::unordered_set<std::string> pure = {
		"abs", "absf", "absi", "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh",
		"bezier_derivative", "bezier_interpolate", "ceil", "ceilf", "ceili", "clamp", "clampf",
		"clampi", "cos", "cosh", "cubic_interpolate", "deg_to_rad", "ease", "exp", "floor",
		"floorf", "floori", "fmod", "fposmod", "inverse_lerp", "is_equal_approx", "is_finite",
		"is_inf", "is_nan", "is_zero_approx", "lerp", "lerp_angle", "lerpf", "linear_to_db",
		"db_to_linear", "log", "max", "maxf", "maxi", "min", "minf", "mini", "move_toward",
		"nearest_po2", "pingpong", "posmod", "pow", "rad_to_deg", "remap", "rotate_toward",
		"round", "roundf", "roundi", "sign", "signf", "signi", "sin", "sinh", "smoothstep",
		"snapped", "snappedf", "snappedi", "sqrt", "step_decimals", "tan", "tanh", "wrap",
		"wrapf", "wrapi", "int", "float", "bool", "range", "typeof",
	};
	return pure.count(name) != 0 || is_value_type(name);
}

bool is_global_constant(const std::string& name) {
	return name == "PI" || name == "TAU" || name == "INF" || name == "NAN" ||
		name.rfind("TYPE_", 0) == 0;
}

// Methods of a Packed*Array that leave its size alone.
bool keeps_size(const std::string& method) {
	static const std::unordered_set<std::string> keepers = {
		"size", "is_empty", "get", "set", "has", "find", "rfind", "count", "slice", "duplicate",
		"fill", "sort", "reverse", "bsearch", "hex_encode", "to_byte_array", "encode_u8",
		"encode_s8", "encode_u16", "encode_s16", "encode_u32", "encode_s32", "encode_u64",
		"encode_s64", "encode_half", "encode_float", "encode_double", "decode_u8", "decode_s8",
		"decode_u16", "decode_s16", "decode_u32", "decode_s32", "decode_u64", "decode_s64",
		"decode_half", "decode_float", "decode_double", "get_string_from_ascii",
		"get_string_from_utf8", "get_string_from_utf16", "get_string_from_utf32",
		"get_string_from_wchar", "to_int32_array", "to_int64_array", "to_float32_array",
		"to_float64_array",
	};
	return keepers.count(method) != 0;
}

// What a function declares: every local's type name ("" when not known, or when
// the name is declared twice and so means different things in different places).
struct Locals {
	std::unordered_map<std::string, std::string> types;

	void declare(const std::string& name, const std::string& type) {
		const std::pair<std::unordered_map<std::string, std::string>::iterator, bool> found =
			types.try_emplace(name, type);
		if (!found.second && found.first->second != type) {
			found.first->second.clear();
		}
	}
	bool has(const std::string& name) const { return types.count(name) != 0; }
	std::string type_of(const std::string& name) const {
		const std::unordered_map<std::string, std::string>::const_iterator it = types.find(name);
		return it != types.end() ? it->second : std::string();
	}
};

// "Variant" for a declaration GDScript itself leaves untyped (`var x = v`, a
// parameter without a hint): any value, decided at run time. "" for a type that
// GDScript fixes but this pass does not work out (`var n := a.size() / 3`):
// rewriting such a variable as if it could be anything would not even parse,
// since the analyzer knows what it is.
std::string declared_type(const VarDeclStmt& decl) {
	if (!decl.type_hint.single_name().empty()) {
		return decl.type_hint.arguments.empty() ? decl.type_hint.single_name() : std::string();
	}
	if (!decl.type_hint.empty()) {
		return std::string();
	}
	if (!decl.inferred) {
		return "Variant";
	}
	if (!decl.initializer) {
		return std::string();
	}
	if (const CallExpr* call = dynamic_cast<const CallExpr*>(decl.initializer.get())) {
		if (is_packed(call->function_name) || is_value_type(call->function_name)) {
			return call->function_name;
		}
	}
	// Vector2.ZERO, Color.WHITE: a constant of the value type it is read from.
	if (const MemberCallExpr* member = dynamic_cast<const MemberCallExpr*>(decl.initializer.get())) {
		const VariableExpr* owner = dynamic_cast<const VariableExpr*>(member->object.get());
		if (owner != nullptr && !member->is_method_call && is_value_type(owner->name) &&
			!owner->name.empty() && std::isupper(static_cast<unsigned char>(member->member_name[0]))) {
			return owner->name;
		}
	}
	if (const LiteralExpr* literal = dynamic_cast<const LiteralExpr*>(decl.initializer.get())) {
		switch (literal->lit_type) {
			case LiteralExpr::Type::INTEGER: return "int";
			case LiteralExpr::Type::FLOAT: return "float";
			case LiteralExpr::Type::BOOL: return "bool";
			default: return std::string();
		}
	}
	return std::string();
}

void collect_locals(const std::vector<StmtPtr>& body, Locals& locals);

void collect_pattern(const MatchPattern* pattern, Locals& locals) {
	if (pattern == nullptr) {
		return;
	}
	if (pattern->kind == MatchPattern::Kind::BIND) {
		locals.declare(pattern->name, std::string());
	}
	for (const MatchPatternPtr& element : pattern->elements) {
		collect_pattern(element.get(), locals);
	}
	for (const MatchPattern::Entry& entry : pattern->entries) {
		collect_pattern(entry.value.get(), locals);
	}
	for (const MatchPattern::StructEntry& entry : pattern->struct_entries) {
		collect_pattern(entry.value.get(), locals);
	}
}

void collect_locals(const std::vector<StmtPtr>& body, Locals& locals) {
	for (const StmtPtr& stmt : body) {
		if (const VarDeclStmt* decl = dynamic_cast<const VarDeclStmt*>(stmt.get())) {
			locals.declare(decl->name, declared_type(*decl));
		} else if (const IfStmt* branch = dynamic_cast<const IfStmt*>(stmt.get())) {
			if (branch->binding) {
				locals.declare(branch->binding->name, std::string());
			}
			collect_locals(branch->then_branch, locals);
			collect_locals(branch->else_branch, locals);
		} else if (const WhileStmt* loop = dynamic_cast<const WhileStmt*>(stmt.get())) {
			collect_locals(loop->body, locals);
		} else if (const ForStmt* loop = dynamic_cast<const ForStmt*>(stmt.get())) {
			// The element of a typed packed array, the counter of a numeric loop.
			std::string type;
			if (const VariableExpr* walked = dynamic_cast<const VariableExpr*>(loop->iterable.get())) {
				const std::string container = locals.type_of(walked->name);
				const std::unordered_map<std::string, std::string>::const_iterator element =
					packed_elements().find(container);
				type = element != packed_elements().end() ? element->second
					: container == "int" ? "int" : std::string();
			} else if (const LiteralExpr* literal = dynamic_cast<const LiteralExpr*>(loop->iterable.get());
				literal != nullptr && literal->lit_type == LiteralExpr::Type::INTEGER) {
				type = "int";
			} else if (const CallExpr* call = dynamic_cast<const CallExpr*>(loop->iterable.get());
				call != nullptr && call->function_name == "range") {
				type = "int";
			}
			locals.declare(loop->variable, type);
			collect_locals(loop->body, locals);
		} else if (const MatchStmt* match = dynamic_cast<const MatchStmt*>(stmt.get())) {
			for (const MatchStmt::Branch& arm : match->branches) {
				for (const MatchPatternPtr& pattern : arm.patterns) {
					collect_pattern(pattern.get(), locals);
				}
				collect_locals(arm.body, locals);
			}
		}
	}
}

// Names a loop body may read that are not locals: constants and enums.
struct ScriptNames {
	std::unordered_set<std::string> constants;
	std::unordered_set<std::string> enums;
};

// Does the loop leave `array` its size, and run no code that could reach it?
// See rewrite.h. `writes` reports a store into one of its elements.
class SizeCheck {
public:
	SizeCheck(const std::string& array, const std::string& array_type, const Locals& locals,
		const ScriptNames& names) :
			m_array(array), m_array_type(array_type), m_locals(locals), m_names(names) {}

	bool ok = true;
	bool writes = false;

	void body(const std::vector<StmtPtr>& statements) {
		for (const StmtPtr& stmt : statements) {
			statement(stmt.get());
		}
	}

	void expression(const Expr* expr) {
		if (expr == nullptr || !ok) {
			return;
		}
		if (dynamic_cast<const LiteralExpr*>(expr) != nullptr) {
			return;
		}
		if (const VariableExpr* variable = dynamic_cast<const VariableExpr*>(expr)) {
			if (variable->name == m_array) {
				fail(); // the array as a value: it may go anywhere
			} else if (!m_locals.has(variable->name) && !m_names.constants.count(variable->name) &&
				!is_global_constant(variable->name)) {
				fail(); // a member variable may have a getter
			}
			return;
		}
		if (const IndexExpr* index = dynamic_cast<const IndexExpr*>(expr)) {
			indexed(index->object.get());
			expression(index->index.get());
			return;
		}
		if (const MemberCallExpr* member = dynamic_cast<const MemberCallExpr*>(expr)) {
			member_access(member);
			return;
		}
		if (const CallExpr* call = dynamic_cast<const CallExpr*>(expr)) {
			if (!is_pure_builtin(call->function_name) || m_locals.has(call->function_name)) {
				fail();
				return;
			}
			for (const ExprPtr& argument : call->arguments) {
				expression(argument.get());
			}
			return;
		}
		if (const BinaryExpr* binary = dynamic_cast<const BinaryExpr*>(expr)) {
			// `x in object` may ask a script; the rest are engine operators.
			if (binary->op == BinaryExpr::Op::IN && static_type(binary->right.get()).empty()) {
				fail();
				return;
			}
			expression(binary->left.get());
			expression(binary->right.get());
			return;
		}
		if (const UnaryExpr* unary = dynamic_cast<const UnaryExpr*>(expr)) {
			expression(unary->operand.get());
			return;
		}
		if (const TernaryExpr* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
			expression(ternary->condition.get());
			expression(ternary->true_value.get());
			expression(ternary->false_value.get());
			return;
		}
		if (const CastExpr* cast = dynamic_cast<const CastExpr*>(expr)) {
			expression(cast->value.get());
			return;
		}
		if (const TypeTestExpr* test = dynamic_cast<const TypeTestExpr*>(expr)) {
			expression(test->value.get());
			return;
		}
		if (const ArrayLiteralExpr* array = dynamic_cast<const ArrayLiteralExpr*>(expr)) {
			for (const ExprPtr& element : array->elements) {
				expression(element.get());
			}
			return;
		}
		if (const DictionaryLiteralExpr* dictionary = dynamic_cast<const DictionaryLiteralExpr*>(expr)) {
			for (const std::pair<ExprPtr, ExprPtr>& entry : dictionary->elements) {
				expression(entry.first.get());
				expression(entry.second.get());
			}
			return;
		}
		fail(); // await, lambda, anything new
	}

	void statement(const Stmt* stmt) {
		if (stmt == nullptr || !ok) {
			return;
		}
		if (const ExprStmt* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
			expression(expr_stmt->expression.get());
		} else if (const VarDeclStmt* decl = dynamic_cast<const VarDeclStmt*>(stmt)) {
			if (decl->name == m_array || decl->has_accessors()) {
				fail();
				return;
			}
			expression(decl->initializer.get());
		} else if (const AssignStmt* assign = dynamic_cast<const AssignStmt*>(stmt)) {
			if (!assign->name.empty()) {
				if (assign->name == m_array || !m_locals.has(assign->name)) {
					fail(); // replacing the array, or a member's setter
					return;
				}
			} else if (const IndexExpr* index = dynamic_cast<const IndexExpr*>(assign->target.get())) {
				if (const VariableExpr* object = dynamic_cast<const VariableExpr*>(index->object.get());
					object != nullptr && object->name == m_array) {
					writes = true;
				} else {
					indexed(index->object.get());
				}
				expression(index->index.get());
			} else if (const MemberCallExpr* member = dynamic_cast<const MemberCallExpr*>(assign->target.get())) {
				// A component of a local value (`c.r = x`) changes that copy only.
				const VariableExpr* object = dynamic_cast<const VariableExpr*>(member->object.get());
				if (member->is_method_call || object == nullptr ||
					!is_value_type(m_locals.type_of(object->name))) {
					fail();
					return;
				}
			} else {
				fail();
				return;
			}
			expression(assign->value.get());
		} else if (const ReturnStmt* returned = dynamic_cast<const ReturnStmt*>(stmt)) {
			expression(returned->value.get());
		} else if (const IfStmt* branch = dynamic_cast<const IfStmt*>(stmt)) {
			expression(branch->condition.get());
			if (branch->binding) {
				if (branch->binding->name == m_array) {
					fail();
					return;
				}
				expression(branch->binding->initializer.get());
			}
			body(branch->then_branch);
			body(branch->else_branch);
		} else if (const WhileStmt* loop = dynamic_cast<const WhileStmt*>(stmt)) {
			expression(loop->condition.get());
			body(loop->body);
		} else if (const ForStmt* loop = dynamic_cast<const ForStmt*>(stmt)) {
			if (loop->variable == m_array) {
				fail();
				return;
			}
			if (const VariableExpr* walked = dynamic_cast<const VariableExpr*>(loop->iterable.get());
				walked != nullptr && walked->name == m_array) {
				// Walking the same array again only reads it.
			} else {
				expression(loop->iterable.get());
			}
			body(loop->body);
		} else if (const MatchStmt* match = dynamic_cast<const MatchStmt*>(stmt)) {
			expression(match->subject.get());
			for (const MatchStmt::Branch& arm : match->branches) {
				for (const MatchPatternPtr& pattern : arm.patterns) {
					match_pattern(pattern.get());
				}
				expression(arm.guard.get());
				body(arm.body);
			}
		} else if (dynamic_cast<const BreakStmt*>(stmt) == nullptr &&
			dynamic_cast<const ContinueStmt*>(stmt) == nullptr &&
			dynamic_cast<const PassStmt*>(stmt) == nullptr) {
			fail();
		}
	}

private:
	const std::string& m_array;
	const std::string& m_array_type;
	const Locals& m_locals;
	const ScriptNames& m_names;

	void fail() { ok = false; }

	void match_pattern(const MatchPattern* pattern) {
		if (pattern == nullptr) {
			return;
		}
		if (pattern->kind == MatchPattern::Kind::BIND && pattern->name == m_array) {
			fail();
			return;
		}
		expression(pattern->value.get());
		for (const MatchPatternPtr& element : pattern->elements) {
			match_pattern(element.get());
		}
		for (const MatchPattern::Entry& entry : pattern->entries) {
			expression(entry.key.get());
			match_pattern(entry.value.get());
		}
		for (const MatchPattern::StructEntry& entry : pattern->struct_entries) {
			match_pattern(entry.value.get());
		}
	}

	// The static type of a few simple expressions, "" otherwise.
	std::string static_type(const Expr* expr) const {
		if (const VariableExpr* variable = dynamic_cast<const VariableExpr*>(expr)) {
			if (variable->name == m_array) {
				return m_array_type;
			}
			return m_locals.type_of(variable->name);
		}
		if (const IndexExpr* index = dynamic_cast<const IndexExpr*>(expr)) {
			const std::string container = static_type(index->object.get());
			const std::unordered_map<std::string, std::string>::const_iterator it =
				packed_elements().find(container);
			return it != packed_elements().end() ? it->second : std::string();
		}
		if (const CallExpr* call = dynamic_cast<const CallExpr*>(expr)) {
			if (is_value_type(call->function_name) || is_packed(call->function_name)) {
				return call->function_name;
			}
		}
		return std::string();
	}

	// `object[...]`: the array itself, or a container that cannot be an Object
	// (whose `[]` would be a property access a script could answer).
	void indexed(const Expr* object) {
		if (const VariableExpr* variable = dynamic_cast<const VariableExpr*>(object);
			variable != nullptr && variable->name == m_array) {
			return;
		}
		const std::string type = static_type(object);
		const bool container = is_packed(type) || is_value_type(type) || type == "Array" ||
			type == "Dictionary" || type == "String";
		if (!container) {
			fail();
			return;
		}
		if (const IndexExpr* inner = dynamic_cast<const IndexExpr*>(object)) {
			indexed(inner->object.get());
			expression(inner->index.get());
		}
	}

	void member_access(const MemberCallExpr* member) {
		if (member->safe) {
			fail();
			return;
		}
		const VariableExpr* object = dynamic_cast<const VariableExpr*>(member->object.get());
		if (object != nullptr && object->name == m_array) {
			if (!member->is_method_call || !member->arguments.empty() ||
				(member->member_name != "size" && member->member_name != "is_empty")) {
				fail();
			}
			return;
		}
		// Vector3.UP, Color.from_hsv(...), an enum's member.
		if (object != nullptr && !m_locals.has(object->name) &&
			(is_value_type(object->name) || m_names.enums.count(object->name))) {
			for (const ExprPtr& argument : member->arguments) {
				expression(argument.get());
			}
			return;
		}
		const std::string type = static_type(member->object.get());
		bool allowed = is_value_type(type);
		if (!allowed && is_packed(type) && member->is_method_call) {
			// Another array: its size may change, not this one's -- unless it could
			// be this one, which an untyped walk or the same type cannot rule out.
			allowed = keeps_size(member->member_name) ||
				(!m_array_type.empty() && type != m_array_type);
		}
		if (!allowed) {
			fail();
			return;
		}
		expression(member->object.get());
		for (const ExprPtr& argument : member->arguments) {
			expression(argument.get());
		}
	}
};

struct Edit {
	int first; // authored lines first..last are replaced
	int last;
	std::vector<std::pair<std::string, int>> lines; // text, authored line it maps to
	std::string note;
};

class Rewriter {
public:
	Rewriter(const std::string& source, const Program& program, const RewriteOptions& options) :
			m_source(source), m_lines(split_lines(source)), m_options(options) {
		for (const VarDeclStmt& global : program.globals) {
			if (global.is_const) {
				m_names.constants.insert(global.name);
			}
		}
		for (const EnumDecl& decl : program.enums) {
			m_names.enums.insert(decl.name);
			for (const EnumDecl::Member& member : decl.members) {
				m_names.constants.insert(member.name);
			}
		}
		for (const StructDecl& decl : program.structs) {
			for (const StructField& constant : decl.constants) {
				m_names.constants.insert(constant.name);
			}
		}
	}

	void function(const FunctionDecl& decl) {
		Locals locals;
		for (const Parameter& parameter : decl.parameters) {
			locals.declare(parameter.name, parameter.type_hint.empty() ? std::string("Variant")
				: parameter.type_hint.arguments.empty() ? parameter.type_hint.single_name() : std::string());
		}
		collect_locals(decl.body, locals);
		statements(decl.body, locals);
	}

	RewriteResult finish() {
		RewriteResult result;
		if (m_edits.empty()) {
			result.source = m_source;
			return result;
		}
		std::sort(m_edits.begin(), m_edits.end(),
			[](const Edit& a, const Edit& b) { return a.first < b.first; });
		result.line_map.push_back(0);
		int next = 1;
		const std::function<void(int)> keep = [&](int line) {
			const Line& kept = m_lines[size_t(line - 1)];
			result.source += kept.text + kept.ending;
			result.line_map.push_back(line);
		};
		for (const Edit& edit : m_edits) {
			while (next < edit.first) {
				keep(next++);
			}
			const std::string& ending = m_lines[size_t(edit.first - 1)].ending.empty()
				? std::string("\n") : m_lines[size_t(edit.first - 1)].ending;
			for (const std::pair<std::string, int>& line : edit.lines) {
				result.source += line.first + ending;
				result.line_map.push_back(line.second);
			}
			next = edit.last + 1;
			result.notes.push_back(edit.note);
		}
		while (next <= int(m_lines.size())) {
			keep(next++);
		}
		result.applied = int(m_edits.size());
		return result;
	}

private:
	const std::string& m_source;
	std::vector<Line> m_lines;
	RewriteOptions m_options;
	ScriptNames m_names;
	std::vector<Edit> m_edits;
	std::unordered_set<int> m_touched; // authored lines an edit replaces
	int m_next_name = 0;

	std::string fresh(const std::string& stem) {
		for (;;) {
			const std::string name = "_sgd_" + stem + std::to_string(m_next_name++);
			if (m_source.find(name) == std::string::npos) {
				return name;
			}
		}
	}

	const std::string& text(int line) const { return m_lines[size_t(line - 1)].text; }

	bool claim(int first, int last) {
		for (int line = first; line <= last; line++) {
			if (m_touched.count(line)) {
				return false;
			}
		}
		for (int line = first; line <= last; line++) {
			m_touched.insert(line);
		}
		return true;
	}

	// The authored lines of a block statement's body: everything after the header
	// indented deeper than it, without trailing blank or comment lines.
	int body_end(int header) const {
		const size_t indent = leading_whitespace(text(header)).size();
		int last = header;
		for (int line = header + 1; line <= int(m_lines.size()); line++) {
			if (is_filler(text(line))) {
				continue;
			}
			if (leading_whitespace(text(line)).size() <= indent) {
				break;
			}
			last = line;
		}
		return last;
	}

	// Indentation of the first statement line after `header`.
	std::string body_indent(int header) const {
		for (int line = header + 1; line <= int(m_lines.size()); line++) {
			if (!is_filler(text(line))) {
				return leading_whitespace(text(line));
			}
		}
		return std::string();
	}

	void statements(const std::vector<StmtPtr>& body, const Locals& locals) {
		for (const StmtPtr& stmt : body) {
			if (const ForStmt* loop = dynamic_cast<const ForStmt*>(stmt.get())) {
				walk(loop, locals);
				statements(loop->body, locals);
			} else if (const WhileStmt* loop = dynamic_cast<const WhileStmt*>(stmt.get())) {
				hoist_size(loop, locals);
				statements(loop->body, locals);
			} else if (const IfStmt* branch = dynamic_cast<const IfStmt*>(stmt.get())) {
				statements(branch->then_branch, locals);
				statements(branch->else_branch, locals);
			} else if (const MatchStmt* match = dynamic_cast<const MatchStmt*>(stmt.get())) {
				for (const MatchStmt::Branch& arm : match->branches) {
					statements(arm.body, locals);
				}
			}
		}
	}

	// `for x in v:` over a packed array.
	void walk(const ForStmt* loop, const Locals& locals) {
		const VariableExpr* walked = dynamic_cast<const VariableExpr*>(loop->iterable.get());
		if (walked == nullptr || !locals.has(walked->name) || loop->line <= 0 ||
			loop->line > int(m_lines.size())) {
			return;
		}
		const std::string type = locals.type_of(walked->name);
		const bool typed = is_packed(type);
		if (!typed && type != "Variant") {
			return; // an Array (already fetched in batches), a String, a number, or not known
		}
		if (typed && m_options.fast_arrays) {
			return; // the region walks the copy
		}
		static const std::regex header(
			R"(^([ \t]*)for[ \t]+([A-Za-z_][A-Za-z_0-9]*)([ \t]*:[ \t]*[A-Za-z_][A-Za-z_0-9]*)?[ \t]+in[ \t]+([A-Za-z_][A-Za-z_0-9]*)[ \t]*:[ \t]*(#.*)?$)");
		std::smatch parts;
		const std::string& authored = text(loop->line);
		if (!std::regex_match(authored, parts, header) || parts[2].str() != loop->variable ||
			parts[4].str() != walked->name) {
			return; // an inline body, a header over several lines
		}
		SizeCheck check(walked->name, type, locals, m_names);
		check.body(loop->body);
		if (!check.ok) {
			return;
		}
		const int last = body_end(loop->line);
		const std::string indent = parts[1].str();
		const std::string inner = body_indent(loop->line);
		if (last == loop->line || inner.size() <= indent.size() || !claim(loop->line, typed ? loop->line : last)) {
			return;
		}
		const std::string array = walked->name;
		const std::string comment = parts[5].matched ? " " + parts[5].str() : std::string();
		Edit edit;
		edit.first = loop->line;
		edit.last = loop->line;
		if (typed) {
			const std::string element = packed_elements().at(type);
			if (!check.writes) {
				// Fetched sixteen elements a host call; nothing writes the array, so
				// the snapshot cannot go stale.
				const std::string each = fresh("e");
				edit.lines.push_back({ indent + "for " + each + " in Array(" + array + "):" + comment, loop->line });
				edit.lines.push_back({ inner + "var " + loop->variable + ": " + element + " = " + each, loop->line });
				edit.note = "line " + std::to_string(loop->line) + ": walk: `for " + loop->variable + " in " +
					array + "` reads an Array copy of the " + type;
			} else {
				const std::string index = fresh("i");
				edit.lines.push_back({ indent + "for " + index + " in " + array + ".size():" + comment, loop->line });
				edit.lines.push_back({ inner + "var " + loop->variable + ": " + element + " = " + array + "[" + index + "]", loop->line });
				edit.note = "line " + std::to_string(loop->line) + ": walk: `for " + loop->variable + " in " +
					array + "` indexes the " + type;
			}
			m_edits.push_back(std::move(edit));
			return;
		}
		// Untyped: the walk under a type test, the authored loop as the fallback.
		if (inner.compare(0, indent.size(), indent) != 0) {
			return;
		}
		const std::string step = inner.substr(indent.size());
		for (int line = loop->line + 1; line <= last; line++) {
			if (text(line).find("\"\"\"") != std::string::npos || text(line).find("'''") != std::string::npos) {
				return; // re-indenting would change a multi-line string
			}
		}
		const std::string index = fresh("i");
		edit.last = last;
		edit.lines.push_back({ indent + "if typeof(" + array + ") >= TYPE_PACKED_BYTE_ARRAY and typeof(" + array +
			") <= TYPE_PACKED_VECTOR4_ARRAY:" + comment, loop->line });
		edit.lines.push_back({ inner + "for " + index + " in " + array + ".size():", loop->line });
		edit.lines.push_back({ inner + step + "var " + loop->variable + " = " + array + "[" + index + "]", loop->line });
		for (int line = loop->line + 1; line <= last; line++) {
			edit.lines.push_back({ is_filler(text(line)) ? text(line) : step + text(line), line });
		}
		edit.lines.push_back({ indent + "else:", loop->line });
		edit.lines.push_back({ step + authored, loop->line });
		for (int line = loop->line + 1; line <= last; line++) {
			edit.lines.push_back({ is_filler(text(line)) ? text(line) : step + text(line), line });
		}
		edit.note = "line " + std::to_string(loop->line) + ": walk: `for " + loop->variable + " in " + array +
			"` indexes it when it is a packed array (typeof guard; the authored loop otherwise)";
		m_edits.push_back(std::move(edit));
	}

	// `while ... v.size() ...` over a typed packed array.
	void hoist_size(const WhileStmt* loop, const Locals& locals) {
		if (m_options.fast_arrays || loop->line <= 0 || loop->line > int(m_lines.size())) {
			return; // with fast arrays the region reads the size from the copy
		}
		std::vector<std::string> arrays;
		std::function<void(const Expr*)> find = [&](const Expr* expr) {
			if (const MemberCallExpr* member = dynamic_cast<const MemberCallExpr*>(expr)) {
				const VariableExpr* object = dynamic_cast<const VariableExpr*>(member->object.get());
				if (object != nullptr && member->is_method_call && member->arguments.empty() &&
					member->member_name == "size" && is_packed(locals.type_of(object->name)) &&
					std::find(arrays.begin(), arrays.end(), object->name) == arrays.end()) {
					arrays.push_back(object->name);
				}
				find(member->object.get());
				for (const ExprPtr& argument : member->arguments) find(argument.get());
			} else if (const BinaryExpr* binary = dynamic_cast<const BinaryExpr*>(expr)) {
				find(binary->left.get());
				find(binary->right.get());
			} else if (const UnaryExpr* unary = dynamic_cast<const UnaryExpr*>(expr)) {
				find(unary->operand.get());
			} else if (const CallExpr* call = dynamic_cast<const CallExpr*>(expr)) {
				for (const ExprPtr& argument : call->arguments) find(argument.get());
			}
		};
		find(loop->condition.get());
		if (arrays.empty()) {
			return;
		}
		static const std::regex header(R"(^([ \t]*)while[ \t]+([^"'#]*):[ \t]*(#.*)?$)");
		std::smatch parts;
		const std::string& authored = text(loop->line);
		if (!std::regex_match(authored, parts, header) || body_end(loop->line) == loop->line) {
			return;
		}
		std::string condition = parts[2].str();
		std::vector<std::pair<std::string, std::string>> hoisted;
		for (const std::string& array : arrays) {
			SizeCheck check(array, locals.type_of(array), locals, m_names);
			check.expression(loop->condition.get());
			check.body(loop->body);
			if (!check.ok) {
				continue;
			}
			const std::regex size_call("\\b" + array + "[ \\t]*\\.[ \\t]*size[ \\t]*\\([ \\t]*\\)");
			const std::string count = fresh("n");
			const std::string replaced = std::regex_replace(condition, size_call, count);
			if (replaced == condition) {
				continue;
			}
			condition = replaced;
			hoisted.emplace_back(count, array);
		}
		if (hoisted.empty() || !claim(loop->line, loop->line)) {
			return;
		}
		const std::string indent = parts[1].str();
		Edit edit;
		edit.first = loop->line;
		edit.last = loop->line;
		std::string names;
		for (const std::pair<std::string, std::string>& size : hoisted) {
			edit.lines.push_back({ indent + "var " + size.first + " := " + size.second + ".size()", loop->line });
			names += (names.empty() ? "" : ", ") + size.second + ".size()";
		}
		edit.lines.push_back({ indent + "while " + condition + ":" +
			(parts[3].matched ? " " + parts[3].str() : std::string()), loop->line });
		edit.note = "line " + std::to_string(loop->line) + ": size: `while` reads " + names +
			" once, before the loop";
		m_edits.push_back(std::move(edit));
	}
};

} // namespace

RewriteResult rewrite_packed_loops(const std::string& source, const Program& program,
	const RewriteOptions& options)
{
	Rewriter rewriter(source, program, options);
	for (const FunctionDecl& decl : program.functions) {
		rewriter.function(decl);
	}
	for (const StructDecl& decl : program.structs) {
		for (const FunctionDecl& method : decl.methods) {
			rewriter.function(method);
		}
	}
	return rewriter.finish();
}

} // namespace gdscript

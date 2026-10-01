#include "../source_model.h"

#include "witness/doctest.h"
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace gdscript;

TEST_CASE("the source model describes a script") {
	SourceModel model = analyze_source(
			"## Adds values\nfunc add(a: int, b: int = 2) -> int:\n\treturn a + b\nvar value: int = 3\n",
			"res://sample.sgd", ANALYZE_ALL, 3, 8);
	REQUIRE(model.declarations.size() >= 2);
	REQUIRE(model.declarations[0].documentation == "Adds values");
	REQUIRE(model.declarations[0].parameters.size() == 2);
	REQUIRE(model.declarations[0].return_type == "int");
	const std::vector<uint8_t> bytes = encode_source_model(model);
	REQUIRE(bytes == encode_source_model(model));
	SourceModel decoded;
	REQUIRE(decode_source_model(bytes.data(), bytes.size(), decoded));
	REQUIRE(decoded.path == model.path);
	REQUIRE(decoded.declarations.size() == model.declarations.size());
	for (size_t n = 0; n < bytes.size(); n++)
		REQUIRE(!decode_source_model(bytes.data(), n, decoded));
	std::vector<uint8_t> trailing = bytes;
	trailing.push_back(0);
	REQUIRE(!decode_source_model(trailing.data(), trailing.size(), decoded));

	std::vector<uint8_t> future = bytes;
	uint32_t count = 0;
	std::memcpy(&count, future.data() + 8, 4);
	count++;
	std::memcpy(future.data() + 8, &count, 4);
	const uint32_t id = 0xf00d, length = 3;
	const uint8_t *id_bytes = reinterpret_cast<const uint8_t *>(&id);
	const uint8_t *len_bytes = reinterpret_cast<const uint8_t *>(&length);
	future.insert(future.end(), id_bytes, id_bytes + 4);
	future.insert(future.end(), len_bytes, len_bytes + 4);
	future.insert(future.end(), { 1, 2, 3 });
	REQUIRE(decode_source_model(future.data(), future.size(), decoded));

	SourceModel broken = analyze_source(
			"func first(\nvar x =\nfunc later():\n\treturn 1\n", "res://broken.sgd");
	REQUIRE(broken.diagnostics.size() >= 2);
	bool later = false;
	for (const auto &decl : broken.declarations)
		later |= decl.name == "later";
	REQUIRE(later);
	// Statements, not lines: a bracket, a block string or a backslash carries a
	// statement across newlines, and a comment never opens a string.
	auto errors = [](const SourceModel &model) {
		size_t count = 0;
		for (const auto &d : model.diagnostics)
			count += d.severity == DiagnosticSeverity::ERROR;
		return count;
	};
	auto has_code = [](const SourceModel &model, const char *code) {
		for (const auto &d : model.diagnostics)
			if (d.code == code)
				return true;
		return false;
	};

	const SourceModel continued = analyze_source(
			"func setup():\n\tvar list = [\n\t\t1,\n\t\t2,\n\t]\n\tvar s = \"it's fine\" # don't worry\n\treturn list\n",
			"res://continued.sgd");
	REQUIRE(errors(continued) == 0);

	const SourceModel block = analyze_source(
			"func f():\n\tvar s = \"\"\"\nfunc not_really():\n\"\"\"\n\treturn s\n", "res://block.sgd");
	REQUIRE(errors(block) == 0);
	REQUIRE(block.declarations.size() == 2); // not_really() is string content

	const SourceModel joined = analyze_source(
			"func add(\n\t\ta: int,\n\t\tb: int = 2) -> int:\n\treturn a + b\n", "res://joined.sgd");
	REQUIRE(errors(joined) == 0);
	REQUIRE(joined.declarations[0].parameters.size() == 2);
	REQUIRE(joined.declarations[0].return_type == "int");

	// Statement keywords are statements, not discarded expressions.
	const SourceModel keywords = analyze_source(
			"func value() -> int:\n\treturn 1\nfunc f(n):\n\twhile n:\n\t\tbreak\n\t\tcontinue\n\tpass\n\treturn value()\n",
			"res://keywords.sgd");
	REQUIRE(!has_code(keywords, "STANDALONE_EXPRESSION"));
	REQUIRE(!has_code(keywords, "DISCARDED_RETURN_VALUE"));
	const SourceModel standalone = analyze_source("func f():\n\tn\n", "res://standalone.sgd");
	REQUIRE(has_code(standalone, "STANDALONE_EXPRESSION"));

	// An unclosed bracket is reported at the line that opened it, and the next
	// declaration resumes analysis instead of the error swallowing the file.
	const SourceModel unclosed = analyze_source(
			"func first(\nfunc later():\n\treturn 1\n", "res://unclosed.sgd");
	REQUIRE(has_code(unclosed, "EXPECTED_CLOSING_DELIMITER"));
	REQUIRE(unclosed.diagnostics[0].range.start_line == 1);

	const SourceModel traits = analyze_source(
			"## Can be damaged\ntrait Damageable:\n\tfunc damage(amount: int) -> bool\n",
			"res://traits.sgd");
	REQUIRE(errors(traits) == 0);
	REQUIRE(traits.declarations.size() >= 3); // trait, method, parameter
	REQUIRE(traits.declarations[0].kind == DeclarationKind::TRAIT);
	REQUIRE(traits.declarations[0].name == "Damageable");
	REQUIRE(traits.declarations[0].documentation == "Can be damaged");
	REQUIRE(traits.declarations[1].kind == DeclarationKind::FUNCTION);
	REQUIRE(traits.declarations[1].parent == 0);
	const std::vector<uint8_t> trait_bytes = encode_source_model(traits);
	SourceModel decoded_traits;
	REQUIRE(decode_source_model(trait_bytes.data(), trait_bytes.size(), decoded_traits));
	REQUIRE(decoded_traits.declarations[0].kind == DeclarationKind::TRAIT);

	SourceModel invalid_kind;
	invalid_kind.declarations.push_back(SourceDeclaration{});
	invalid_kind.declarations[0].kind = static_cast<DeclarationKind>(255);
	const std::vector<uint8_t> invalid_kind_bytes = encode_source_model(invalid_kind);
	std::string decode_error;
	REQUIRE(!decode_source_model(invalid_kind_bytes.data(), invalid_kind_bytes.size(),
								 decoded_traits, decode_error));
	REQUIRE(decode_error == "declaration 0 has unsupported kind 255");

	auto declaration_named = [](const SourceModel &model, const char *name)
			-> const SourceDeclaration * {
		for (const auto &d : model.declarations)
			if (d.name == name)
				return &d;
		return nullptr;
	};
	auto warnings_with = [](const SourceModel &model, const char *code) {
		size_t count = 0;
		for (const auto &d : model.diagnostics) {
			count += d.severity == DiagnosticSeverity::WARNING && d.code == code;
		}
		return count;
	};

	const SourceModel recovered = analyze_source(
			"func first():\n\tvar a = *\n\treturn a\n\nfunc second(value):\n\treturn value\n",
			"res://recovered.sgd");
	REQUIRE(errors(recovered) >= 1);
	REQUIRE(declaration_named(recovered, "second") != nullptr);
	REQUIRE(declaration_named(recovered, "value") != nullptr);

	const SourceModel missing = analyze_source("var x = \n", "res://missing.sgd");
	REQUIRE(has_code(missing, "EXPECTED_EXPRESSION"));
	for (const auto &d : missing.diagnostics) {
		if (d.code != "EXPECTED_EXPRESSION")
			continue;
		REQUIRE(d.range.start_line == 1);
		REQUIRE(d.range.start_column == 9);
		REQUIRE(d.range.end_column > d.range.start_column);
	}

	const SourceModel loop = analyze_source(
			"func go():\n\tfor index in 3:\n\t\tprint(index)\n\tprint(0)\n", "res://loop.sgd");
	const SourceDeclaration *index = declaration_named(loop, "index");
	REQUIRE(index != nullptr);
	REQUIRE((index->lexical_scope.start_line == 2 && index->lexical_scope.end_line == 3));

	const SourceModel bound = analyze_source(
			"func go(value):\n\tmatch value:\n\t\tvar captured:\n\t\t\tprint(captured)\n\tprint(1)\n",
			"res://match.sgd");
	const SourceDeclaration *captured = declaration_named(bound, "captured");
	REQUIRE(captured != nullptr);
	REQUIRE(captured->lexical_scope.end_line == 4);

	const SourceModel lambda = analyze_source(
			"func go():\n\tvar f = func(inner):\n\t\treturn inner\n\treturn f\n", "res://lambda.sgd");
	const SourceDeclaration *inner = declaration_named(lambda, "inner");
	REQUIRE(inner != nullptr);
	REQUIRE(inner->kind == DeclarationKind::PARAMETER);
	REQUIRE(inner->lexical_scope.end_line == 3);

	const SourceModel structs = analyze_source(
			"struct Point:\n\tvar x: int = 0\n\tvar y: int = 0\n\nfunc go():\n\tvar p := Point.new(1, 2)\n\treturn p.x\n",
			"res://struct.sgd");
	REQUIRE(errors(structs) == 0);
	const SourceDeclaration *point = declaration_named(structs, "Point");
	REQUIRE(point != nullptr);
	REQUIRE(point->kind == DeclarationKind::STRUCT);
	REQUIRE(point->children.size() == 2);
	REQUIRE(structs.declarations[size_t(point->children[0])].name == "x");
	const SourceDeclaration *p = declaration_named(structs, "p");
	REQUIRE((p != nullptr && p->resolved_type == "Point"));

	const SourceModel unknown_field = analyze_source(
			"struct Point:\n\tvar x = 0\n\nfunc go():\n\tvar p = Point.new()\n\treturn p.z\n",
			"res://field.sgd");
	REQUIRE(has_code(unknown_field, "UNKNOWN_STRUCT_FIELD"));

	const SourceModel enums = analyze_source(
			"enum Mode { IDLE, RUN = 5, STOP }\nfunc go():\n\treturn Mode.WALK\n", "res://enum.sgd");
	REQUIRE(has_code(enums, "UNDECLARED_ENUM_MEMBER"));
	const SourceDeclaration *mode = declaration_named(enums, "Mode");
	REQUIRE((mode != nullptr && mode->enum_members.size() == 3));
	REQUIRE((mode->enum_members[1].value == 5 && mode->enum_members[2].value == 6));

	const SourceModel arity = analyze_source(
			"func takes(a, b):\n\treturn a\nfunc go():\n\treturn takes(1)\n", "res://arity.sgd");
	REQUIRE(has_code(arity, "MISSING_ARGUMENT"));

	const SourceModel inferred = analyze_source("func go():\n\tvar v := Vector2()\n\treturn v\n",
												"res://inferred.sgd");
	const SourceDeclaration *v = declaration_named(inferred, "v");
	REQUIRE((v != nullptr && v->resolved_type == "Vector2"));

	const SourceModel engine_new = analyze_source(
			"func go():\n\tvar cfg := ConfigFile.new()\n\treturn cfg\n", "res://engine_new.sgd");
	const SourceDeclaration *cfg = declaration_named(engine_new, "cfg");
	REQUIRE((cfg != nullptr && cfg->resolved_type == "ConfigFile"));
	REQUIRE(!has_code(engine_new, "UNSAFE_METHOD_ACCESS"));
	const SourceModel local_new = analyze_source(
			"func go(Maker):\n\tvar made := Maker.new()\n\treturn made\n", "res://local_new.sgd");
	const SourceDeclaration *made = declaration_named(local_new, "made");
	REQUIRE((made != nullptr && made->resolved_type.empty()));

	const SourceModel extended = analyze_source("extends Node2D\nfunc go():\n\tpass\n",
												"res://extends.sgd");
	REQUIRE(extended.declarations[0].kind == DeclarationKind::CLASS);
	REQUIRE(extended.declarations[0].base_type == "Node2D");
	const std::vector<uint8_t> extended_bytes = encode_source_model(extended);
	SourceModel decoded_extended;
	REQUIRE(decode_source_model(extended_bytes.data(), extended_bytes.size(), decoded_extended));
	REQUIRE(decoded_extended.declarations[0].base_type == "Node2D");

	const SourceModel precise = analyze_source(
			"func go():\n\tvar x2 = 1\n\tvar text = \"a / b\"\n\tif x2:\n\t\treturn text\n\tprint(x2)\n",
			"res://precise.sgd");
	REQUIRE(warnings_with(precise, "INTEGER_DIVISION") == 0);
	REQUIRE(warnings_with(precise, "UNREACHABLE_CODE") == 0);
	REQUIRE(warnings_with(precise, "UNUSED_VARIABLE") == 0);

	const SourceModel divided = analyze_source("func go():\n\tvar a := 7\n\treturn a / 2\n",
											   "res://divided.sgd");
	REQUIRE(warnings_with(divided, "INTEGER_DIVISION") == 1);

	const SourceModel unreachable = analyze_source(
			"func go():\n\treturn 1\n\tprint(2)\n", "res://unreachable.sgd");
	REQUIRE(warnings_with(unreachable, "UNREACHABLE_CODE") == 1);

	const SourceModel qualified = analyze_source(
			"var mode: Node.ProcessMode\nfunc go():\n\treturn mode\n", "res://qualified.sgd");
	REQUIRE(warnings_with(qualified, "UNRESOLVED_TYPE_HINT") == 1);

	const SourceModel unknown_export = analyze_source(
			"@export_nonsense(\"a\") var x := 1\n", "res://export.sgd");
	REQUIRE(warnings_with(unknown_export, "UNSUPPORTED_ANNOTATION") == 1);

	const SourceModel known_export = analyze_source(
			"@export_range(0, 10) var x := 1\n", "res://known.sgd");
	REQUIRE(warnings_with(known_export, "UNSUPPORTED_ANNOTATION") == 0);
	const SourceModel silenced = analyze_source(
			"@warning_ignore(\"unresolved_type_hint\")\nvar mode: Node.ProcessMode\n",
			"res://silenced.sgd");
	REQUIRE(warnings_with(silenced, "UNRESOLVED_TYPE_HINT") == 0);
}

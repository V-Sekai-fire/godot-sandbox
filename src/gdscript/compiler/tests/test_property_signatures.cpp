#include "../compiler.h"
#include "../property_signature.h"

#include "witness/doctest.h"
#include <cstring>
#include <iostream>

using namespace gdscript;

static const PropertySignature *find_property(const std::vector<PropertySignature> &properties,
											  const std::string &name) {
	for (const PropertySignature &property : properties) {
		if (property.name == name) {
			return &property;
		}
	}
	return nullptr;
}

TEST_CASE("property signatures carry their hints") {
	Compiler compiler;
	CompilerOptions options;
	options.output_elf = false;
	compiler.compile(R"(
@export_range(0, 10) var count: int = 3
var ratio = 1.5
var enabled = true
var title = "hello"
var nothing = null
var items = []
var mapping = {}
var untyped
static var shared = 7
var runtime_value = make_value()
func make_value():
	return 9
)",
					 options);
	REQUIRE(!compiler.get_error_info().has_error);
	const auto &properties = compiler.get_property_signatures();
	REQUIRE(find_property(properties, "count")->default_kind == PropertyDefaultKind::INT);
	REQUIRE(find_property(properties, "count")->hint == 1);
	REQUIRE(find_property(properties, "count")->hint_string == "0.0,10.0");
	REQUIRE(find_property(properties, "ratio")->default_kind == PropertyDefaultKind::FLOAT);
	REQUIRE(find_property(properties, "enabled")->default_kind == PropertyDefaultKind::BOOL);
	REQUIRE(find_property(properties, "title")->default_kind == PropertyDefaultKind::STRING);
	REQUIRE(find_property(properties, "nothing")->default_kind == PropertyDefaultKind::NIL);
	REQUIRE(find_property(properties, "items")->default_kind == PropertyDefaultKind::EMPTY_ARRAY);
	REQUIRE(find_property(properties, "mapping")->default_kind == PropertyDefaultKind::EMPTY_DICTIONARY);
	REQUIRE(find_property(properties, "untyped")->type == -1);
	REQUIRE(find_property(properties, "shared")->is_static);
	REQUIRE(find_property(properties, "runtime_value")->default_kind == PropertyDefaultKind::NONE);

	const std::vector<uint8_t> bytes = encode_property_signatures(properties);
	std::vector<PropertySignature> decoded;
	REQUIRE(decode_property_signatures(bytes.data(), bytes.size(), decoded));
	REQUIRE(decoded.size() == properties.size());
	REQUIRE(encode_property_signatures(decoded) == bytes);

	for (size_t size = 0; size < bytes.size(); size++) {
		REQUIRE(!decode_property_signatures(bytes.data(), size, decoded));
		REQUIRE(decoded.empty());
	}
	std::vector<uint8_t> trailing = bytes;
	trailing.push_back(0);
	REQUIRE(!decode_property_signatures(trailing.data(), trailing.size(), decoded));
	std::vector<uint8_t> invalid_kind = encode_property_signatures({ PropertySignature{ "x" } });
	// Offset 63 = kind byte (after header + name + fixed fields + section lengths).
	invalid_kind[63] = 255;
	REQUIRE(!decode_property_signatures(invalid_kind.data(), invalid_kind.size(), decoded));

	Compiler sectioned;
	sectioned.compile(R"(
@export var plain := 1

@export_category("Movement")
@export_group("Speed", "speed_")
@export var speed_walk := 1.0
@export_subgroup("Limits")
@export var speed_max := 10.0
@export_group("Jump")
@export var jump_height := 2.0
)",
					  options);
	REQUIRE(!sectioned.get_error_info().has_error);
	const auto &sections = sectioned.get_property_signatures();
	REQUIRE(find_property(sections, "plain")->section.is_default());
	REQUIRE(find_property(sections, "speed_walk")->section.category == "Movement");
	REQUIRE(find_property(sections, "speed_walk")->section.group == "Speed");
	REQUIRE(find_property(sections, "speed_walk")->section.group_prefix == "speed_");
	REQUIRE(find_property(sections, "speed_walk")->section.subgroup.empty());
	REQUIRE(find_property(sections, "speed_max")->section.subgroup == "Limits");
	REQUIRE(find_property(sections, "jump_height")->section.group == "Jump");
	REQUIRE(find_property(sections, "jump_height")->section.group_prefix.empty());
	REQUIRE(find_property(sections, "jump_height")->section.subgroup.empty());
	REQUIRE(find_property(sections, "jump_height")->section.category == "Movement");

	const std::vector<uint8_t> section_bytes = encode_property_signatures(sections);
	std::vector<PropertySignature> section_decoded;
	REQUIRE(decode_property_signatures(section_bytes.data(), section_bytes.size(),
									   section_decoded));
	REQUIRE(encode_property_signatures(section_decoded) == section_bytes);
	REQUIRE(find_property(section_decoded, "speed_max")->section.subgroup == "Limits");

	std::vector<uint8_t> legacy;
	auto put32 = [&legacy](uint32_t value) {
		for (int i = 0; i < 4; i++)
			legacy.push_back(uint8_t(value >> (8 * i)));
	};
	auto put16 = [&legacy](uint16_t value) {
		legacy.push_back(uint8_t(value));
		legacy.push_back(uint8_t(value >> 8));
	};
	put32(0x50524753u);
	put16(1);
	put16(0);
	put32(1);
	put32(1);
	legacy.push_back('x');
	put32(uint32_t(-1));
	put32(0);
	put32(0);
	put32(0);
	put32(0);
	put32(7);
	legacy.push_back(1);
	legacy.push_back(0);
	legacy.push_back(uint8_t(PropertyDefaultKind::NONE));
	std::vector<PropertySignature> legacy_decoded;
	REQUIRE(decode_property_signatures(legacy.data(), legacy.size(), legacy_decoded));
	REQUIRE(legacy_decoded.size() == 1);
	REQUIRE(legacy_decoded[0].name == "x");
	REQUIRE(legacy_decoded[0].declaration_line == 7);
	REQUIRE(legacy_decoded[0].is_member);
	REQUIRE(legacy_decoded[0].section.is_default());
}

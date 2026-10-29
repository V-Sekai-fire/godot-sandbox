#include <catch2/catch_test_macros.hpp>

#include <libriscv/machine.hpp>
#include <dlfcn.h>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static", bool cpp = false);

namespace {
struct TemporaryFile {
	std::string path;
	explicit TemporaryFile(const char *suffix) {
		std::string pattern = std::string("/tmp/libriscv-bintr-XXXXXX") + suffix;
		std::vector<char> writable(pattern.begin(), pattern.end());
		writable.push_back('\0');
		const int fd = mkstemps(writable.data(), std::char_traits<char>::length(suffix));
		REQUIRE(fd >= 0);
		close(fd);
		path = writable.data();
	}
	~TemporaryFile() { unlink(path.c_str()); }
};

void write_source(const std::string& path, const std::string& source) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	REQUIRE(file.is_open());
	file << source;
	REQUIRE(file.good());
}
}

TEST_CASE("Emit cache-loadable and embeddable C99 independently", "[Bintr]") {
	if (std::system("cc --version >/dev/null 2>&1") != 0) {
		SKIP("a system C compiler is not available");
	}

	const auto binary = build_and_load(R"(
		__attribute__((noinline)) int square(int value) { return value * value; }
		__asm__(".global _start\n"
		        "_start:\n"
		        "li a0, 7\n"
		        "call square\n"
		        "li a7, 1\n"
		        "ecall\n");
	)", "-O2 -static -ffreestanding -nostdlib -nostartfiles");
	std::string embeddable;
	std::string shared;
	riscv::MachineOptions<riscv::RISCV64> options;
	options.memory_max = 8u << 20;
	options.translate_enabled = false;
	options.translate_enable_embedded = false;
	options.translate_invoke_compiler = false;
	options.cross_compile.emplace_back(riscv::MachineTranslationEmbeddableCodeOptions{
		.result_c99 = &embeddable,
		.result_shared_c99 = &shared,
	});
	riscv::Machine<riscv::RISCV64> machine(binary, options);

	REQUIRE_FALSE(embeddable.empty());
	REQUIRE_FALSE(shared.empty());
	REQUIRE(embeddable.find("#define EMBEDDABLE_CODE 1") != std::string::npos);
	REQUIRE(shared.find("#define RISCV_TRANSLATION_DYLIB 8") != std::string::npos);

	TemporaryFile shared_c(".c");
	TemporaryFile shared_so(".so");
	write_source(shared_c.path, shared);
	const std::string shared_command = "cc -shared -fPIC -O2 -w -o " +
		shared_so.path + " " + shared_c.path;
	REQUIRE(std::system(shared_command.c_str()) == 0);
	void *shared_handle = dlopen(shared_so.path.c_str(), RTLD_NOW);
	REQUIRE(shared_handle != nullptr);
	REQUIRE(dlsym(shared_handle, "init") != nullptr);
	REQUIRE(dlsym(shared_handle, "no_mappings") != nullptr);
	dlclose(shared_handle);

	TemporaryFile embedded_c(".c");
	TemporaryFile embedded_so(".so");
	write_source(embedded_c.path, embeddable);
	const std::string embedded_command = "cc -shared -fPIC -O2 -w -DCALLBACK_INIT -o " +
		embedded_so.path + " " + embedded_c.path;
	REQUIRE(std::system(embedded_command.c_str()) == 0);
	void *embedded_handle = dlopen(embedded_so.path.c_str(), RTLD_NOW);
	REQUIRE(embedded_handle != nullptr);
	REQUIRE(dlsym(embedded_handle, "init") == nullptr);
	REQUIRE(dlsym(embedded_handle, "no_mappings") == nullptr);
	dlclose(embedded_handle);
}

namespace {
// A guest with enough separate functions that its translation has several blocks.
const char* split_guest = R"(
	#define F(n, k) __attribute__((noinline)) unsigned long f##n(unsigned long x) { \
		for (int i = 0; i < 40 + n; i++) x = x * k + (x >> 7) + i; return x; }
	F(0, 3) F(1, 5) F(2, 7) F(3, 11) F(4, 13) F(5, 17) F(6, 19) F(7, 23)
	F(8, 29) F(9, 31) F(10, 37) F(11, 41) F(12, 43) F(13, 47) F(14, 53) F(15, 59)
	__attribute__((noinline)) unsigned long fib(unsigned long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
	__attribute__((noinline)) int run(void) {
		unsigned long x = 1;
		for (int r = 0; r < 50; r++) {
			x = f0(x) ^ f1(x) ^ f2(x) ^ f3(x) ^ f4(x) ^ f5(x) ^ f6(x) ^ f7(x);
			x = f8(x) ^ f9(x) ^ f10(x) ^ f11(x) ^ f12(x) ^ f13(x) ^ f14(x) ^ f15(x);
		}
		return (int)((x ^ fib(20)) & 0x7fffffff);
	}
	void _start(void) {
		register long a0 __asm__("a0") = run();
		register long a7 __asm__("a7") = 1;
		__asm__ volatile("ecall" : : "r"(a0), "r"(a7));
		for (;;) {}
	}
)";

int run_guest(const std::vector<uint8_t>& binary, riscv::MachineOptions<riscv::RISCV64> options, bool* translated) {
	options.memory_max = 8u << 20;
	// Each run decodes the guest afresh, rather than reusing a segment an earlier
	// machine decoded (and translated, or not) for the same program.
	options.use_shared_execute_segments = false;
	riscv::Machine<riscv::RISCV64> machine(binary, options);
	machine.install_syscall_handler(1, [] (auto& m) { m.stop(); });
	machine.simulate(500'000'000ull);
	if (translated) *translated = machine.is_binary_translation_enabled();
	return machine.return_value<int>();
}

// Compile each unit to an object, then link them into one shared library.
// Returns the linker's exit status; dropping a unit must make the link fail.
int link_units(const std::vector<std::string>& units, const std::string& out, size_t drop) {
	std::vector<std::unique_ptr<TemporaryFile>> objects;
	std::string link = "cc -shared -fPIC -o " + out;
#ifdef __APPLE__
	link += " -Wl,-undefined,error";
#else
	link += " -Wl,--no-undefined";
#endif
	for (size_t i = 0; i < units.size(); i++) {
		if (i == drop) continue;
		TemporaryFile c(".c");
		write_source(c.path, units[i]);
		objects.push_back(std::make_unique<TemporaryFile>(".o"));
		const std::string cc = "cc -c -fPIC -O1 -w -o " + objects.back()->path + " " + c.path;
		REQUIRE(std::system(cc.c_str()) == 0);
		link += " " + objects.back()->path;
	}
	link += " 2>/dev/null";
	return std::system(link.c_str());
}
}

TEST_CASE("A translation split into units links into a library that runs the guest", "[Bintr]") {
	if (std::system("cc --version >/dev/null 2>&1") != 0) {
		SKIP("a system C compiler is not available");
	}
	const auto binary = build_and_load(split_guest, "-O1 -static -ffreestanding -nostdlib -nostartfiles");

	riscv::MachineOptions<riscv::RISCV64> interp;
	interp.translate_enabled = false;
	bool translated = true;
	const int expected = run_guest(binary, interp, &translated);
	REQUIRE_FALSE(translated);

	// Emit the translation as one unit and as four.
	std::string whole;
	std::vector<std::string> units;
	riscv::MachineOptions<riscv::RISCV64> emit;
	emit.memory_max = 8u << 20;
	emit.translate_enabled = false;
	emit.translate_enable_embedded = false;
	emit.translate_invoke_compiler = false;
	emit.translation_cache = false;
	emit.use_shared_execute_segments = false;
	// Small blocks, so that this small guest still spreads over every unit.
	emit.translate_block_split = 16;
	emit.cross_compile.emplace_back(riscv::MachineTranslationEmbeddableCodeOptions{
		.result_shared_c99 = &whole,
		.result_shared_c99_units = &units,
		.shared_c99_units = 4,
	});
	riscv::Machine<riscv::RISCV64> emitter(binary, emit);
	const uint32_t hash = emitter.memory.exec_segment_for(emitter.memory.start_address())->translation_hash();

	REQUIRE(units.size() == 4);
	REQUIRE(units[0].find("#define RISCV_TRANSLATION_UNIT 0") != std::string::npos);
	REQUIRE(units[0].find("VISIBLE const uint32_t no_mappings") != std::string::npos);
	for (size_t i = 1; i < units.size(); i++) {
		REQUIRE(units[i].find("#define RISCV_TRANSLATION_UNIT " + std::to_string(i)) != std::string::npos);
		REQUIRE(units[i].find("no_mappings") == std::string::npos);
	}
	// Every unit carries code: the blocks are spread, not left in the first unit.
	for (const auto& unit : units)
		REQUIRE(unit.find("BINTR_FUNC ReturnValues f_") != std::string::npos);

	char dir_pattern[] = "/tmp/libriscv-units-XXXXXX";
	REQUIRE(mkdtemp(dir_pattern) != nullptr);
	const std::string prefix = std::string(dir_pattern) + "/rvbintr-";
	const std::string library = riscv::MachineOptions<riscv::RISCV64>::translation_filename(prefix, hash, ".so");

	// Control: without unit 2 the link fails, naming the functions it held.
	REQUIRE(link_units(units, library, 2) != 0);

	REQUIRE(link_units(units, library, units.size()) == 0);
	{
		void* h = dlopen(library.c_str(), RTLD_NOW);
		INFO("dlopen: " << (h ? "ok" : dlerror()));
		REQUIRE(h != nullptr);
		REQUIRE(dlsym(h, "init") != nullptr);
		REQUIRE(dlsym(h, "no_mappings") != nullptr);
		dlclose(h);
	}
	riscv::MachineOptions<riscv::RISCV64> load;
	load.translate_enabled = true;
	load.translate_invoke_compiler = false;
	load.translation_cache = true;
	load.translation_prefix = prefix;
	load.translation_suffix = ".so";
	load.translate_block_split = 16;
	setenv("BINTR_CACHE", "1", 1);
	translated = false;
	const int got = run_guest(binary, load, &translated);
	unsetenv("BINTR_CACHE");
	unlink(library.c_str());
	rmdir(dir_pattern);

	REQUIRE(translated);
	REQUIRE(got == expected);
}

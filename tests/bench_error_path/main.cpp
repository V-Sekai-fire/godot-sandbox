#include "common.h"
#include <chrono>
#include <random>
#include <vector>
#include <cstdlib>
Handler get_macro_handler0(); Handler get_macro_handler1();
bool call_macro0(Handler, Machine &, uint64_t, int); bool call_macro1(Handler, Machine &, uint64_t, int);
int main(int argc, char **argv) {
	const int N = argc > 1 ? atoi(argv[1]) : 200000, RUNS = 8;
	const int depths[] = {2, 6, 12};
	const double ps[] = {1.0, 0.05, 0.0};
	std::mt19937_64 rng(1234);
	puts("depth,p,variant,run,ns_per_call,faults");
	for (int d : depths) for (double p : ps) {
		std::vector<uint64_t> addrs(N);
		for (auto &a : addrs) a = (std::uniform_real_distribution<>(0, 1)(rng) < p) ? 0 : 1 + (rng() & 0xffff);
		for (int v = 0; v < 3; v++) for (int run = 0; run < RUNS; run++) {
			Machine m; uint64_t faults = 0;
			Handler h = v == 0 ? get_exc_handler() : v == 1 ? get_macro_handler0() : get_macro_handler1();
			auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < N; i++) {
				m.stopped = false;
				bool f = v == 0 ? call_exc(h, m, addrs[i], d) : v == 1 ? call_macro0(h, m, addrs[i], d) : call_macro1(h, m, addrs[i], d);
				faults += f;
			}
			double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
			printf("%d,%.2f,%s,%d,%.2f,%llu\n", d, p, v == 0 ? "throw" : v == 1 ? "macro_v0" : "macro_v1_cold", run, ns, (unsigned long long)faults);
		}
	}
}

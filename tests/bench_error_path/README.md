# Error path: throw vs SB_FAIL

Model of a syscall handler, a helper chain and the vmcall boundary. Build and run:

    clang++ -O2 -std=c++20 -fexceptions    -c exc.cpp -o exc.o
    clang++ -O2 -std=c++20 -fno-exceptions -DSB_VARIANT=0 -c macro.cpp -o m0.o
    clang++ -O2 -std=c++20 -fno-exceptions -DSB_VARIANT=1 -c macro.cpp -o m1.o
    clang++ -O2 -std=c++20 -fexceptions    -c main.cpp -o main.o
    clang++ main.o exc.o m0.o m1.o -o bench && ./bench 200000 > results.csv && python3 frontier.py

`frontier.py` scores each depth x fault-rate cell over 8 runs: a run succeeds at 2x faster than
throw on faulting cells, or within 5% of throw on the no-fault cell. Result on macOS arm64
(clang -O2): 72 of 72 runs for both variants, 40x to 160x faster on the fault path; v0 (inline)
and v1 (cold function) are equal within noise, so `SB_FAIL` records inline.

Limits: a model, not libriscv; one platform. libriscv's own guest-fault throws are not covered.

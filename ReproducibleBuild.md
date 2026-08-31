Reproducible build instructions
==============================

Purpose: produce two independent, byte-identical builds of `build/db_cli` and publish both hashes and build logs.

Recommended exact commands
-------------------------
Set reproducibility environment (prefer using commit timestamp):

```bash
# use commit timestamp so builds are tied to the repo state
export SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)

# normalize debug/source paths and strip debug info
export CFLAGS="-g0 -fdebug-prefix-map=$PWD=."
export CXXFLAGS="-g0 -fdebug-prefix-map=$PWD=."

# disable build-id
export LDFLAGS="-Wl,--build-id=none"

# tool versions
gcc --version -> gcc (GCC) 16.1.1 20260728
cmake --version -> cmake version 4.4.2 (kitware)
ld --version -> GNU ld (GNU Binutils) 2.47
uname -a -> Linux pranarchdell 7.1.6-arch1-1 #1 SMP PREEMPT_DYNAMIC Tue, 04 Aug 2026 11:19:27 +0000 x86_64 GNU/Linux
```

Build #1 (clean directory) and capture logs:

```bash
rm -rf build1
cmake -S . -B build1 -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" 2>&1 | tee build1_config.log
cmake --build build1 -j 2>&1 | tee build1_build.log
ctest --test-dir build1 --output-on-failure 2>&1 | tee build1_ctest.log || true

sha256sum build1/db_cli > build1.sha256
```

Build #2 (independent clean directory) and capture logs:

```bash
rm -rf build2
cmake -S . -B build2 -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" 2>&1 | tee build2_config.log
cmake --build build2 -j 2>&1 | tee build2_build.log
ctest --test-dir build2 --output-on-failure 2>&1 | tee build2_ctest.log || true

sha256sum build2/db_cli > build2.sha256
```

Compare the artifacts:

```bash
cat build1.sha256 build2.sha256
sha256sum -c build1.sha256 || true
sha256sum -c build2.sha256 || true
cmp --silent build1/db_cli build2/db_cli && echo "IDENTICAL" || echo "DIFFER"
```

The hash i got:
```bash
2c44be1ef7bc984decfc3f2883554f26f7632b80a18ff6e45f49953e9fb20f95  build1/db_cli
2c44be1ef7bc984decfc3f2883554f26f7632b80a18ff6e45f49953e9fb20f95  build2/db_cli
```

CI recommendation
- Run the same `reproduce.sh` on a CI runner (GitHub Actions) and publish the CI artifact and its sha256. Independent CI + local builds make faking much harder.

If you'd like, I can create a `reproduce.sh` file in the repo and attempt two deterministic builds here and report whether the artifacts match. Reply "do it" to proceed.

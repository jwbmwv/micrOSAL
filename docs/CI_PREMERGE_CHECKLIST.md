# MicrOSAL CI Pre-Merge Checklist

Use this checklist before opening or merging a pull request.
It mirrors the guardrails in [Agents.md](../Agents.md) and maps directly to current CI lanes.

## 1) Baseline Gates (always)

- [ ] Formatting check passes.
- [ ] Hosted Linux configure/build/test passes.
- [ ] Core clang-tidy pass succeeds.
- [ ] Header, CMake, and Doxygen versions agree (checked automatically during standalone configuration).

Commands:

```bash
CLANG_FORMAT=clang-format-18 ./scripts/format.sh --check
cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
TIDY_JOBS=$(nproc) BUILD_DIR=build ./scripts/tidy.sh --warnings-as-errors='*'
```

## 2) Change-Type Gates (apply what changed)

### A) Public API/ABI change under include/

- [ ] Linux hosted regression completed.
- [ ] clang-tidy rerun from fresh compile database completed.
- [ ] Public contract docs updated when behavior changed.

### B) Backend implementation change under src/<backend>/

- [ ] Touched backend path validated.
- [ ] Hosted Linux regression completed.
- [ ] Capability traits and is_supported behavior still match implementation.

### C) Shared emulation/common change under src/common/ or shared .inl

- [ ] Affected tests rebuilt with clean-first.
- [ ] Synchronization/timing-sensitive slices rerun.

Suggested focused slice:

```bash
cmake --build build --parallel --clean-first --target test_condvar test_event_flags test_work_queue
ctest --test-dir build --output-on-failure -R '^(test_condvar|test_event_flags|test_work_queue)$'
```

### D) Build/config/tooling change

- [ ] Fresh configure performed from scratch.
- [ ] At least one full hosted test pass completed.
- [ ] Installed-package consumer and extracted README example pass.

```bash
ctest --test-dir build --output-on-failure -R '^(test_installed_package|test_readme_example)$'
```

For correctness hardening, include the portable queue/flags contract and the
Linux fault-injection tests:

```bash
cmake --build build --parallel --target test_embedded_adapters test_delayable_work_faults
ctest --test-dir build --output-on-failure -R '^(test_embedded_adapters|test_delayable_work_faults)$'
```

## 3) Backend-Specific Validation (when relevant)

### FreeRTOS

- [ ] FreeRTOS simulation tests pass.

```bash
cmake -B build-freertos tests/freertos -DCMAKE_BUILD_TYPE=Debug
cmake --build build-freertos --parallel
ctest --test-dir build-freertos --output-on-failure
```

### Zephyr

- [ ] Zephyr test lane rerun in isolated west/module context when Zephyr code paths changed.
- [ ] Both v3.7.0 LTS (SDK 0.16.8) and v4.5.0 (SDK 1.0.1) remain in the
      `native_sim` and nRF52840 Renode matrix; the advisory discovery sweep follows v4.5.0.
- [ ] The v4.5.0 workspace uses CMake 3.28+ and Python 3.12+.
- [ ] Capability-gated behavior remains explicit (for example high-resolution clock and CPU query support).

Use the isolated-workspace commands in
[Test Coverage](TestCoverage.md#running-the-zephyr-suite). Do not change another
project's west manifest or present a newer development checkout as v4.5.0 evidence.

### ThreadX

- [ ] Pinned SDK adapter build and Linux/GNU-port tests pass when ThreadX changed.
- [ ] Simulator evidence is not presented as board/ISR/SMP certification.
- [ ] Started-timer reclamation remains explicitly unsupported.
- [ ] Cross-task readiness/cancellation, in-flight callback close, native timeout,
      and semaphore-exhaustion/recovery checks pass.

```bash
cmake -S tests/threadx -B build-threadx -DCMAKE_BUILD_TYPE=Debug
cmake --build build-threadx --parallel
ctest --test-dir build-threadx --output-on-failure --timeout 30
```

### NuttX

- [ ] NuttX sim validation rerun when NuttX path changed.
- [ ] Public headers using atomics include `osal/detail/atomic_compat.hpp`
      rather than `<atomic>` directly, including when included before `osal.hpp`.
- [ ] The test app links the shared C bridge as well as the NuttX backend;
      C++ mutex configuration also depends on the shared bridge.

### Optional C++23 APIs

- [ ] The feature-presence gate succeeds with the same compiler/library used by the tests.
- [ ] Expected adapters are not silently compiled out of the C++23 validation run.

```bash
cmake -B build-cxx23 -DOSAL_BACKEND=LINUX -DCMAKE_CXX_STANDARD=23 \
    -DOSAL_REQUIRE_STD_EXPECTED=ON -DOSAL_BUILD_TESTS=ON
cmake --build build-cxx23 --parallel
ctest --test-dir build-cxx23 --output-on-failure
```

## 4) Off-Rails Checks (must stay true)

- [ ] No new backend-specific ABI hooks were introduced when composition over existing primitives is sufficient.
- [ ] Compile-time capability decisions were not replaced with runtime-only branching unless required by design.
- [ ] Error semantics remain explicit and predictable via osal::result/osal::error_code.
- [ ] Behavior-changing work includes tests, or documents why tests are infeasible.
- [ ] Simulation/CI evidence is treated as regression evidence, not hardware sign-off.

## 5) Release Consistency (when versioning/releasing)

- [ ] Version sync confirmed across:
  - include/osal/version.hpp
  - CMakeLists.txt
  - Doxyfile
- [ ] Doxygen generation succeeds.
- [ ] Generated Doxygen HTML/LaTeX/XML remains untracked and is not included in the release changeset.

Commands:

```bash
cmake -P cmake/check_version.cmake
rm -rf docs/doxygen/html docs/doxygen/latex docs/doxygen/xml
doxygen Doxyfile
```

## 6) CI Job Mapping

Current workflow jobs in [.github/workflows/ci.yml](../.github/workflows/ci.yml):

- format-check
- build-test
- cxx23-test
- cxx26-test (advisory, non-blocking; requires native `std::inplace_vector`)
- clang-tidy
- clang-tidy-tests (non-blocking)
- sanitizers
- freertos-test
- threadx-test
- nuttx-test
- zephyr-test

When a change impacts one of these paths, ensure the corresponding lane has been exercised locally or in PR CI.

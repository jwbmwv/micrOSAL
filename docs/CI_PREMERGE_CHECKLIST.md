# MicrOSAL CI Pre-Merge Checklist

Use this checklist before opening or merging a pull request.
It mirrors the guardrails in [Agents.md](../Agents.md) and maps directly to current CI lanes.

## 1) Baseline Gates (always)

- [ ] Formatting check passes.
- [ ] Hosted Linux configure/build/test passes.
- [ ] Core clang-tidy pass succeeds.

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
- [ ] Capability-gated behavior remains explicit (for example high-resolution clock and CPU query support).

### NuttX

- [ ] NuttX sim validation rerun when NuttX path changed.

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

Commands:

```bash
rm -rf docs/doxygen/html docs/doxygen/latex docs/doxygen/xml
doxygen Doxyfile
```

## 6) CI Job Mapping

Current workflow jobs in [.github/workflows/ci.yml](../.github/workflows/ci.yml):

- format-check
- build-test
- clang-tidy
- clang-tidy-tests (non-blocking)
- sanitizers
- freertos-test
- nuttx-test
- zephyr-test

When a change impacts one of these paths, ensure the corresponding lane has been exercised locally or in PR CI.

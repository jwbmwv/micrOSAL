# MicrOSAL Agent Rules

This file is the canonical contributor/agent playbook for this repository.
It distills the rules used to build and maintain MicrOSAL.

## 1) Purpose and Scope

- Keep MicrOSAL a thin, compile-time OS abstraction with predictable behavior.
- Favor correctness and portability across supported backends over cleverness.
- Keep changes aligned with the existing public API and design constraints.

## 2) Core Architecture Invariants

- Language baseline is C++20.
- Exactly one backend is selected per build (`OSAL_BACKEND`).
- No virtual dispatch in the public abstraction layer.
- No RTTI requirement.
- No exceptions for API error flow; use `osal::result` / `osal::error_code`.
- Capability-gated features must use `is_supported` / `require_support()` patterns.
- Header-level wrappers stay thin; backend behavior lives in backend source files.

## 3) Primitive and ABI Rules

- `osal::mailbox<T>` is intentionally implemented as `osal::queue<T, 1>`.
- Do not add a separate mailbox backend ABI surface.
- C API mailbox behavior is modeled through queue capacity = 1.
- Keep higher-level helpers portable and built on existing primitives:
  - `osal::notification<Slots>`
  - `osal::delayable_work`
  - `osal::object_wait_set`
- Do not introduce backend-native ABI hooks for these helpers unless the project explicitly adopts that contract.
- `osal::wait_set` and `spinlock` remain capability-sensitive APIs.

## 4) Memory and Allocation Model

- Preserve config/data split where immutable config can stay read-only.
- Static-pool style backends should remain allocation-free in normal operation.
- POSIX-family backends may use small host-managed control allocations where required.
- Do not introduce hidden heap dependencies in paths intended for static-pool targets.

## 5) Bare-Metal and ISR Semantics

- Production bare-metal integrations must drive `osal_baremetal_tick()` or `osal_baremetal_tick_with_timers()` from target timer interrupt flow.
- The hosted self-tick path is test-only behavior.
- ISR-safe APIs are only valid where the backend advertises ISR capability.
- Keep ISR work bounded and avoid blocking/policy logic in ISR context.
- For timer-expiry to work-queue handoff, respect `timer_callbacks_may_run_in_isr` capability semantics.

## 6) Code Style and Formatting

- Canonical formatter is `clang-format-18` with repository `.clang-format`.
- Formatting helper:

```bash
./scripts/format.sh
```

- Formatting check:

```bash
CLANG_FORMAT=clang-format-18 ./scripts/format.sh --check
```

- Editor defaults from `.editorconfig`:
  - UTF-8, LF, final newline
  - C/C++ indentation: 4 spaces
  - Markdown/YAML/JSON follow repo-specific sizes

## 7) Static Analysis Rules

- Use `scripts/tidy.sh` as the authoritative clang-tidy entrypoint.
- `scripts/tidy.sh` reads translation units from `compile_commands.json`.
- Run tidy against a configured build directory, commonly Linux-hosted for broad coverage.

Example setup:

```bash
cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
./scripts/tidy.sh
```

## 8) Build and Test Baseline

Primary local regression slice:

```bash
cmake -B build -DOSAL_BACKEND=LINUX -DOSAL_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Additional validation is backend-dependent (FreeRTOS, Zephyr, NuttX, etc.).
Simulation and hosted CI runs are regression evidence, not hardware sign-off.

## 9) Zephyr Integration Rules

- Keep MicrOSAL integrated as a Zephyr module.
- In standalone local setups, ensure west/manifest isolation is correct before concluding Zephyr failures are code regressions.
- Keep Zephyr-specific capability behavior explicit (for example, high-resolution clock and current CPU query gating must reflect available Zephyr config symbols).

## 10) Release Consistency Rules

When versioning/releasing, keep these in sync:

- `include/osal/version.hpp`
- `CMakeLists.txt`
- `Doxyfile`

Release verification should include format check, build, tests, and Doxygen generation.

## 11) Change Discipline for Agents

- Make minimal, targeted changes.
- Do not broaden scope without clear need.
- Preserve public API behavior unless the task explicitly changes it.
- Update or add tests when behavior changes.
- Prefer compile-time guarantees over runtime branching where feasible.
- Prefer portability-safe implementations over backend-specialized shortcuts unless required.

## 12) Quick Pre-PR Checklist

- Code is formatted (`scripts/format.sh` or `--check` passes).
- Relevant clang-tidy slice is clean.
- Relevant CTest / backend validation passes.
- Capability and ISR semantics are preserved.
- Documentation updated if API/behavior/contract changed.
- Use `docs/CI_PREMERGE_CHECKLIST.md` as the executable pre-merge runbook.

## 13) Source of Truth Order

When guidance conflicts, follow this order:

1. Public contract and release boundary: `docs/RELEASE_CONTRACT.md`
2. Architecture and invariants: `docs/design.md`
3. Backend porting contract: `docs/backend_integration.md`
4. Build/test instructions and project overview: `README.md`
5. Tooling behavior: `scripts/format.sh`, `scripts/tidy.sh`, `.clang-format`, `.clang-tidy`, `.editorconfig`

Do not rely on implicit conventions when a repository document states behavior explicitly.

## 14) Required Validation by Change Type

- Header-only API/ABI change under `include/`:
  - Build and run hosted Linux tests.
  - Run clang-tidy from a fresh or updated compile database.
  - Update docs if behavior/contract changed.
- Backend implementation change under `src/<backend>/`:
  - Validate the touched backend path plus hosted Linux regression.
  - Confirm capability flags and `is_supported` behavior still match implementation.
- Shared emulation/common change under `src/common/` or shared `.inl`:
  - Prefer clean-first rebuild of affected tests before `ctest` slice.
  - Verify no regressions in synchronization or timing-sensitive suites.
- Build/config change (`CMakeLists.txt`, `Kconfig`, tool scripts):
  - Reconfigure from scratch and verify at least one full hosted test pass.

## 15) Off-Rails Prevention Rules

- Do not introduce new backend-specific ABI hooks when composition over existing primitives is sufficient.
- Do not convert compile-time capability decisions into runtime-only branching unless required by the design.
- Do not weaken error semantics (keep `osal::result`/`error_code` behavior explicit and predictable).
- Do not merge behavior-changing work without corresponding tests or a documented rationale for test infeasibility.
- Do not treat simulator or hosted CI success as hardware deployment proof.

## 16) Definition of Done

A change is done only when all are true:

1. Scope remains minimal and aligned with this file's architecture rules.
2. Formatting and relevant static analysis pass.
3. Relevant tests pass for changed surfaces.
4. Capability/ISR behavior remains coherent with backend traits.
5. Contract docs are updated if externally visible behavior changed.

## 17) No-Touch and Generated Paths

- Do not edit generated or transient outputs unless the task explicitly targets them.
- Treat these as read-only by default: `build*/`, `twister-out*/`, and other tool-generated output directories.
- Do not modify vendored external code under `tests/nuttx/vendor/` unless the task explicitly requires a vendor update.

## 18) Change Boundary Rules

- No drive-by refactors or opportunistic reformatting in unrelated files.
- Touch only files required to satisfy the requested change.
- If mechanical cleanup is needed, submit it separately from behavior changes.

## 19) Validation Evidence Requirements

- Pull requests must include exact validation commands that were run.
- Pull requests must include the backend/configuration context used for validation.
- If a required validation item is skipped, provide a brief reason and risk statement.

## 20) CI Gate Policy

- Treat these as merge-blocking unless repository settings explicitly say otherwise:
  - `format-check`
  - `build-test`
  - `clang-tidy`
  - `sanitizers`
  - `freertos-test`
  - `nuttx-test`
  - `zephyr-test`
- `clang-tidy-tests` is advisory (`continue-on-error`) and should still be triaged before release branches.

## 21) API/ABI Compatibility Discipline

- Changes under `include/` must explicitly state API/ABI impact in the PR summary.
- If public behavior changes, update docs and tests in the same PR.
- Avoid silent contract shifts; document migration impact when relevant.

## 22) Failure and Escalation Protocol

- If unrelated failures appear, do not mask or bypass them; report them in the PR validation notes.
- If repository state changes unexpectedly during work, pause and request direction before proceeding.
- If required tooling or environment is unavailable, stop at the safe boundary, report what was verified, and identify the remaining gap.

## 23) License and Notice Hygiene

- Do not alter license headers, `LICENSE`, or `NOTICE` unless the task explicitly includes legal/compliance updates.
- Preserve existing SPDX identifiers and copyright notices.

## 24) Version-Bump Rule

- Any release version bump must update all version-bearing files in one changeset:
  - `include/osal/version.hpp`
  - `CMakeLists.txt`
  - `Doxyfile`
- Run release verification commands after the synchronized update.

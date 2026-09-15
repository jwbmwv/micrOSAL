# Pull Request

## Summary

Briefly describe the change and the problem it solves.

## Scope Guard

- [ ] This change stays minimal and focused.
- [ ] I did not introduce backend-specific ABI hooks where composition over existing primitives is sufficient.
- [ ] I preserved compile-time capability behavior unless a design change explicitly required otherwise.
- [ ] I did not modify generated output directories or vendored code unless the task explicitly required it.
- [ ] I avoided drive-by refactors/reformatting in unrelated files.

## Type of Change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation update
- [ ] CI or tooling

## Changed Surface

- [ ] `include/` public API or ABI
- [ ] `src/<backend>/` backend implementation
- [ ] `src/common/` or shared `.inl` emulation/common path
- [ ] Build/config (`CMakeLists.txt`, `Kconfig`, scripts, workflow)

## API/ABI Impact (required if `include/` changed)

Describe whether this change is API-compatible and ABI-compatible.
If behavior changed, describe migration impact.

## Required Validation (select and fill relevant items)

### If `include/` changed

- [ ] Hosted Linux build + tests passed
- [ ] clang-tidy passed from current `compile_commands.json`
- [ ] Contract/design docs updated if externally visible behavior changed

### If `src/<backend>/` changed

- [ ] Touched backend path validated
- [ ] Hosted Linux regression passed
- [ ] Capability flags and `is_supported` behavior still match implementation

### If `src/common/` or shared `.inl` changed

- [ ] Clean-first rebuild used for affected tests
- [ ] Synchronization/timing-sensitive test slice passed

### If build/config changed

- [ ] Fresh configure/build completed
- [ ] At least one full hosted CTest pass completed

## Core Checklist

- [ ] My code follows the project style
- [ ] I added tests that prove the behavior or explain why tests are not applicable
- [ ] I updated documentation where necessary
- [ ] I considered relevant backend capability and ISR behavior

## Definition of Done

- [ ] Formatting and relevant static analysis pass
- [ ] Relevant tests pass for changed surfaces
- [ ] Capability/ISR behavior remains coherent with backend traits
- [ ] Contract docs are updated if externally visible behavior changed

## CI Gate Status

- [ ] Blocking lanes are green (`format-check`, `build-test`, `clang-tidy`, `sanitizers`, `freertos-test`, `nuttx-test`, `zephyr-test`).
- [ ] Advisory lane reviewed (`clang-tidy-tests`) and any findings are triaged.

## Validation

List the commands or target environments used to validate the change.

Include exact commands and backend/config context.
If any required validation was skipped, include reason and risk statement.

Reference checklist: `docs/CI_PREMERGE_CHECKLIST.md`

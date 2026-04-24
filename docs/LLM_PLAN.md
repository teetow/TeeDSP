# TeeDSP Slow-and-Right Plan

This document defines how to restart and verify TeeDSP with minimal human interaction.

## 1. Reset architecture boundaries

1. APO runtime module
- COM registration, format negotiation, realtime processing, shared memory producer.
- No UI policy logic.

2. DSP core module
- Pure processors and chain orchestration.
- No Windows, registry, or Qt dependencies.

3. Control and install module
- TeeDspConfig app, shared memory client, install/uninstall scripts.
- No realtime processing callbacks.

## 2. Verification-first development workflow

1. Start with contract tests
- Shared memory schema compatibility.
- Parameter snapshot round-trip and seqlock semantics.

2. Add deterministic DSP tests
- Processor-level vectors.
- Chain order and bypass expectations.

3. Add endpoint integration checks
- Registry keys and FxProperties writes.
- audiodg callback visibility through logs and mapping probe.

4. Add release sanity checkpoints
- One short human listening pass per release candidate.

## 3. Unattended validation pipeline

1. Configure and build using presets.
2. Build TeeDspConfig every run.
3. Attempt TeeDspApo build when APO base library is available.
4. Optionally run install/uninstall scripts in elevated context.
5. Run contract checks (registry, services, endpoint binding, optional mapping probe).
6. Emit pass/fail summary with actionable next step.

Primary entrypoint: scripts/validate_stack.ps1.
Contract checker: scripts/verify_apo_contract.ps1.

## 4. Human interaction policy

Human interaction is required only for:
1. UAC approval for endpoint mutation.
2. Optional listening sanity check.

Everything else should run unattended via scripted validation.

## 5. Acceptance criteria

1. Build gate passes on clean workspace.
2. Install/uninstall lifecycle leaves services healthy.
3. APO contract checks pass for shared memory and callback traces.
4. DSP controls update live without audio interruption.
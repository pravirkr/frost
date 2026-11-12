# Working on frost

Read `docs/architecture.md` before changing code. It is the frozen layout and
the rules every change must follow (public/private boundaries, includes,
plans, numerics, memory, configuration, tests). Its "Code" section covers
adding a pipeline stage or a backend implementation. Before changing a
design choice or doing performance work, read `docs/decisions.md`: those
choices are settled, so raise a change first and record it there.

Before finishing a change:

- `scripts/check_architecture.sh` passes;
- `pre-commit run --files <changed files>` passes;
- `cmake --preset cpu && cmake --build --preset cpu &&
  ctest --preset cpu -j 8` passes. The `cpu` preset builds into `build/`
  with the GPU off; use the `release` preset to also build CUDA or HIP when
  a toolkit is found. Run the tests on a Release build, not a Debug one
  (`cpu-debug`, `ci-tidy`): they are far too slow there.
- `docs/roadmap.md` is updated when planned work or known limitations
  change.
- A change users can see (an option, a default, the `.cand` format,
  installation) updates the user documentation in `docs/` and
  `CHANGELOG.md`, and `sphinx-build -W -b html docs docs/_build/html`
  passes (packages in `docs/requirements.txt`; see the "Documentation"
  section of `docs/architecture.md`).

Work on a branch; commit, push or open pull requests only when asked.

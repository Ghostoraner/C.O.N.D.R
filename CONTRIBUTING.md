# Contributing

Thanks for your interest in C.O.N.D.R. Contributions are welcome, especially
focused fixes, documentation improvements and reproducible bug reports.

## Before opening a pull request

- Keep changes focused and explain the problem they solve.
- Build the kernel with `make kernel`.
- For changes affecting boot or runtime behavior, test with `make run` when the
  required QEMU and ISO tools are available. The console `check` command runs
  the NVMFS self-test.
- Describe what you tested and note anything you could not test.
- Do not include generated files from `build/` or the downloaded `limine/`
  directory.

There is no guarantee that every contribution will be accepted. This is an
experimental hobby operating system and its interfaces may change.
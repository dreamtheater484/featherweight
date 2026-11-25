# Contributing

Thanks for helping. Featherweight stays small and fast on purpose, so the bar for new features is high and the bar
for fixes is low.

## Report a problem

- **A bug:** [open an issue](https://github.com/dreamtheater484/featherweight/issues/new). In Featherweight, ⋯ →
  **Help** → **Report a problem** fills in the versions. A file that shows the problem helps most, if it isn't private.
- **A crash:** the next start offers to report it, with the crash report to attach.
- **A vulnerability:** report it privately, as [SECURITY.md](SECURITY.md) says. Not in a public issue.
- **An idea:** open an issue first, before writing code, so we can agree it fits.

## Change something

1. Fork the repository and make a branch.
2. Build and test as the README says (`cmake`, then `ctest`). Every test must pass.
3. Open a pull request against `main` that says what changes and why. CI builds it, runs the tests, the torture
   tests, the fuzzers for a minute each and CodeQL.

What a change needs:
- **Tests:** new behaviour comes with tests in `tests/unit` (or the corpus and torture tests for file formats). A fix
  comes with a test that fails without it.
- **Never damage a file:** a change to reading or saving keeps the parts it doesn't edit exactly as they were, and
  passes the torture tests.
- **Lightweight:** nothing new loads at launch; a size increase gets a line with its reason in
  `tests/budgets/ledger.json` (`IMPLEMENTATION_PLAN.md` §11).
- **No new network access** and no new third-party library without agreeing it in an issue first.
- **Style:** the existing code's style: C++20, two-space indents, lines of about 120 characters, comments that say
  why.
  The compiler's warnings (`/W4`) stay at zero.
- **Strings** the user sees go through the translation catalog (`T()`).

## Licence

Featherweight is MIT-licensed. By contributing, you agree that your contribution is released under the same licence.
The name "Featherweight" and its logo aren't covered by the licence.

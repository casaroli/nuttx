# AGENTS.md — working on Apache NuttX

Rules for AI agents contributing to this repository. Read
[CONTRIBUTING.md](CONTRIBUTING.md) and [INVIOLABLES.md](INVIOLABLES.md) first —
this file summarizes what matters most for agents and adds a few local rules.

## Code

- Follow the [NuttX C Coding Standard](https://nuttx.apache.org/docs/latest/contributing/coding_style.html).
  Nothing enters NuttX that does not follow it, and expediency is not a
  justification for breaking it.
- Essentials: 2-space indent, no TABs, lines ≤ 78 columns, braces on their own
  line, two spaces after `.` and `:` in comments.
- Verify before committing:
  - `./tools/checkpatch.sh -g HEAD~1..HEAD`
  - `./tools/checkpatch.sh -f path/to/file.c`
- Fix style issues in every file you touch, even pre-existing ones.
- Prefer non-invasive, self-compatible changes. Breaking changes are not
  welcome and need prior community discussion and a mailing list vote.
- POSIX compliance and the modular architecture are inviolable. Keep the code
  portable across all supported hosts, toolchains, and architectures.

## Comments and docstrings

- Keep function header comments and inline comments **as short as possible**.
  State what the function does, its parameters, and its return value — nothing
  more.
- The detailed rationale ("why", background, alternatives considered) belongs in
  the commit message, not in the source. Do not duplicate it in both places.

## Commits

Format (see CONTRIBUTING.md 1.5 and 2.2):

```
functional/area: Short self-explanatory topic.

What is changed, how, and why. Several short sentences or bullet
points. Include impact and how it was tested.

Assisted-by: AGENT_NAME:MODEL_VERSION [TOOL1] [TOOL2]
```

- The topic prefix is the functional area (`net/can:`, `arch/arm:`,
  `boards/sim:`, `Documentation:`), followed by a capitalized, self-explanatory
  summary ending with `.`.
- **`Assisted-by:`** is mandatory for any commit an agent helped produce, where
  `AGENT_NAME` is the tool or framework, `MODEL_VERSION` is the model used, and
  the optional `[TOOL1] [TOOL2]` are specialized analysis tools.
  Do **not** use `Co-authored-by` — NuttX uses `Assisted-by` only.
- **Never add `Signed-off-by`** on behalf of the user. Only a human can legally
  certify a commit.
- **Never include a link to the AI session**, and never include any link that is
  not publicly accessible (internal dashboards, private repos, local paths).
- Every commit must build on its own. Split code and documentation into separate
  commits in the same PR where practical.

## Sign-off — always ask the user

After creating a commit or a batch of commits, stop and ask the user to review
and sign off manually. Provide the command:

Last commit:

```sh
git commit --amend -s --no-edit
```

A batch of commits (all commits after `<base>`, e.g. `origin/master`):

```sh
git rebase --exec 'git commit --amend -s --no-edit' <base>
```

Verify with `git log --format='%an %ae%n%b' <base>..HEAD`.

## Pull requests

- **Do not create PRs automatically.** Ask the user first.
- When asked to create one, **always create it as a draft**:
  `gh pr create --draft`.
- The PR description must follow the template in CONTRIBUTING.md 2.3, with all
  sections filled in: **Summary**, **Impact**, **Testing**, and the
  **PR verification Self-Check** boxes.
- The PR title is the first commit's topic line.
- Write PR descriptions to make the reviewer's life easy: concise but complete.
  Say what changed, why, and what the impact is. Cut filler, restated diffs,
  and anything the reviewer does not need to judge the change.
- **Never post comments on PRs or issues** — neither your own nor on behalf of
  the user. Draft the wording, show it to the user, and let them post it
  manually.
- Same link rules as commits: no AI session links, no non-public links.
- One functional change per PR. Documentation updates for new or changed
  functionality go in the same PR.
- Breaking changes: `!` as the first character of the title, plus a
  `BREAKING CHANGE:` block with quick-fix instructions in the body.

## Testing

Build and runtime logs from **at least one real hardware target** are mandatory
for code changes (more than one architecture for breaking changes; QEMU does not
count there). Agents cannot produce these — ask the user to run the tests and
supply the logs, and never claim testing that was not performed.

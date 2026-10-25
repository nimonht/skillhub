# Benchmark baseline: Skill Hub 1.0.1, and the 2.0 engine

`python3 bench/bench.py BINARY --repos N --skills 20` on 2026-10-25. Machine: Intel Core i7-7700HQ (4 cores, 8 threads), 16 GB,
CachyOS, git 2.56, local repositories over `file://` (no network). Every skill is also linked for Claude Code.

| Skills | Install | Check (nothing changed) | Check (10 % changed) | Update all | Peak RSS | git processes (install / check / update) |
|---:|---:|---:|---:|---:|---:|---|
| 100 | 1.95 s | 0.76 s | 0.78 s | 0.89 s | 63 MB | 410 / 210 / 230 |
| 500 | 16.11 s | 3.57 s | 3.64 s | 5.52 s | 65 MB | 2,050 / 1,050 / 1,150 |
| 1,000 | 48.29 s | 7.15 s | 7.30 s | 14.25 s | 69 MB | 4,100 / 2,100 / 2,300 |

What the numbers show:

- **Install is quadratic.** Every skill rewrites the lock file and relinks everything, rescanning every installed skill.
  From these three points, `t ≈ 0.016 n + 0.000032 n²` seconds. That puts 10,000 skills near **56 minutes**, which is why
  1.0.1 is not run at 10,000.
- **A check costs two git processes per skill plus two per repository, even when nothing changed.** That is linear: about
  7 ms per skill here, so about 70 s for 10,000 skills on local disks. Against GitHub each repository adds two network
  round trips (about 0.9 s each), one after another: about 15 minutes for 500 repositories.
- **Update all** pays the check plus a full relink per updated skill: quadratic again, about 13 minutes for 10,000 skills
  with 10 % changed (extrapolated).
- Memory is flat at 63–69 MB, almost all of it the Qt Quick libraries that the single binary maps even for command-line
  runs (`skillhub --version` alone peaks at 65 MB).

## Targets for 2.0

Same machine, same fixture, 10,000 skills (500 repositories × 20):

| Phase | 1.0.1 (extrapolated) | 2.0 target |
|---|---:|---:|
| Install all | ~56 min | under 60 s |
| Check, nothing changed | ~70 s | under 2 s |
| Update all, 10 % changed | ~13 min | under 60 s |
| Backend peak RSS | 69 MB at 1,000 | under 100 MB at 10,000 |
| `skillhub list`, 137 skills | 65 MB | under 8 MB |
| Window: longest UI-thread stall during a 10,000-skill update | n/a | 20 ms or less |

## 2.0 engine (milestone M1b)

`python3 bench/bench.py build/skillhub --repos N --skills 20 --install-mode all`, same machine and fixture. Install is one
`skillhub add` with every repository. The engine handles each repository as one task on 16 worker threads, with at most
8 clones or fetches per host at a time.

| Skills | Install | Check (nothing changed) | Check (10 % changed) | Update all | Peak RSS | git processes (install / check / update) |
|---:|---:|---:|---:|---:|---:|---|
| 100 | 0.05 s | 0.02 s | 0.04 s | 0.03 s | 3 MB | 25 / 15 / 20 |
| 500 | 0.22 s | 0.08 s | 0.20 s | 0.13 s | 5 MB | 125 / 75 / 100 |
| 1,000 | 0.44 s | 0.18 s | 0.39 s | 0.24 s | 7 MB | 250 / 150 / 200 |
| 10,000 | 5.30 s | 1.61 s | 3.94 s | 2.48 s | 52 MB | 2,500 / 1,500 / 2,000 |

Against the targets at 10,000 skills: install 5.3 s (under 60 s), check 1.6 s (under 2 s), update 2.5 s (under 60 s),
peak 52 MB (under 100 MB). `skillhub list` on the real 137-skill library peaks at 3 MB.

Where the time goes now:

- **git processes per repository, not per skill.** A check with nothing changed runs three per repository: `rev-parse`
  for the clone's own HEAD, `ls-remote` to compare with upstream, `ls-tree` for every folder's tree id at once. A changed
  repository adds `fetch` and `update-ref`. Install and update add one `cat-file --batch` that streams every needed folder
  to disk.
- **The lock file is written once per 200 entries**, through a lookup table instead of a walk through every entry.
  Before that table, `list` took 1.3 s at 10,000 skills and `install` 8.7 s.
- **Of the 1.6 s check, about 0.35 s is reading the shared folder** (every SKILL.md and every link), as `list` does.

## The window's backend (milestone M1c)

`python3 bench/bench_serve.py build/skillhub --repos 500 --skills 20`, same machine and fixture: every skill installed
first with one `skillhub add`, then `skillhub serve --stdio` driven over its pipes the way the window drives it.

| 10,000 skills | Time | Data to the window |
|---|---:|---:|
| First snapshot, 10 pages of 1,000 | 0.49 s | 4.7 MB |
| `skills.check`, nothing changed upstream | 1.07 s | delta 4.7 MB: every row goes from tracked to current |
| `skills.check`, 10 % changed | 2.72 s | delta 0.5 MB |
| `skills.update`, the 1,000 available | 1.69 s | delta 0.5 MB |

Backend peak RSS over the whole run: 69 MB (target: under 100 MB). The first check after start is the largest delta the
window ever applies; the window's 20 ms stall budget (M2) is measured against that one.

A small run against real GitHub repositories and a Windows run are still to be recorded.

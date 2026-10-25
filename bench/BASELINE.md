# Benchmark baseline: Skill Hub 1.0.1

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

A small run against real GitHub repositories and a Windows run are recorded in M1, once the new engine exists.

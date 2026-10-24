---
name: Skill Hub
description: A native console for one shared agent-skills folder: every coding agent found on the machine is a column, every skill has a port in it.
colors:
  dark-window: "#0B0E13"
  dark-rail: "#10141A"
  dark-surface: "#141920"
  dark-raised: "#1B212A"
  dark-selected: "#222A35"
  dark-text: "#E8ECF1"
  dark-text-muted: "#A5B0BE"
  dark-text-quiet: "#8995A4"
  dark-mint: "#34E0A6"
  dark-amber: "#FFB224"
  dark-lime: "#C8F23C"
  dark-red: "#FF6A5E"
  light-window: "#DDE1E6"
  light-rail: "#E8EBEE"
  light-surface: "#F5F6F7"
  light-raised: "#FFFFFF"
  light-selected: "#DCEFE8"
  light-text: "#11161C"
  light-text-muted: "#465160"
  light-text-quiet: "#5E6977"
  light-mint: "#0A8F69"
  light-mint-text: "#07765A"
  light-amber-text: "#8A5200"
  light-lime-text: "#466100"
  light-red-text: "#B3241B"
typography:
  display:
    fontFamily: "Barlow Condensed, sans-serif"
    fontSize: "26px"
    fontWeight: 700
    lineHeight: 1.1
    fontFeature: "tnum"
  title:
    fontFamily: "Barlow Condensed, sans-serif"
    fontSize: "22px"
    fontWeight: 700
  body:
    fontFamily: "Barlow Semi Condensed, sans-serif"
    fontSize: "14px"
    fontWeight: 500
    lineHeight: 1.22
  small:
    fontFamily: "Barlow Semi Condensed, sans-serif"
    fontSize: "13px"
    fontWeight: 400
  label:
    fontFamily: "Barlow Semi Condensed, sans-serif"
    fontSize: "12px"
    fontWeight: 600
    letterSpacing: "0.05em"
rounded:
  sm: "4px"
  md: "6px"
  cut: "8px"
spacing:
  xs: "4px"
  sm: "8px"
  md: "12px"
  lg: "16px"
  xl: "24px"
  xxl: "32px"
components:
  button-primary:
    backgroundColor: "{colors.dark-mint}"
    textColor: "#04251A"
    rounded: "{rounded.cut}"
    height: "36px"
    padding: "0 15px"
  button-secondary:
    backgroundColor: "{colors.dark-raised}"
    textColor: "{colors.dark-text}"
    rounded: "{rounded.cut}"
    height: "36px"
  status-tag:
    backgroundColor: "{colors.dark-raised}"
    textColor: "{colors.dark-text-muted}"
    rounded: "{rounded.sm}"
    height: "24px"
  skill-row:
    backgroundColor: "{colors.dark-surface}"
    textColor: "{colors.dark-text}"
    height: "56px"
---

# Design System: Skill Hub

## Overview

**Creative North Star: "The Agent Columns."** The shared skills folder is a block on the left; every coding agent Skill Hub finds on the machine is one column above the list, and every skill has one port in that column. A column says who the agent is (its own single-colour brand mark on a flat tile, tinted by the theme and never in brand colours; a two-letter monogram when no mark exists), how many skills it can see ("69 / 69") and how much of the folder that is (a thin ratio bar). The columns are detected, not fixed: install a new agent and a column appears; with more than six, the rest fold into a "+N more" column.

This is a working tool first. The visual language is **inspired by Arknights: Endfield**: smoked-charcoal and paper-white materials, chamfered cuts on action shapes, circles for instruments, selection by corner hooks instead of glow, and a texture budget of one faint dot grid. It borrows type, palette, density and one signature move; the layout, navigation and controls stay the ones a Linux developer already knows (side rail, search, table, detail pane, activity log). No game assets, logos, art or fonts are used.

## Colors

Colour has jobs, never decoration. Every state also carries an icon or a word.

- **Mint (selection, lit, primary).** The brand colour. `dark-mint` fills the primary action and lights healthy wires and ports; on light surfaces the action is ink and mint appears as `light-mint` marks and `light-mint-text`.
- **Amber (needs action).** An update is available or a link needs repair. Text uses `light-amber-text` on paper.
- **Lime (just happened).** A skill was updated in this session; it returns to neutral on the next check.
- **Red (broken).** A missing link, a failed update, destructive actions.
- **Neutrals.** Window < rail/console < list surface < raised < selected, separated by tone steps and 1px quiet lines. Dark is smoked charcoal, light is paper over concrete; light is tuned on its own, not inverted.

All text pairs are checked at 4.5:1 and graphics and control boundaries at 3:1 by `tests/contrast.py`. Cyan, blue and purple are deliberately unused.

## Typography

Barlow Semi Condensed carries the interface; Barlow Condensed (Bold) carries numbers and titles. Both are SIL OFL and bundled. A fixed scale (12 / 13 / 14 / 16 / 20 / 26 / 34 px) with tabular numerals wherever digits align. Never below 12px; micro labels are sentence-weight semibold with slight tracking and appear only as column or section headings, not as kickers above headings.

## Layout

Rail (216) | centre | detail (380) under a 64px top bar and a 34px status bar, with an optional activity feed above the status bar. The centre is the agent band (128) over a sortable column header (36) over the list (56px rows). The agent columns (56-72px each, narrower as there are more of them, at most six) are shared by the band and the rows so ports line up exactly. Below 1140px the detail pane becomes an overlay with a close button; the minimum window is 820x560. Top-bar actions drop to icons (with hover names) below 1000px. The rail scrolls on short windows and keeps the appearance block pinned when there is room.

## Elevation & Depth

Flat. Depth is tone: each surface is one step lighter (dark) or whiter (light) than the one below, and controls are tone fills with no outline. The only floating things are the toast and tooltips; they use an inverted surface (`inkBg`) so they need neither border nor shadow. Keyboard focus is the one place an outline appears: a 2px ring in `focus`, drawn outside the shape so nothing clips it.

## Shapes

- **Chamfer (8px cut, top-right and bottom-left):** action buttons and structure that the user operates. The hub block cuts top-left and bottom-right.
- **Circle:** instruments: the ports in the list rows.
- **Plain rectangle (4px radius):** everything else: agent tiles (6px), search and form fields, the switch and segmented control, toasts, the SKILL.md block, list rows. No capsules, no pill badges: status is an icon and a word, selection is a tone step plus corner hooks.
- **Corner hooks:** the one selection mark (list row, rail filter, inspected agent column).

## Components

- **Buttons:** primary (mint on dark / ink on light), secondary, danger (outline), ghost. Icon-only variants name themselves on hover. Disabled keeps its shape and loses emphasis.
- **Status:** an icon and a word, with no badge or outline around it: Update, Current, Updated, Error, Local, Not checked; for agents Healthy, Needs repair, Not found.
- **Port:** a plain lit disc when the skill is visible; a ring appears only where something needs a look: cross (missing), diamond (separate copy), dash (link elsewhere or agent absent).
- **Agent column:** a 36px tile with the agent's brand mark (monogram when it has none), the short name, `visible / total` in condensed numerals and a 3px ratio bar (mint for the visible share, amber when something needs attention). The inspected column gets corner hooks. The sixth column folds the agents beyond five into "+N more".
- **Agents manager:** every known agent (75 in the catalog) with how it was found and how it sees skills; one switch to manage or ignore it; your own agents can be added.
- **Transfer:** export writes one file; import shows what is inside before installing; dropping a file on the window starts it.
- **Loaders:** one spinner (a turning three-quarter arc) for work of unknown length, and a 2px line along the top edge of the status bar when progress is known. No sliding or pulsing bars.
- **Detail pane:** actions first, then who can see the skill, source facts, then the rendered SKILL.md (selectable).
- **Add sheet:** inline, not a modal: repository field, skeleton while reading, checkable skills, one install button.
- **Activity feed:** real log lines from the same code the CLI uses.

## Do's and Don'ts

Do: use real data only (counts, hashes, paths); show state with an icon and a word as well as colour; keep every action keyboard reachable; animate state changes only (120-220ms) and honour reduced motion.

Don't: add sliding or pulsing progress bars; outline controls or wrap status in pills (tone steps and plain text do that job); put glows, gradients or shadows on things at rest; add a kicker label above a heading; number sections; fake telemetry or metrics; use game assets, logos or fonts; turn the whole screen into a mood board of chamfers: shapes mean something or they are not used.

## Credits

Visual language inspired by *Arknights: Endfield* via the MIT-licensed `endfield-design` skill (Statrue/endfield-design-skill); no official material is included. Barlow by The Barlow Project Authors, SIL OFL 1.1 (`fonts/OFL.txt`).

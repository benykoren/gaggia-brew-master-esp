# OEM manuals — Gaggia New Espresso 06 / Pure & Color

Original manufacturer documents for this project's target machine (Gaggia
Espresso Color; per [`AGENTS.md`](../../AGENTS.md) Section 1, "New Espresso 06 /
Pure & Color" is the same model family). Added 2026-08-30 — see the
[`AGENTS.md`](../../AGENTS.md) Section 10 change log entry for that date for
what these resolved.

**Read `AGENTS.md` first, not this file** — it's the project's authoritative
doc. This directory is source material; the facts extracted from it are
already folded into `AGENTS.md` Sections 2/4/7 and `HARDWARE_ROADMAP.md`.
Come here only when you need the original diagram/page itself.

| File | Drawing/doc # | What it is |
|------|----------------|------------|
| [`electrical-schematic-SAE0486.pdf`](electrical-schematic-SAE0486.pdf) | SAE0486 (25/11/06) | Official schematic + topographic wiring diagram, 120V/230V. Shows the on/off switch, brew switch/thermostat, steam switch/thermostat, ready/power lamps, and pump on one page. |
| [`hydraulic-schematic-SAI0103.pdf`](hydraulic-schematic-SAI0103.pdf) | SAI0103 (14/02/2007) | Official water-circuit diagram: tank → pump → 3-way fitting → boiler, safety-valve discharge line, steam tap/wand line, with the exact silicone tubing part numbers/sizes for each run. |
| [`parts-catalog-ER0270.pdf`](parts-catalog-ER0270.pdf) | ER0270 Rev.00 (03-08-07) | Official exploded-view parts catalog (bodywork + boiler assemblies) with part numbers, including thermostat/heater/pump model numbers and ratings. |
| [`user-manual.pdf`](user-manual.pdf) | — (2007, multi-language) | Official end-user instruction manual. |

## Key facts extracted (already in AGENTS.md, repeated here for quick lookup)

From the parts catalog (TAV. 2 — CALDAIA):
- **Brew thermostat ("Termostato Caffè"):** 95°C, Gaggia P/N `12000160`, model `US-622AXTDNO`.
- **Steam thermostat ("Termostato Vapore"):** 127°C, Gaggia P/N `12000161`, same `US-622AXTDNO` family.
- **Heating element:** stainless boiler, 1000W — `230V` (P/N `11001002`) or `120V` (P/N `11004509`) variant.
- **Pump:** ULKA `EP5/S` (230V-50Hz, P/N `12000140`) or `EAP5/S` (120V-60Hz, P/N `12000142`).

From the hydraulic schematic (SAI0103):
- **Safety valve discharge rating: 16 bar** ("SCARICO VALVOLA DI SICUREZZA 16 bar").

From the electrical schematic (SAE0486) — legend only, full topology not yet
transcribed into prose (read the PDF directly if you need the actual circuit
paths):
1. Pompa (pump) · 2. Spina autobloccante (self-locking connector) ·
3. Interruttore ON/OFF · 4. Termostato caffè · 5. Resistenza (heater) ·
6. Termostato vapore · 7. Interruttore vapore · 8. Spia pronto macchina
(ready lamp) · 9. Spia ON/OFF (power lamp) · 10. Interruttore caffè (brew
switch, feeds the pump).

## What this does and doesn't settle

- **Settles** the previously-"unknown" steam-thermostat trip point
  (AGENTS.md Section 4) at 127°C, and the brew thermostat at 95°C — see the
  Section 4/7 updates for what that means for `STEAM_MAX_SAFETY`.
- **Does NOT retroactively confirm** the specific *this-unit's* panel wiring
  described secondhand in AGENTS.md Section 7's "historical investigation"
  subsection (LED colors, which wire goes to the pump vs. mains, etc.) — this
  is the factory reference schematic for the model line, not a photo of the
  actual unit's current wiring, which may have been serviced/modified. The
  project's mains-wiring plan deliberately doesn't depend on that panel
  wiring anyway (clean-bypass design), so this is informational, not a
  reason to revisit that decision on its own.

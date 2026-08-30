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

## Electrical circuit — transcription (SAE0486)

> **Read this before touching anything mains-voltage.** This is an AI's
> visual reading of a hand/CAD-drawn schematic, cross-checked against the
> parts catalog and basic component physics (see **Validation notes**
> below) and transcribed into text so an agent without PDF access can still
> work from it — it is **not a substitute for the original drawing**.
> Confidence varies by branch (marked below). This project's actual mains
> build (`AGENTS.md` Section 7) is a from-scratch clean-bypass circuit that
> does **not** depend on any of this legacy panel topology being correct —
> nothing here should be used to justify reviving the old panel wiring
> without first re-verifying against the PDF
> (`electrical-schematic-SAE0486.pdf`) or the physical machine.

**Legend** (numbering is the schematic's own — the *user manual*'s separate
FIG.01 numbers the same kind of parts differently, don't cross-reference the
two by number):

| # | Component | Original Italian label |
|---|-----------|-------------------------|
| 1 | Pump | Pompa |
| 2 | Mains power-cord inlet socket (3-pole) | Spina autobloccante |
| 3 | Main ON/OFF switch | Interruttore ON/OFF |
| 4 | Brew thermostat, 95°C | Termostato caffè |
| 5 | Heating element | Resistenza |
| 6 | Steam thermostat, 127°C | Termostato vapore |
| 7 | Steam button | Interruttore vapore |
| 8 | "Ready" lamp | Spia pronto macchina |
| 9 | Power-on lamp | ON/OFF spia |
| 10 | Brew button | Interruttore caffè |

**Circuit** (L and N are the incoming mains rails, sourced through inlet
socket 2 — confirmed at high zoom: it's a physical 3-pin connector labeled
**L / Earth / N**, fed directly by the incoming power cord):

```mermaid
flowchart TD
    L(("L — mains live"))
    N(("N — mains neutral"))

    L --> SW3["Main ON/OFF switch"]
    SW3 --> N

    L --> LMP9["Power-on lamp"]
    LMP9 --> N

    L --> T4["Brew thermostat, 95°C"]
    L --> LMP8["Ready lamp"]
    L -. "bypasses brew thermostat<br/>when pressed" .-> SW7["Steam button"]
    T4 --> J((" "))
    LMP8 --> J
    SW7 -.-> J
    J --> T6["Steam thermostat, 127°C"]
    T6 --> R5["Heating element, 1000W"]
    R5 --> N

    L --> SW10["Brew button"]
    SW10 --> PUMP1["Pump"]
    PUMP1 --> N
```

**Walkthrough:**
1. **Main switch and power lamp are two independent branches, not one
   series pair (medium confidence — flagged as odd, see Validation
   notes):** zoomed in expecting to find the switch and lamp in series
   (the obvious design), but the drawing clearly shows two separate wires
   off L, each running straight to N with nothing else in the branch —
   switch alone, lamp alone. Wired as literally drawn, the switch branch
   has no load in series with it, which is electrically strange for a
   "master switch." Possible explanations: it's a reference/test switch
   not meant to be read as a master power switch (the legend's OCR gave a
   garbled "INTERRUTTORE RFIT ON/OFF" — possibly "RIF." /reference/, which
   would fit), or there's a mechanical/multi-pole detail this single-line
   schematic doesn't capture. Left unresolved rather than guessed at.
2. **Heating branch (high confidence, refined from the previous pass):**
   the brew thermostat *and* the ready lamp **both tap L independently**
   and rejoin at the same junction, which then continues through the steam
   thermostat and heating element to N — the lamp runs in parallel with
   the thermostat itself (not "after" it, as the previous version of this
   section had it). Electrically this means: while the brew thermostat is
   closed (actively heating, below 95°C) the junction sits at L on both
   sides of the lamp, so it stays **dark**; once the thermostat opens
   (95°C reached) the junction can only be reached through the lamp's
   high-resistance path, so nearly all the voltage drops across the lamp
   and it **lights** — i.e. "lit = ready," the more intuitive reading,
   corrected from the previous version's "lit while heating" guess. The
   steam button taps L directly into the same junction too — now clearly
   visible as its own dot in the high-resolution render — confirming the
   bypass topology at high confidence (upgraded from the previous pass's
   "verify against the PDF" hedge): pressing it holds the junction at L
   regardless of the brew thermostat, letting heating continue toward the
   steam thermostat's 127°C cutoff.
3. **Pump branch (high confidence, unchanged):** brew button → pump → N.
   Matches both the topographic half of this same drawing (a "blue" wire
   run to the pump) and the user manual's own description of its brew/
   hot-water button driving the pump.

### Validation notes

**2026-08-30, second pass (high-resolution re-check):** rendered the PDF at
6x (~430 DPI) with PyMuPDF and cropped into each junction individually,
the same technique that caught the hydraulic-diagram errors. This refined
(rather than overturned) the electrical reading:
- **Ready lamp actually taps L directly, in parallel with the brew
  thermostat itself** — not "downstream of it" as the first pass had it.
  Both feed the same junction, which then continues through the steam
  thermostat and heater to N. Re-derived the lamp's behavior from this
  corrected topology: **lit = ready** (thermostat open), dark while
  actively heating — the opposite of the first pass's guess, and the more
  intuitive reading besides.
- **The steam button's bypass wiring — previously flagged as unverifiable
  — is now confirmed at high confidence.** The high-resolution render
  shows the steam button's wire clearly joining the same junction as the
  brew thermostat and ready lamp, settling what the first pass could only
  guess at functionally.
- **Confirmed component 2 with certainty**, closing out the first
  correction: it's a physical 3-pin connector labeled **L / Earth / N** in
  the topographic diagram, wired directly to the incoming power-cord
  drawing right next to it. No remaining doubt this is the mains inlet
  socket, not a pump-specific part.
- **New open question found, not resolved:** the main ON/OFF switch and
  the power-on lamp are drawn as **two independent branches**, each wired
  straight across L→N with nothing else in series — not the series pair
  the first pass assumed. Read literally, the switch branch has no load,
  which is electrically odd for a "master switch." Documented as
  unresolved in the walkthrough above rather than guessed at — this is
  exactly the kind of thing worth a human checking against the physical
  switch/lamp assembly rather than trusting either AI reading.

**2026-08-30, first pass:** two corrections found on the initial re-check,
done because this is mains-voltage reference material:
- **Inlet socket, not a pump connector.** Originally placed component 2 in
  series with the pump's supply wire, based only on its position near the
  bottom of the drawing. Cross-checking the parts catalog
  (`parts-catalog-ER0270.pdf`, TAV.2 item 47: "SPINA AUTOBLOC.TRIPOL." —
  "self-locking **three-pole** plug") showed it's actually the machine's
  mains power-cord inlet (3 poles = live/neutral/earth; a pump lead would
  only need 2) — now directly confirmed by the second pass above.
  Corrected — it no longer appears in the pump branch.
- **Ready lamp is a parallel tap, not a series element.** Originally drawn
  in series between the two thermostats and the heating element — not
  possible, a neon pilot lamp's series resistor limits current to a couple
  of mA, nowhere near a 1000W element's load. Corrected to a parallel tap
  (position refined further by the second pass above).

## Hydraulic circuit — transcription (SAI0103)

> **This section was rewritten 2026-08-30 after the user flagged the first
> version as wrong** ("from the tank it's 2 water outputs"). That flag was
> correct — the first pass missed a real tank outlet and mis-traced several
> connections. This version was rebuilt by rendering the PDF at high
> resolution (6x, ~430 DPI) and cropping into each junction individually
> rather than reading the page at normal size — see **Validation notes**
> below for exactly what changed and what's still not 100% certain.

**Tubing legend** (every tube in the diagram, with its real size — this is
what was missing before):

| Tag | Part No. | Bore × OD | Length | Material | Qty |
|-----|----------|-----------|--------|----------|-----|
| 1 | AG3170 | 5×9mm | 120mm | Silicone | 2 (both instances shown below) |
| 2 | 11001207 | 4.2×8.2mm | 150mm | Reinforced silicone, pink | 1 |
| 3 | 11001206 | 4.2×8.2mm | 200mm | Reinforced silicone, pink | 1 |
| 4 | DM1206/015 | 5×9mm | 70mm | Silicone | 1 |
| 5 | PA1062 | 5×9mm | 350mm | Silicone ("ASPI" — see note) | 1 |

**Circuit:**

```mermaid
flowchart TD
    TANKA["Tank — outlet A"] -- "tube 1 (120mm)" --> PUMPIN["Pump suction inlet"]
    TANKB["Tank — outlet B"] -- "tube (unlabeled run)" --> TWAY["3-way tee (passive)"]
    SAFETY["Boiler safety valve<br/>discharge (16 bar)"] -- "tube 4 (70mm)" --> TWAY
    TWAY -- "tube 1 (120mm, 2nd)" --> PUMPIN
    PUMPIN --> PUMP["Pump"]
    PUMP -- "discharge, tube 2 (150mm)" --> BOILER["Boiler — water inlet"]
    RVAP["Rubinetto vapore<br/>(steam valve, on boiler)"] -- "tube 3 (200mm) — steam" --> WAND["Steam wand / Pannarello"]
    RVAP -- "tube 5 (350mm) — air intake, probable" --> WAND
```

**Walkthrough:**
1. **The tank has two separate bottom outlets** (confirmed by zooming into
   the drawing — both are visible as distinct stub fittings on the tank's
   bottom edge, not one outlet drawn twice):
   - **Outlet A** feeds the pump's suction inlet **directly** through a
     120mm tube (tag 1) — short, unambiguous, a clean single connection.
   - **Outlet B** runs down and over to the **3-way tee** ("Raccordo 3
     vie"), which also receives the boiler's 16-bar safety valve's
     discharge (tag 4, 70mm) on a second port. The tee's third port feeds
     back **up to the same pump suction inlet** through a second 120mm
     tube (also tag 1 — the parts table's qty=2 is exactly these two
     uses). So the pump's suction is fed by outlet A directly *and*
     outlet B indirectly (merged with any safety-valve discharge) at the
     same inlet point.
2. **The pump's discharge (pressurized) side** feeds up toward the boiler's
   own water inlet — this is the pump actually doing its job (drawing from
   the tank, pushing to the boiler), which was missing from the first
   version entirely.
3. **The steam side is a separate, distinct sub-circuit**, not fed by the
   tank/pump path at all: the boiler's own steam valve ("Rubinetto vapore")
   feeds the steam wand/Pannarello through tube 3 (200mm). A second tube
   (tag 5, 350mm) runs alongside it to the same wand assembly — given it's
   explicitly labeled "ASPI" (**aspirazione**) in the parts table, and the
   wand is a Pannarello-style frother (parts catalog item 45), this is most
   likely the **frother's air-intake tube** (Pannarellos froth by drawing
   in air via venturi effect alongside the steam, not by carrying water) —
   not a water-suction line at all. This reading fits the tube's label and
   destination well, but isn't independently confirmable from the drawing
   alone.

### Validation notes (2026-08-30 rewrite)

The **first version of this section was substantially wrong** — logged
here rather than silently replaced, since the corrections change the whole
shape of the diagram, not just one detail:
- **Missed that the tank has two outlets entirely.** The first pass only
  traced one path out of the tank (to the pump) and treated a second,
  separate line as if it were a continuation of the same run toward the
  boiler. Re-cropping the tank's bottom edge at high zoom showed two
  distinct stub fittings, not one.
- **Never identified the pump's discharge path at all** — the first
  version went straight from "pump" to "3-way tee" as if the tee sat on
  the pump's output side. It's actually on the **suction** side (merging
  tank outlet B with the safety valve's discharge back into the pump's
  inlet); the pump's actual discharge runs a completely different path up
  toward the boiler, which the first version never traced.
- **Mislabeled the 3-way tee as sitting between the pump and the boiler.**
  It doesn't — per the above, it's a suction-side junction, not a
  discharge-side one.
- **Never previously listed tube sizes at all** — the "Tubing legend"
  table above (added at the user's request) didn't exist in the first
  version.
- **Still a reasoned inference, not a certainty:** tube 5's exact function
  (air-intake for the Pannarello). It's well-supported by the "ASPI" label
  and the Pannarello destination, but this drawing alone doesn't spell out
  "aria" (air) anywhere, so treat it as the best available reading, not a
  confirmed fact.

## Parts catalog — full listing (ER0270)

Transcribed directly from the catalog's own English column (it's bilingual
in the source; Italian dropped here since the source already gives English
descriptions). Positions match the numbered exploded-view diagrams in
`parts-catalog-ER0270.pdf` — open that PDF if you need to see where a
position number actually points on the physical part.

### TAV. 1 — Bodywork assembly

| Pos | Part No. | Description |
|-----|----------|--------------|
| 01 | 4337026000 | Upper inside package |
| 02 | 4337027000 | Upper external package |
| 03 | 4337028000 | Package box |
| 04 | 4337031000 | Instructions booklet |
| 05 | WGA2PR/1 | Separator |
| 06 | NF08/002 | 1-cup filter, 5.5/6.5g |
| 07 | NF08/005 | 2-cup filter, 12/14g |
| 08 | 11005535 | Filter for pod |
| 09 | 11007038 | "Perfect crema" filter, Ø0.6mm |
| 10 | WGADM1017 | Measuring scoop |
| 11 | 4337004000 | Drip grid |
| 12 | 4337007000 / 11006932 | Drip tray — black (Pure) / red metallic (Color) |
| 13 | WGAFG0245 | Foot |
| 14 | 11005482 | Cap for bottom of bodywork |
| 15 | 11006683 / 11006933 | Body — black (Pure) / red metallic (Color) |
| 16 | CF0292/SCH (+GB/CH/AUS/USA variants), 11005633 | Power cord (country-specific plug variants) |
| 17 | 12000782 | Green pilot lamp, 120V-230V |
| 18 | 4337018000 | Switch button (black) |
| 19 | 4337019000 | Keys/buttons support |
| 20 | 5337002000 | Unipolar switch |
| 21 | 8337008000 | Keyboard cover assembly, black |
| 22 | 8337005000 | Steam knob assembly |
| 23 | 4337014000 / 11006935 | Machine cover — black (Pure) / black assembly (Color) |
| 24 | 11005142 | Tank assembly |
| 25 | 4337012000 | Tank handle |
| 26 | 144650800 | Water tank filter |
| 27 | 140328461 | O-ring, metric 0190-10, EPDM |
| 28 | 140324362 | O-ring, metric 0060-30, silicone |
| 29 | 126764718 | Spring for water-container valve |
| 30 | 147660562 | Water-valve container piston, grey |
| 31 | 11007400 | Water-valve container piston, black |
| 32 | 4337010000 | Tank cover |
| 33 | DM0814 | Spring for timer knob |
| 34 | 11006030 | Filter basket (plastic) |
| 35 | 18G1514 | Filter-retaining spring |
| 36 | 6301002000 | Filter holder cup |
| 37 | 4332037000 | Filter holder knob |
| 38 | WGADM1319 | Special screw, 6x16 |
| 39 | 4332038000 | Filter-holder knob cap |
| 40 | 4301006000 | 2-way spout |
| 41 | 4337024000 / 11008908 | Wiring harness assembly (standard / UL variant) |
| 42 | DI7981P4.2X16Z | Self-tapping galvanized screw, plastic |

### TAV. 2 — Boiler assembly

| Pos | Part No. | Description |
|-----|----------|--------------|
| 01 | 11001002 / 11004509 | Heating element, stainless boiler, 1000W — 230V / 120V |
| 02 | U023.022 | Screw TCEI M6.0x30 |
| 03 | 11001465 | Brass nut for stainless boiler |
| 04 | 149361400 | Transparent silicone tube 5x9, 65SH (roll) |
| 05 | DI7981P3.5X9.5Z | Self-tapping galvanized screw, plastic |
| 06 | 12000361 | Oetiker clamp, D=9 |
| 07 | 140323062 | O-ring 2043, silicone |
| 08 | 11001000 | Brass nozzle, stainless boiler |
| 09 | 12000160 | **Thermostat, 95°C, model US-622AXTDNO — brew** |
| 10 | 11000998 | Brass faucet connector, stainless boiler |
| 11 | 16000380 | Silicone tube 5x8.9 (roll) |
| 12 | 11001462 | Stainless upper boiler casing |
| 13 | 12000161 | **Thermostat, 127°C, model US-622AXTDNO — steam** |
| 14 | 140320662 | O-ring 106, silicone |
| 15 | 11000994 | Thermostat-retaining spring |
| 16 | 128310304 | Washer, burnished |
| 17 | 129821402 | Screw TCB M3x4 |
| 18 | 12000087 | O-ring, metric 0850-30, silicone |
| 19 | 123390109 | Brass nut, 1/8 gas |
| 20 | 11004588 | Stainless lower boiler casing/base assembly |
| 21 | EF0012 | Seal for valve |
| 22 | EF0013 | Spring for boiler valve |
| 23 | 11000996 | Brass valve-holder screw |
| 24 | 11005079 / 11009073 | Filter-holder retaining ring — 230V / 120V |
| 25 | 0701.031.150 | Seal cover, grey |
| 26 | 129535721 | Screw TSP 3.5x9.5, stainless |
| 27 | 145842900 | Water-container valve seal (Gaco, dim.14) |
| 28 | 0701.014.150 | Seal container, grey |
| 29 | 140320461 | O-ring, metric 0080-20, Termoil |
| 30 | 11007402 | Seal cover, black |
| 31 | 11007403 | Water-container valve seal, silicone (Gaco, dim.14) |
| 32 | 11007401 | Seal container, black |
| 33 | DI7981P4.2X16Z | Self-tapping galvanized screw, plastic |
| 34 | 4337008000 | Components plate |
| 35 | UN5931-5X12I | Screw 5x12, stainless |
| 36 | 11001471 | Percolator/shower disc, Ø49.5 |
| 37 | DI7987-5X8I | Screw TSP M5x8, stainless |
| 38 | 11004543 | Filter-holder seal |
| 39 | 4337005000 | Power-inlet support bracket |
| 40 | 4332055000 | Support for spherical union |
| 41 | 4332049000 | Spherical union (Grivory) |
| 42 | DM0041/088 | O-ring gasket 2025, EPDM |
| 43 | 4332050000 | Coupling for spherical union |
| 44 | AM0011/01 | Steam tube, lower, chromed |
| 45 | 8301006000 | Frother/Pannarello assembly |
| 46 | 5332004000 | On-off bipolar switch |
| 47 | DM1563/GW / NE13.013 | **Mains power-cord inlet — self-locking 3-pole plug (230V) / IEC C13 cup socket (120V)** |
| 48 | 4337009000 / 11006934 | Power-input mask — black (Pure) / red metallic (Color) |
| 49 | WGADY0003 | Pump support, Ulka |
| 50 | 12000140 / 12000142 | **Pump, Ulka EP5/S GW (230V-50Hz) / EAP5/S (120V-60Hz)** |
| 51 | 147920300 | Tee connector, D=8 |
| 52 | 11001007 | Boiler valve assembly, black |
| 53 | DM1596 | Pump suction inlet fitting |
| 54 | 9011.144 | Fork for D.4 tube |
| 55 | 11007270 | Faucet/cock body (self-priming valve) |
| 56 | 11001003 | Faucet shaft, brass |
| 57 | WGAEF0061 | Sphere, Viton 85SH, D=5 |
| 58 | EF0099 | Self-priming valve discharge fitting |
| 59 | EF0111 | Gasket, Teflon |
| 60 | WGADM0041/031 | O-ring 2018, Viton 70SH |
| 61 | PA1062 | Pump suction pipe |

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
  - **Update (2026-08-30):** the pump/Brew-switch half of this gap is now
    closed — the user separately hand-traced the actual unit's topographic
    wiring while wiring item 4 (pump relay) and confirmed White = the
    switched wire (to the Brew Switch, component 10), Blue = unswitched
    (straight to the Main Switch, component 3). See `AGENTS.md` §7's
    "Update" note and `HARDWARE_ROADMAP.md` item 4 for how that's used.

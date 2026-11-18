# TODO

## Minitek PCB: bottom ground pour too close to the board edge

**Board:** `hardware/pcb-minitek/` (KiCad 9). The JST SH board is fine.
**Fix before ordering the next Minitek batch.**

### Problem

On the bottom layer (`B.Cu`), the GND pour comes to **0.12–0.15 mm** from
the board outline and from the slot on the right-hand side. The JST SH
board (`hardware/pcb-jst-sh/`, Altium) keeps about **0.4 mm** there.

The cause is the board rule `min_copper_edge_clearance = 0.1` mm in
`L-1K1W1007851A.kicad_pro`. The pour was filled down to that limit.

At 0.1–0.15 mm the router can cut into the copper and leave GND exposed
on the board edge. PCBWay may also stop the order to ask about it.

### Fix (in KiCad 9, so the files stay readable by KiCad 9)

1. Open `hardware/pcb-minitek/source/L-1K1W1007851A.kicad_pcb`.
2. Board Setup → Design Rules → Constraints → **Copper to edge clearance:
   0.4 mm**.
3. Refill all zones (`B`).
4. Run DRC; it should report no new errors.
5. Plot Gerbers and drill files into
   `hardware/pcb-minitek/manufacturing/gerbers/`, replacing the old ones
   (same layer names).
6. Compare the new Gerbers with `hardware/pcb-jst-sh/manufacturing/gerbers/`:
   the only differences should be at J2 (the battery connector) and the
   bottom pour, which should now pull back from the edge like on the JST SH board.
7. Upload the new Gerber zip to the PCBWay quote.

### Also noticed (minor, optional)

- The bottom solder mask has no opening around the 0.5 mm microphone
  hole at U4. The JST SH board has a 1.8 mm opening there. It has no
  effect on the mic.

### Already checked (2026-11-18)

Except for J2, the Minitek board matches the JST SH board: same nets, pad
positions, top copper, mask, paste and drill holes. The J2 footprint
matches the Amphenol 10144041 drawing, and pin 1 is VBAT, which matches
EEMB's "pin 1 = red B+". The EEMB cell fits the case (v1 and v2) if its
lead end faces the USB-C port.

#!/usr/bin/env python3
"""Writes the edits cosim.patch is made of into a tree (the export build.sh
makes, after multihart.patch). Kept so the patch can be regenerated:

    cp -r build/src /tmp/a && cp -r build/src /tmp/b
    python3 make_cosim_patch.py /tmp/b && (cd /tmp && diff -ruN a b) > cosim.patch
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])


def edit(rel, pairs):
    p = root / rel
    s = p.read_text()
    for a, b in pairs:
        assert s.count(a) == 1, (rel, a[:70])
        s = s.replace(a, b)
    p.write_text(s)


edit("model/sys/platform.sail", [
("""// Top-level MMIO dispatch
""", """// Co-simulation with DoomV (a DoomV patch; see the cosim driver in
// sail_riscv_mh.cpp and tools/verification/lockstep_linux.py).
//
// DoomV's Linux machine has devices this model does not: a UART, an RTC, an
// APLIC, virtio. To follow that machine the model takes what those devices
// answered from DoomV's own run: cosim_claim says whether an access is one
// of them, cosim_read returns the value DoomV's device gave the same load,
// and cosim_write hands over the store DoomV's device took, to be checked.
// The instructions, and everything else, are the model's own. A driver with
// no log claims nothing, and the model is exactly as it was.
val cosim_claim = impure {cpp: "cosim_claim"} : (bits(64), bits(64)) -> bool
val cosim_read  = impure {cpp: "cosim_read"}  : (bits(64), bits(64)) -> bits(64)
val cosim_write = impure {cpp: "cosim_write"} : (bits(64), bits(64), bits(64)) -> unit

private function within_cosim forall 'n, 0 < 'n <= max_mem_access . (Physaddr(addr) : physaddr, width : int('n)) -> bool =
  'n <= 8 & cosim_claim(zero_extend(64, addr), to_bits(64, width))

// Top-level MMIO dispatch
"""),
("""  else within_clint(addr, width) | within_sig(addr, width) | (within_htif_readable(addr, width) & 1 <= 'n)
""", """  else within_clint(addr, width) | within_sig(addr, width) | (within_htif_readable(addr, width) & 1 <= 'n)
       | within_cosim(addr, width)
"""),
("""  else within_clint(addr, width) | within_sig(addr, width) | (within_htif_writable(addr, width) & 'n <= 8)
""", """  else within_clint(addr, width) | within_sig(addr, width) | (within_htif_writable(addr, width) & 'n <= 8)
       | within_cosim(addr, width)
"""),
("""  else if within_htif_readable(paddr, width)
  then htif_load(access, paddr, width)
  else Err(paddr, accessFaultFromAccessType(access))
""", """  else if within_htif_readable(paddr, width)
  then htif_load(access, paddr, width)
  else if within_cosim(paddr, width) & 'n <= 8
  then {
    let Physaddr(a) = paddr;
    Ok(truncate(cosim_read(zero_extend(64, a), to_bits(64, width)), 8 * 'n))
  }
  else Err(paddr, accessFaultFromAccessType(access))
"""),
("""  else if within_htif_writable(paddr, width)
  then htif_store(paddr, width, data)
  else Err(paddr, E_SAMO_Access_Fault())
""", """  else if within_htif_writable(paddr, width)
  then htif_store(paddr, width, data)
  else if within_cosim(paddr, width) & 'n <= 8
  then {
    let Physaddr(a) = paddr;
    cosim_write(zero_extend(64, a), to_bits(64, width), zero_extend(64, data));
    Ok(true)
  }
  else Err(paddr, E_SAMO_Access_Fault())
"""),
])

edit("model/sys/interrupt_implementation.sail", [
("""function external_interrupts_pending(hgeie_val : xlenbits) -> Minterrupts =
  // The CLINT doesn't deal with external interrupts.
  if plat_have_sig then sig_external_interrupts_pending(hgeie_val) else Mk_Minterrupts(zeros())
""", """// The external-interrupt lines of a co-simulated machine (a DoomV patch):
// what DoomV's APLIC drives into the hart -- SEIP, bit 9 -- as DoomV's log
// says, step by step. Zero without a log.
val cosim_external = impure {cpp: "cosim_external"} : unit -> bits(64)

function external_interrupts_pending(hgeie_val : xlenbits) -> Minterrupts = {
  // The CLINT doesn't deal with external interrupts.
  let sig = if plat_have_sig then sig_external_interrupts_pending(hgeie_val) else Mk_Minterrupts(zeros());
  Mk_Minterrupts(sig.bits | truncate(cosim_external(), xlen))
}
"""),
])

edit("c_emulator/riscv_platform_if.h", [
("""class PlatformInterface {
public:
  // Unset, no access is claimed and the model is the single hart it was.
""", """// Co-simulation with DoomV (a DoomV patch; see cosim_claim in
// model/sys/platform.sail): the devices the model does not have, answered
// from DoomV's log of its run.
class Cosim {
public:
  virtual ~Cosim() = default;
  virtual bool claim(uint64_t paddr, uint64_t width) = 0;
  virtual uint64_t read(uint64_t paddr, uint64_t width) = 0;
  virtual void write(uint64_t paddr, uint64_t width, uint64_t value) = 0;
  virtual uint64_t external() = 0;
};

class PlatformInterface {
public:
  // Unset, nothing is the log's and no external interrupt is raised.
  Cosim *cosim = nullptr;
  bool cosim_claim(uint64_t paddr, uint64_t width) {
    return cosim != nullptr && cosim->claim(paddr, width);
  }
  uint64_t cosim_read(uint64_t paddr, uint64_t width) {
    return cosim->read(paddr, width);
  }
  unit cosim_write(uint64_t paddr, uint64_t width, uint64_t value) {
    cosim->write(paddr, width, value);
    return UNIT;
  }
  uint64_t cosim_external(unit) {
    return cosim != nullptr ? cosim->external() : 0;
  }

  // Unset, no access is claimed and the model is the single hart it was.
"""),
])
print("ok")

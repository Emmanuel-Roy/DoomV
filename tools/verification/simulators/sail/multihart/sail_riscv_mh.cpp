// sail_riscv_mh: the Sail RISC-V model as a machine of several harts.
//
// Sail models one hart. This driver runs N copies of that model -- the
// generated code unchanged apart from the CLINT patch beside this file --
// over the one memory the Sail runtime keeps, and adds the three things a
// shared machine needs that a single hart never has to say:
//
//   * who runs when. Harts step round-robin, hart 0 first, one step each per
//     round. That order is the machine's: DoomV's -harts=N runs the same one,
//     which is what lets the two be lock-stepped.
//   * the clock. mtime is one platform-wide counter. It moves as Sail's does
//     for one hart, counted in rounds rather than steps: once every
//     instructions_per_tick rounds in which some hart retired, or every round
//     in which all harts are waiting. With one hart this is exactly
//     riscv_sim.cpp's loop.
//   * the CLINT array and the reservations. Another hart's msip and mtimecmp
//     are reached through the patched model's clint_remote hook, and a write
//     by one hart cancels any other hart's LR reservation on the bytes it
//     wrote.
//
// Arguments are sail_riscv_sim's, plus --harts N (default 1). With more than
// one hart the trace gains a line "hart <i>" before each step of a hart other
// than the one that stepped last, so a reader knows whose records follow.
// Each hart starts at the ELF's entry with a0 = its mhartid, the boot
// convention Sail's init_boot_requirements sets up.

#include "cli_options.h"
#include "config_utils.h"
#include "riscv_callbacks_log.h"
#include "riscv_model_impl.h"
#include "riscv_platform_if.h"
#include "riscv_sim.h"
#include "traploop_detector.h"

#include <jsoncons/json.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The CLINT's layout (model/sys/platform.sail): msip at 4*hart, mtimecmp at
// 0x4000 + 8*hart, mtime at 0xbff8.
constexpr uint64_t MTIMECMP_BASE = 0x4000;
constexpr uint64_t MTIME_BASE = 0xbff8;

struct Machine;

// Another hart's msip and mtimecmp, decoded exactly as the model decodes its
// own: msip as a 4- or 8-byte access at its address, mtimecmp as 8 bytes or
// either 4-byte half.
class Clint final : public ClintRemote {
public:
  explicit Clint(Machine &m) : machine(m) {}
  bool claim(PlatformInterface &self, uint64_t offset, uint64_t width, bool store) override;
  uint64_t read(PlatformInterface &self, uint64_t offset, uint64_t width) override;
  void write(PlatformInterface &self, uint64_t offset, uint64_t width, uint64_t value) override;

private:
  enum class Reg { None, Msip, Cmp, CmpHi };
  Reg decode(uint64_t offset, uint64_t width, size_t &hart) const;
  Machine &machine;
};

// A store by one hart ends every other hart's reservation on the bytes it
// wrote. (A hart's own stores leave its reservation alone, as the
// configuration's invalidate_on_same_hart_store says; the model does that.)
class Reservations final : public callbacks_if {
public:
  explicit Reservations(Machine &m) : machine(m) {}
  void mem_write_callback(ModelImpl &model, const char *, sbits paddr, int64_t width, lbits) override;

private:
  Machine &machine;
};

struct Machine {
  std::vector<std::unique_ptr<ModelImpl>> harts;
  uint64_t granule = 8;   // the reservation set, from the configuration

  size_t index_of(PlatformInterface &p) const {
    for (size_t i = 0; i < harts.size(); i++)
      if (&harts[i]->platform() == &p) return i;
    abort();
  }
  size_t index_of(ModelImpl &m) const { return index_of(m.platform()); }
};

Clint::Reg Clint::decode(uint64_t offset, uint64_t width, size_t &hart) const {
  if (offset < MTIMECMP_BASE) {
    if (offset % 4 != 0 || (width != 4 && width != 8)) return Reg::None;
    hart = offset / 4;
    return Reg::Msip;
  }
  if (offset < MTIME_BASE) {
    const uint64_t rel = offset - MTIMECMP_BASE;
    hart = rel / 8;
    if (rel % 8 == 0 && (width == 4 || width == 8)) return Reg::Cmp;
    if (rel % 8 == 4 && width == 4) return Reg::CmpHi;
  }
  return Reg::None;
}

bool Clint::claim(PlatformInterface &self, uint64_t offset, uint64_t width, bool) {
  size_t hart = 0;
  if (decode(offset, width, hart) == Reg::None) return false;
  return hart < machine.harts.size() && hart != machine.index_of(self);
}

uint64_t Clint::read(PlatformInterface &, uint64_t offset, uint64_t width) {
  size_t hart = 0;
  const Reg reg = decode(offset, width, hart);
  hart::Model &t = machine.harts[hart]->state();
  switch (reg) {
  case Reg::Msip:
    return t.zmsip;
  case Reg::Cmp:
    return width == 8 ? t.zmtimecmp : (t.zmtimecmp & 0xffffffffu);
  case Reg::CmpHi:
    return t.zmtimecmp >> 32;
  default:
    abort();
  }
}

void Clint::write(PlatformInterface &, uint64_t offset, uint64_t width, uint64_t value) {
  size_t hart = 0;
  const Reg reg = decode(offset, width, hart);
  hart::Model &t = machine.harts[hart]->state();
  switch (reg) {
  case Reg::Msip:
    t.zmsip = (t.zmsip & ~UINT64_C(1)) | (value & 1);
    break;
  case Reg::Cmp:
    t.zmtimecmp = width == 8 ? value : ((t.zmtimecmp & ~UINT64_C(0xffffffff)) | (value & 0xffffffffu));
    break;
  case Reg::CmpHi:
    t.zmtimecmp = (t.zmtimecmp & UINT64_C(0xffffffff)) | (value << 32);
    break;
  default:
    abort();
  }
}

void Reservations::mem_write_callback(ModelImpl &model, const char *, sbits paddr, int64_t width, lbits) {
  const size_t self = machine.index_of(model);
  const uint64_t first = paddr.bits & ~(machine.granule - 1);
  const uint64_t last = paddr.bits + (uint64_t)width - 1;
  for (size_t k = 0; k < machine.harts.size(); k++) {
    if (k == self) continue;
    PlatformInterface &other = machine.harts[k]->platform();
    for (uint64_t g = first; g <= last; g += machine.granule) {
      sbits a;
      a.len = paddr.len;
      a.bits = g;
      if (other.match_reservation(a)) {
        other.cancel_reservation(UNIT);
        break;
      }
    }
  }
}

// The setters preinit_model applies to the first model, for the rest.
void configure(ModelImpl &m, const CLIOptions &opts) {
  if (opts.config_enable_experimental_extensions) m.set_enable_experimental_extensions(true);
  m.set_config_print_instr(opts.config_print_instr);
  m.set_config_print_clint(opts.config_print_clint);
  m.set_config_print_exception(opts.config_print_exception);
  m.set_config_print_interrupt(opts.config_print_interrupt);
  m.set_config_print_htif(opts.config_print_htif);
  m.set_config_print_pma(opts.config_print_pma);
  m.set_config_print_pmp(opts.config_print_pmp);
  m.set_config_rvfi(false);
  m.set_config_use_abi_names(opts.config_use_abi_names);
  m.set_config_print_step(opts.config_print_step);
}

// The configuration with platform.hartid set to `hart`. Nothing else in it
// changes: every hart is the configured hart, under its own number.
std::string config_for_hart(const std::string &config, size_t hart) {
  jsoncons::json j = jsoncons::json::parse(config);
  j["platform"]["hartid"] = hart;
  std::ostringstream os;
  os << j;
  return os.str();
}

// Takes --harts N or --harts=N out of argv, for parse_cli not to see.
size_t take_harts(int &argc, char **argv) {
  size_t n = 1;
  int out = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--harts") == 0 && i + 1 < argc) {
      n = strtoull(argv[++i], nullptr, 0);
    } else if (strncmp(argv[i], "--harts=", 8) == 0) {
      n = strtoull(argv[i] + 8, nullptr, 0);
    } else {
      argv[out++] = argv[i];
    }
  }
  argc = out;
  argv[argc] = nullptr;
  if (n < 1 || n > 4095) {
    fprintf(stderr, "--harts must be 1 through 4095 (the CLINT's limit)\n");
    exit(EXIT_FAILURE);
  }
  return n;
}

int inner_main(int argc, char **argv) {
  const size_t nharts = take_harts(argc, argv);
  CLIOptions opts = parse_cli(argc, argv);
  if (opts.gdb_server_port != 0 || opts.rvfi_dii_port != 0) {
    fprintf(stderr, "sail_riscv_mh: the gdb server and RVFI-DII are sail_riscv_sim's alone\n");
    return EXIT_FAILURE;
  }

  std::string config_json_string;
  switch (preinit_args(opts, config_json_string)) {
  case InitResult::ExitSuccess:
    return EXIT_SUCCESS;
  case InitResult::ExitFailure:
    return EXIT_FAILURE;
  case InitResult::Continue:
    break;
  }

  Machine machine;
  run_info run_info;

  // Hart 0 is set up exactly as sail_riscv_sim sets up its one hart, and
  // loads the ELF into the shared memory.
  machine.harts.push_back(std::make_unique<ModelImpl>());
  ModelImpl &first = *machine.harts[0];
  switch (preinit_model(opts, first, config_for_hart(config_json_string, 0), run_info)) {
  case InitResult::ExitSuccess:
    return EXIT_SUCCESS;
  case InitResult::ExitFailure:
    return EXIT_FAILURE;
  case InitResult::Continue:
    break;
  }
  elf_info elf_info;
  uint64_t entry = 0;
  if (init_model(opts, first, elf_info, run_info, entry) != InitResult::Continue) return EXIT_FAILURE;

  // The rest share that memory; each comes up from reset under its own
  // hartid. The memory is not touched again.
  for (size_t h = 1; h < nharts; h++) {
    machine.harts.push_back(std::make_unique<ModelImpl>());
    ModelImpl &m = *machine.harts[h];
    configure(m, opts);
    sail_config_set_string(config_for_hart(config_json_string, h).c_str());
    m.init_platform_constants();
    m.model_init();
    m.set_term_fd(run_info.term_fd);
    m.set_trace_log(run_info.trace_log);
    m.init_sail(entry, opts.config_file.c_str(), elf_info.htif_tohost_address);
  }
  machine.granule = UINT64_C(1) << get_config_uint64({"platform", "reservation", "reservation_set_size_exp"});

  Clint clint(machine);
  auto reservations = std::make_shared<Reservations>(machine);
  auto log_cbs = std::make_shared<log_callbacks>(
    opts.config_print_gpr,
    opts.config_print_fpr,
    opts.config_print_vreg,
    opts.config_print_csr,
    opts.config_print_mem_access,
    opts.config_print_ptw,
    opts.config_print_tlb,
    opts.config_use_abi_names,
    run_info.trace_log
  );
  std::vector<std::shared_ptr<traploop_detector>> loops;
  for (auto &m : machine.harts) {
    if (nharts > 1) {
      m->platform().clint_remote = &clint;
      m->register_callback(reservations);
    }
    loops.push_back(std::make_shared<traploop_detector>());
    if (!opts.disable_trap_loop_detection) m->register_callback(loops.back());
    m->register_callback(log_cbs);
  }

  // The loop is riscv_sim.cpp's run_sail with the step taken by each hart in
  // turn; see the comment at the top for the clock.
  const uint64_t max_wait_steps = get_config_uint64({"platform", "max_time_to_wait"});
  const uint64_t insns_per_tick = get_config_uint64({"platform", "instructions_per_tick"});
  std::vector<bool> waiting(nharts, false);
  std::vector<uint64_t> wait_remaining(nharts, 0);
  std::vector<int64_t> step_no(nharts, 0);
  uint64_t insn_cnt = 0;
  uint64_t mtime = machine.harts[0]->state().zmtime;
  size_t last_hart = SIZE_MAX;
  ModelImpl *ended = nullptr;

  while (ended == nullptr && (opts.insn_limit == 0 || run_info.total_insns < opts.insn_limit)) {
    bool retired = false;
    for (size_t h = 0; h < nharts && ended == nullptr; h++) {
      ModelImpl &m = *machine.harts[h];
      if (nharts > 1 && h != last_hart) {
        fprintf(run_info.trace_log, "hart %zu\n", h);
        last_hart = h;
      }
      m.call_pre_step_callbacks(waiting[h]);
      waiting[h] = m.try_step(step_no[h], wait_remaining[h] == 0);
      if (std::optional<std::string> e = m.string_of_current_exception()) {
        fprintf(stdout, "%s\n", e->c_str());
        ended = &m;
        break;
      }
      if (opts.config_print_instr) flush_logs(run_info);
      if (waiting[h]) {
        wait_remaining[h] = wait_remaining[h] == 0 ? max_wait_steps : wait_remaining[h] - 1;
      } else {
        wait_remaining[h] = 0;
      }
      m.call_post_step_callbacks(waiting[h]);
      if (!waiting[h]) {
        if (opts.config_print_step) fprintf(run_info.trace_log, "\n");
        step_no[h]++;
        run_info.total_insns++;
        retired = true;
      }

      // A write to mtime by this hart is a write to the one platform clock.
      if (m.state().zmtime != mtime) {
        mtime = m.state().zmtime;
        for (auto &o : machine.harts) o->state().zmtime = mtime;
      }

      if (m.htif_done()) {
        if (m.htif_exit_code() == 0) {
          fprintf(stdout, "SUCCESS\n");
        } else {
          fprintf(stdout, "FAILURE: %" PRIu64 " (0x%08" PRIx64 ")\n", m.htif_exit_code(), m.htif_exit_code());
          exit(EXIT_FAILURE);
        }
        ended = &m;
        break;
      }
      if (loops[h]->loop_detected()) {
        fprintf(stdout, "FAILURE: possible trap loop detected on hart %zu with MEPC=0x%" PRIx64 " and SEPC=0x%" PRIx64 "\n",
                h, loops[h]->mepc(), loops[h]->sepc());
        exit(EXIT_FAILURE);
      }
    }
    if (ended != nullptr) break;

    if (retired) insn_cnt++;
    bool all_waiting = true;
    for (size_t h = 0; h < nharts; h++) all_waiting = all_waiting && wait_remaining[h] > 0;
    if (insn_cnt == insns_per_tick) {
      insn_cnt = 0;
      for (auto &m : machine.harts) m->tick_clock();
      mtime = machine.harts[0]->state().zmtime;
    } else if (all_waiting) {
      for (auto &m : machine.harts) m->tick_clock();
      mtime = machine.harts[0]->state().zmtime;
    }
  }

  finish(ended != nullptr ? *ended : first, opts, elf_info, run_info);
  return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return inner_main(argc, argv);
  } catch (const std::exception &exc) {
    std::cerr << "Error: " << exc.what() << std::endl;
  }
  return EXIT_FAILURE;
}

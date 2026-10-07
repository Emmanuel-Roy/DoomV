#pragma once
#include <memory>

class DoomSystem;

// The machine a command line describes, set up and ready to run: everything
// main does before DoomSystem::run, which is also what the in-process
// lock-step (doomv_lockstep.h) does with the arguments it is given. False
// when there is nothing to run -- a bad command line, or -export-state,
// which has done its work -- with the process's exit status in `status`.
//
// One machine per process: the extensions, VLEN and the RAM size it sets
// are the process's.
bool setup_machine(int argc, const char *const *argv, std::unique_ptr<DoomSystem> &system, int &status,
                   bool in_process = false);

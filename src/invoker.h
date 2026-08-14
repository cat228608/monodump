#pragma once
#include <string>

// The invoker turns the JSON dump into something actionable. It watches a
// command file next to the DLL and executes lines like:
//
//   call RecipeManager::UnlockAll()
//   call Inventory::AddResource("wood", 9999) on GameManager::Instance
//   get  PlayerStats::money
//   get  Player::health on Player::local          <- instance field (v1.1)
//   set  PlayerStats::money 999999
//   set  Player::health 999 on Player::local      <- instance field (v1.1)
//
// COMMAND FILE PROTOCOL (v1.1)
//   * A batch is only executed once the file ends with a newline, so a caller
//     can never catch a half-written file.
//   * An optional first line "#seq <token>" is echoed into monodump_result.txt
//     as "seq=<token>" / "end seq=<token> ok=N fail=M", which is how a trainer
//     matches an answer to the batch it just sent.
//   * The file is truncated after the batch runs, so the same commands never
//     fire twice.
//   * Polling is size-based, not timestamp-based: two batches written inside the
//     same filesystem timestamp tick are both executed (v1.0 lost the second).

// dir is the folder holding the payload. main_thread requests that commands run
// on the game's main thread through dispatch.h instead of the watcher thread.
void StartInvoker(const std::string& dir, bool main_thread);

// Signals the watcher thread and waits for it to leave the module. Must be
// called before the DLL unloads, otherwise the thread keeps running inside
// freed code - that was a guaranteed crash in v1.0.
void StopInvoker();

// Executes a single command line. Exposed so the payload can also run a
// one-shot command passed through monodump.ini.
void RunCommand(const std::string& line);

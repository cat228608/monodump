#pragma once
//
// log.h - output plumbing.
//
// v1.0 printed everything to a console it allocated itself. That is fine when a
// human is watching, but useless for a tool driving the payload: there was no
// way to learn whether a command succeeded, and closing that console window
// killed the game. So output now has three independent sinks:
//
//   console            - AllocConsole, as before (console=1)
//   monodump.log       - everything, always available after the fact (log=1)
//   monodump_result.txt- machine-readable result of the last command batch
//
// The result file is rewritten per batch and looks like this:
//
//   seq=7
//   [+] RecipeManager::UnlockAll invoked
//   [-] class 'Nope' not found
//   end seq=7 ok=1 fail=1
//
// A caller writes "#seq 7" as the first line of the command file and then polls
// for "end seq=7".
//

#include <string>

namespace mdlog {

const char* Version();

// dir is the folder holding the payload; console/log pick the sinks.
void Init(const std::string& dir, bool console, bool file);

// General progress output. Goes to console + monodump.log.
void Printf(const char* fmt, ...);

// "What we were doing when the lights went out": rewrites monodump_last.txt in
// place. If the game dies mid-dump, that one line names the class that did it.
// Cheap enough to call per class because the handle stays open.
void Crumb(const char* what);

// Command batch bracketing. seq is whatever the caller put in "#seq <...>".
void BeginBatch(const std::string& seq);
void EndBatch();

// One command outcome. ok=false also bumps the failure counter of the batch.
void Ok(const char* fmt, ...);
void Fail(const char* fmt, ...);

// Handshake file: proves the payload is alive and the runtime was found.
void WriteReady(const std::string& runtime, bool mainthread, bool interactive);
void RemoveReady();

} // namespace mdlog

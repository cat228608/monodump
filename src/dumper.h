#pragma once
#include <string>

struct DumpOptions {
    std::string out_dir;            // where dump.txt / dump.json land
    std::string assembly_filter;    // substring; empty = all assemblies
    std::string name_filter;        // substring on class/field/method names
    bool        compile_methods = false;  // JIT each method to get its address
    // OFF by default since 1.1.2: reading a static value forces the class to be
    // initialised, which runs its cctor - arbitrary managed code - and that is
    // the single most common way to crash a Unity game while dumping.
    bool        static_values   = false;  // read current values of static fields
    bool        json            = true;
    bool        text            = true;
    bool        properties      = true;   // dump properties and nested types
    // ON by default since 1.1.5: compiler-generated types (<Foo>d__12 async /
    // iterator state machines, <>c lambda caches, <PrivateImplementationDetails>)
    // are skipped. They hold nothing you can cheat with, and initialising one
    // can drag in the whole task/awaiter graph of a networked game and kill it.
    bool        skip_generated  = true;
    std::string skip_types;         // comma-separated substrings of class names to skip
};

// Walks every loaded assembly and writes the dump. Safe to call from a thread
// created inside the target process.
void RunDump(const DumpOptions& opts);

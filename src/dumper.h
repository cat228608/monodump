#pragma once
#include <string>

struct DumpOptions {
    std::string out_dir;            // where dump.txt / dump.json land
    std::string assembly_filter;    // substring; empty = all assemblies
    std::string name_filter;        // substring on class/field/method names
    bool        compile_methods = false;  // JIT each method to get its address
    bool        static_values   = true;   // read current values of static fields
    bool        json            = true;
    bool        text            = true;
    bool        properties      = true;   // dump properties and nested types
};

// Walks every loaded assembly and writes the dump. Safe to call from a thread
// created inside the target process.
void RunDump(const DumpOptions& opts);

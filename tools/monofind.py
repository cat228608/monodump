#!/usr/bin/env python3
"""
monofind - turn a monodump dump.json into leads.

The dump is huge. This ranks what is worth looking at, and prints the exact
dnSpy token and the ready-to-paste monodump command for each hit.

    python monofind.py dump.json --hunt
    python monofind.py dump.json --grep unlock
    python monofind.py dump.json --class RecipeManager
    python monofind.py dump.json --callable --grep add
    python monofind.py dump.json --stats
"""

import argparse
import json
import re
import sys

# Keyword groups that consistently mark the functions worth calling in a
# single-player game. Weighted, because "Unlock" is a much stronger signal
# than "Get".
HUNT = {
    100: ["unlockall", "unlockeverything", "giveall", "completeall", "maxall"],
     80: ["unlock", "cheat", "debug", "grant", "award", "reward"],
     60: ["addmoney", "addcoin", "addgold", "addgem", "addresource", "addcurrency",
          "addcredit", "addexp", "addxp", "setmoney", "givemoney", "giveitem"],
     45: ["recipe", "gallery", "achievement", "collectible", "blueprint",
          "skin", "costume", "scene", "cg", "level", "perk", "upgrade"],
     35: ["purchase", "buy", "craft", "unlockitem", "discover", "research"],
     25: ["money", "coin", "gold", "gem", "cash", "currency", "credit",
          "resource", "inventory", "stamina", "energy", "health"],
     15: ["add", "give", "set", "increase", "grantall", "enable", "complete"],
}

# Types monodump's invoker can actually marshal.
SIMPLE = {
    "System.Int32", "System.UInt32", "System.Int64", "System.UInt64",
    "System.Single", "System.Double", "System.Boolean", "System.String",
}


def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return json.load(fh)


def walk(data):
    """Yield (assembly, cls) for every class in the dump."""
    for asm in data.get("assemblies", []):
        for cls in asm.get("classes", []):
            yield asm, cls


def score(cls, method):
    """How likely is this method to be the one you are hunting for."""
    hay = (cls["full_name"] + "." + method["name"]).lower()
    total = 0
    hits = []
    for weight, words in HUNT.items():
        for w in words:
            if w in hay:
                total += weight
                hits.append(w)
                break
    if total == 0:
        return 0, []

    # Callability matters more than the name. A static method taking no
    # arguments is something you can fire immediately; an abstract method on
    # an interface is not.
    if method["abstract"]:
        return 0, []
    if method["special"] and not method["name"].startswith(("get_", "set_")):
        total -= 20
    if method["static"]:
        total += 30
    if method["param_count"] == 0:
        total += 25
    elif all(p["type"] in SIMPLE for p in method["params"]):
        total += 10
    else:
        total -= 15  # needs an object we cannot construct from a command line

    if method["return"] == "System.Void":
        total += 5   # a void method usually performs an action
    return total, hits


def sig(method):
    args = ", ".join(
        "%s %s" % (p["type"].split(".")[-1], p["name"] or "?")
        for p in method["params"]
    )
    return "%s %s(%s)" % (method["return"].split(".")[-1], method["name"], args)


def command_for(cls, method):
    """The line to paste into monodump_cmd.txt."""
    if not all(p["type"] in SIMPLE for p in method["params"]):
        return None
    args = []
    for p in method["params"]:
        if p["type"] == "System.String":
            args.append('"%s"' % (p["name"] or "value"))
        elif p["type"] == "System.Boolean":
            args.append("true")
        else:
            args.append("9999")
    line = "call %s::%s(%s)" % (cls["full_name"], method["name"], ", ".join(args))
    if not method["static"]:
        line += "   on %s::Instance   # <- confirm the singleton field name" % cls["full_name"]
    return line


def print_method(cls, method, indent="    ", show_cmd=True):
    flags = []
    if method["static"]:
        flags.append("static")
    if method["virtual"]:
        flags.append("virtual")
    if not method["public"]:
        flags.append("non-public")
    tag = (" [%s]" % " ".join(flags)) if flags else ""

    print("%s%s%s" % (indent, sig(method), tag))
    print("%s  dnSpy token: %s" % (indent, method["token"]))
    if show_cmd:
        cmd = command_for(cls, method)
        if cmd:
            print("%s  %s" % (indent, cmd))
        else:
            print("%s  (params are object types - hook it instead of calling it)" % indent)


def cmd_hunt(data, args):
    results = []
    for _asm, cls in walk(data):
        for m in cls.get("methods", []):
            s, hits = score(cls, m)
            if s >= args.min_score:
                results.append((s, cls, m, hits))

    results.sort(key=lambda r: -r[0])
    if not results:
        print("No candidates. Try --grep with a word from the game's own UI.")
        return

    print("%d candidates (showing %d)\n" % (len(results), min(len(results), args.limit)))
    last_cls = None
    for s, cls, m, hits in results[: args.limit]:
        if cls["full_name"] != last_cls:
            print("%s   %s" % (cls["full_name"], cls["token"]))
            last_cls = cls["full_name"]
        print("  score %-4d matched: %s" % (s, ", ".join(hits)))
        print_method(cls, m)
        print()


def cmd_grep(data, args):
    rx = re.compile(args.grep, re.I)
    for _asm, cls in walk(data):
        cls_hit = rx.search(cls["full_name"])
        methods = [m for m in cls.get("methods", []) if rx.search(m["name"])]
        fields = [f for f in cls.get("fields", []) if rx.search(f["name"])]
        props = [p for p in cls.get("properties", []) if rx.search(p["name"])]

        if args.callable:
            methods = [m for m in methods if m["static"] and not m["abstract"]]

        if not (cls_hit or methods or fields or props):
            continue

        print("%s   %s   size %d" % (cls["full_name"], cls["token"], cls["instance_size"]))
        for f in fields:
            extra = ""
            if f["static"] and f["value"]:
                extra = "  = %s" % f["value"]
            print("    field %-10s %-32s %s  @+0x%X%s" % (
                "static" if f["static"] else "",
                f["type"].split(".")[-1], f["name"], f["offset"], extra))
            if f["static"] and not f["const"]:
                print("      set %s::%s 999999" % (cls["full_name"], f["name"]))
        for p in props:
            print("    prop  %-32s %s { %s%s}" % (
                p["type"].split(".")[-1], p["name"],
                "get " if p["get"] else "", "set " if p["set"] else ""))
            if p["set"]:
                print("      dnSpy setter token: %s" % p["set_token"])
        for m in methods:
            print_method(cls, m)
        print()


def cmd_class(data, args):
    want = args.klass.lower()
    found = False
    for _asm, cls in walk(data):
        if want not in cls["full_name"].lower():
            continue
        found = True
        print("=" * 70)
        print(cls["full_name"])
        print("  token        %s" % cls["token"])
        print("  parent       %s" % (cls["parent"] or "-"))
        if cls["interfaces"]:
            print("  implements   %s" % ", ".join(cls["interfaces"]))
        print("  size         %d bytes" % cls["instance_size"])
        if cls["nested"]:
            print("  nested       %s" % ", ".join(cls["nested"]))

        if cls["fields"]:
            print("\n  fields")
            for f in cls["fields"]:
                kind = "const" if f["const"] else ("static" if f["static"] else "")
                val = "  = %s" % f["value"] if f["value"] else ""
                print("    +0x%-5X %-7s %-34s %s%s" % (
                    f["offset"], kind, f["type"].split(".")[-1], f["name"], val))

        if cls["properties"]:
            print("\n  properties")
            for p in cls["properties"]:
                print("    %-34s %s { %s%s}" % (
                    p["type"].split(".")[-1], p["name"],
                    "get " if p["get"] else "", "set " if p["set"] else ""))

        if cls["methods"]:
            print("\n  methods")
            for m in cls["methods"]:
                print("    %-6s %-58s %s" % (
                    "static" if m["static"] else "", sig(m), m["token"]))
        print()

    if not found:
        print("No class matching '%s'." % args.klass)


def cmd_stats(data, args):
    print("skipped classes: %d" % data.get("skipped_classes", 0))
    print()
    print("%-40s %8s %8s %8s" % ("ASSEMBLY", "CLASSES", "FIELDS", "METHODS"))
    for asm in data.get("assemblies", []):
        nf = sum(len(c["fields"]) for c in asm["classes"])
        nm = sum(len(c["methods"]) for c in asm["classes"])
        print("%-40s %8d %8d %8d" % (asm["name"][:40], len(asm["classes"]), nf, nm))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("json", help="dump.json produced by monodump")
    ap.add_argument("--hunt", action="store_true",
                    help="rank methods that look like unlock/give/add functions")
    ap.add_argument("--grep", help="regex over class, field, property and method names")
    ap.add_argument("--class", dest="klass", help="print one class in full")
    ap.add_argument("--stats", action="store_true", help="per-assembly counts")
    ap.add_argument("--callable", action="store_true",
                    help="with --grep, only static non-abstract methods")
    ap.add_argument("--limit", type=int, default=40, help="max hunt results")
    ap.add_argument("--min-score", type=int, default=40, help="hunt cutoff")
    args = ap.parse_args()

    data = load(args.json)
    if data.get("schema") != 2:
        print("warning: unexpected schema version", file=sys.stderr)

    if args.stats:
        cmd_stats(data, args)
    elif args.klass:
        cmd_class(data, args)
    elif args.grep:
        cmd_grep(data, args)
    else:
        cmd_hunt(data, args)


if __name__ == "__main__":
    main()

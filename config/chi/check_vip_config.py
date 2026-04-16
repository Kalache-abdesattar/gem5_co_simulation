#!/usr/bin/env python3
"""
check_vip_config.py — structural validator for vip_requestor.py and
                      vip_hierarchy.py.

Checks (no gem5 build required — uses AST analysis only):
  1. vip_requestor.py has valid Python syntax.
  2. VIPRequestor class is defined and inherits from AbstractNode.
  3. Required CHI policy attributes are set in __init__.
  4. vip_hierarchy.py has valid Python syntax.
  5. VIPHierarchy class is defined.
  6. VIPHierarchy imports VIPRequestor.
  7. VIPHierarchy.incorporate_cache creates VIPRequestor instances.
  8. VIPHierarchy.incorporate_cache connects vip_nodes to downstream.
  9. questa_cosim_top.cc exists and contains required identifiers.
 10. questa_cosim_top.cc declares the questa_top instantiation.
 11. questa_cosim_top.cc has the QUESTA_STANDALONE_TEST branch.
 12. Makefile contains questa_cosim_test build rule.
"""

import ast
import re
import sys
from pathlib import Path

BASE    = Path(__file__).parent
NODES   = BASE / "nodes"
VIP_REQ = NODES / "vip_requestor.py"
VIP_HIE = BASE  / "vip_hierarchy.py"
COSIM   = Path(__file__).parent.parent.parent / "chi_vip" / "questa" / "questa_cosim_top.cc"
MAKEFILE = Path(__file__).parent.parent.parent / "chi_vip" / "Makefile"

pass_count = 0
fail_count = 0

def check(cond, msg):
    global pass_count, fail_count
    if cond:
        print(f"  PASS: {msg}")
        pass_count += 1
    else:
        print(f"  FAIL: {msg}")
        fail_count += 1

def parse_py(path):
    """Return AST tree or None on syntax error."""
    try:
        return ast.parse(path.read_text(), filename=str(path))
    except SyntaxError as e:
        print(f"  FAIL: {path.name} syntax error: {e}")
        return None

def find_class(tree, name):
    """Return ClassDef node for 'name', or None."""
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == name:
            return node
    return None

def class_inherits(classdef, base_name):
    """Check if classdef directly inherits from base_name."""
    for base in classdef.bases:
        if isinstance(base, ast.Name) and base.id == base_name:
            return True
        if isinstance(base, ast.Attribute) and base.attr == base_name:
            return True
    return False

def assigns_attr(classdef, attr):
    """Return True if 'self.<attr> = ...' appears in classdef methods."""
    for node in ast.walk(classdef):
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if (isinstance(t, ast.Attribute) and
                        isinstance(t.value, ast.Name) and
                        t.value.id == 'self' and
                        t.attr == attr):
                    return True
    return False

def has_import(tree, name):
    """Return True if 'name' appears in any import statement."""
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            src = ast.dump(node)
            if name in src:
                return True
    return False

def func_contains_name(classdef, func_name, target_name):
    """Return True if method func_name in classdef contains identifier target_name."""
    for node in ast.walk(classdef):
        if isinstance(node, ast.FunctionDef) and node.name == func_name:
            src = ast.dump(node)
            return target_name in src
    return False

# ── [1] vip_requestor.py syntax ──────────────────────────────────────────────
print("\n[1] vip_requestor.py: Python syntax")
vip_req_tree = parse_py(VIP_REQ)
check(vip_req_tree is not None, "vip_requestor.py parses without syntax errors")

if vip_req_tree:
    # ── [2] VIPRequestor class and inheritance ────────────────────────────────
    print("\n[2] VIPRequestor class definition")
    vip_cls = find_class(vip_req_tree, "VIPRequestor")
    check(vip_cls is not None, "class VIPRequestor defined")
    if vip_cls:
        check(class_inherits(vip_cls, "AbstractNode"),
              "VIPRequestor inherits from AbstractNode")

    # ── [3] Required CHI policy attributes ───────────────────────────────────
    print("\n[3] CHI policy attributes in VIPRequestor.__init__")
    required_attrs = [
        "is_HN", "enable_DMT", "enable_DCT",
        "allow_SD", "alloc_on_seq_acc", "send_evictions",
        "number_of_TBEs", "number_of_snoop_TBEs", "use_prefetcher",
    ]
    if vip_cls:
        for attr in required_attrs:
            check(assigns_attr(vip_cls, attr),
                  f"self.{attr} assigned in VIPRequestor")

# ── [4] vip_hierarchy.py syntax ──────────────────────────────────────────────
print("\n[4] vip_hierarchy.py: Python syntax")
vip_hie_tree = parse_py(VIP_HIE)
check(vip_hie_tree is not None, "vip_hierarchy.py parses without syntax errors")

if vip_hie_tree:
    # ── [5] VIPHierarchy class ────────────────────────────────────────────────
    print("\n[5] VIPHierarchy class definition")
    hie_cls = find_class(vip_hie_tree, "VIPHierarchy")
    check(hie_cls is not None, "class VIPHierarchy defined")

    # ── [6] VIPHierarchy imports VIPRequestor ─────────────────────────────────
    print("\n[6] VIPHierarchy imports VIPRequestor")
    check(has_import(vip_hie_tree, "VIPRequestor"),
          "VIPRequestor imported in vip_hierarchy.py")

    # ── [7]-[8] incorporate_cache instantiates and connects VIP nodes ─────────
    print("\n[7-8] VIPHierarchy.incorporate_cache creates and connects vip_nodes")
    if hie_cls:
        check(func_contains_name(hie_cls, "incorporate_cache", "VIPRequestor"),
              "incorporate_cache instantiates VIPRequestor")
        check(func_contains_name(hie_cls, "incorporate_cache", "vip_nodes"),
              "incorporate_cache appends to self.vip_nodes")
        check(func_contains_name(hie_cls, "incorporate_cache", "downstream_destinations"),
              "incorporate_cache sets downstream_destinations on VIP nodes")

# ── [9] questa_cosim_top.cc exists ────────────────────────────────────────────
print("\n[9] questa_cosim_top.cc file presence")
check(COSIM.exists(), f"questa_cosim_top.cc exists at {COSIM.relative_to(COSIM.parent.parent.parent)}")

if COSIM.exists():
    cosim_text = COSIM.read_text()

    # ── [10] questa_top instantiation ─────────────────────────────────────────
    print("\n[10] questa_cosim_top.cc: questa_top instantiation")
    check(re.search(r'questa_top\s+qt\s*\(', cosim_text) is not None,
          "questa_top qt(...) instantiated in sc_main")
    check("MachineType_Cache" in cosim_text,
          "MachineType_Cache used for VIP node ID")
    check("MachineType_Directory" in cosim_text,
          "MachineType_Directory used for HN-F node ID")
    check("VIP_NODE_NUM" in cosim_text,
          "VIP_NODE_NUM macro for configurable node ID")
    check("HNF_NODE_NUM" in cosim_text,
          "HNF_NODE_NUM macro for configurable downstream ID")

    # ── [11] QUESTA_STANDALONE_TEST branch ────────────────────────────────────
    print("\n[11] questa_cosim_top.cc: standalone test branch")
    check("QUESTA_STANDALONE_TEST" in cosim_text,
          "QUESTA_STANDALONE_TEST compile-time flag present")
    check(re.search(r'sc_start\(', cosim_text) is not None,
          "sc_start() called in standalone test branch")
    check(re.search(r'req_lcrd_init_', cosim_text) is not None,
          "L-credit exhaustion check present")

# ── [12] Makefile build rule ────────────────────────────────────────────────
print("\n[12] Makefile: questa_cosim_test build rule")
if MAKEFILE.exists():
    mk = MAKEFILE.read_text()
    check("questa_cosim_test" in mk,
          "questa_cosim_test target in Makefile")
    check("questa_cosim_top.cc" in mk,
          "questa_cosim_top.cc listed as source")
    check("QUESTA_STANDALONE_TEST" in mk,
          "-DQUESTA_STANDALONE_TEST flag in Makefile build rule")
else:
    check(False, "Makefile not found")

# ── Summary ───────────────────────────────────────────────────────────────────
print(f"\n=== RESULTS: {pass_count} passed, {fail_count} failed ===")
sys.exit(1 if fail_count else 0)

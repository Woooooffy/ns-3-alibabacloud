import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# grammar.lark, transformer.py and ns3codegen.py live in the topology-dsl-frontend
# submodule; put it on the path before importing anything that reaches into it
# (ns3writer pulls in ns3codegen).
FRONTEND = os.path.join(HERE, "dsl-frontend")
sys.path.insert(0, FRONTEND)

from lark import Lark
from transformer import TopoTransformer
from ns3codegen import NS3CodeGenerator
from ns3writer import NS3Writer

GRAMMAR = os.path.join(FRONTEND, "grammar.lark")
EXAMPLES = os.path.join(FRONTEND, "examples")
OUTPUT_DIR = os.path.join(HERE, "output")

DEFAULT_TOPO = os.path.join(EXAMPLES, "two_pod_rail_hostbound.topo")


def main():
	ap = argparse.ArgumentParser(description="Generate an ns-3 .cc scenario from a .topo DSL file.")
	ap.add_argument("topo", nargs="?", default=DEFAULT_TOPO, help="input .topo file")
	ap.add_argument("-o", "--output", default=None, help="output .cc file (default: output/<topo>.cc)")
	ap.add_argument("-v", "--verbose", action="store_true", help="dump the parse tree and config")
	args = ap.parse_args()

	if not os.path.isfile(GRAMMAR):
		sys.exit(
			f"{GRAMMAR} is missing -- the dsl-frontend submodule is not checked out.\n"
			"Run: git submodule update --init topology/dsl-frontend"
		)

	out = args.output
	if out is None:
		stem = os.path.splitext(os.path.basename(args.topo))[0]
		out = os.path.join(OUTPUT_DIR, stem + ".cc")

	with open(GRAMMAR, "r") as f:
		grammar_text = f.read()

	parser = Lark(grammar_text, parser="lalr")

	with open(args.topo, "r") as f:
		topo_text = f.read()

	tree = parser.parse(topo_text)

	cfg = TopoTransformer().transform(tree)

	if args.verbose:
		print(tree.pretty())
		print(cfg)

	codegen = NS3CodeGenerator(cfg)
	codegen.Generate()

	if args.verbose:
		print(codegen.insns)
		print(codegen.link_helpers)

	os.makedirs(os.path.dirname(out), exist_ok=True)
	writer = NS3Writer(out, codegen)
	writer.write()
	print(f"Wrote {out}")


if __name__ == "__main__":
	main()

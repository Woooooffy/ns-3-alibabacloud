import argparse
import os

from lark import Lark
from transformer import TopoTransformer
from ns3codegen import NS3CodeGenerator
from ns3writer import NS3Writer

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_TOPO = os.path.join(HERE, "examples/two_pod_rail_hostbound.topo")
DEFAULT_OUT = os.path.join(HERE, "examples/output/two_pod_rail_hostbound.cc")


def main():
	ap = argparse.ArgumentParser(description="Generate an ns-3 .cc scenario from a .topo DSL file.")
	ap.add_argument("topo", nargs="?", default=DEFAULT_TOPO, help="input .topo file")
	ap.add_argument("-o", "--output", default=None, help="output .cc file (default: examples/output/<topo>.cc)")
	ap.add_argument("-v", "--verbose", action="store_true", help="dump the parse tree and config")
	args = ap.parse_args()

	out = args.output
	if out is None:
		if args.topo == DEFAULT_TOPO:
			out = DEFAULT_OUT
		else:
			stem = os.path.splitext(os.path.basename(args.topo))[0]
			out = os.path.join(HERE, "examples/output", stem + ".cc")

	with open(os.path.join(HERE, "grammar.lark"), "r") as f:
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

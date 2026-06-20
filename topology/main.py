from lark import Lark, Transformer
from transformer import TopoTransformer
from ns3codegen import NS3CodeGenerator 
from ns3writer import NS3Writer

with open("grammar.lark", "r") as f:
	grammar_text = f.read()

parser = Lark(grammar_text, parser="lalr")

with open("examples/3nodes.topo", "r") as f:
	topo_text = f.read()

tree = parser.parse(topo_text)

print(tree.pretty())

cfg = TopoTransformer().transform(tree)
print(cfg)

codegen = NS3CodeGenerator(cfg)
codegen.Generate()

print(codegen.insns)
print(codegen.link_helpers)

print("Code emission started")

writer = NS3Writer("examples/output/tmp.cc", codegen)
writer.write()

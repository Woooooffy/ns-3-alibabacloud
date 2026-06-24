from lark import Transformer
from typing import Optional, Any, Callable, TypedDict
import re
import operator

ops = {
    "==": operator.eq,
    "!=": operator.ne,
    "<":  operator.lt,
    "<=": operator.le,
    ">":  operator.gt,
    ">=": operator.ge,
}



'''
Scopes are not build until later step
Since transformer is bottom-up and cannot build parent scope info
Defined here for ease of importing
'''

class Scope():
	def __init__(self, parent: Optional["Scope"] = None):
		self.parent: "Scope" = parent
		self.name_to_val: dict[str, int] = {}
		self.node_name_prefix: str = ""
	
	def set_parent(self, parent: "Scope"):
		self.parent = parent

	def set_name_to_val(self, name: str, value: int) -> None:
		self.name_to_val[name] = value

	def lookup(self, name: str) -> int:
		name = self.resolve_name_with_var(name)
		tmp: int  = self.name_to_val.get(name)
		if tmp is not None:
			return tmp
		if self.parent is not None:
			return self.parent.lookup(name)
		raise RuntimeError(f"Unresolved name {name}.")
	
	def resolve_name_with_var(self, name_w_var: str) -> str:
		match = re.match(r"(.*?)\{(.*?)\}(.*)", name_w_var)
		if match:
			pre, var_str, post = match.groups()
			var = self.lookup(var_str)
			return pre + str(var) + post
		else:
			return name_w_var
	
	def get_node_name_prefix(self) -> str:
		if self.parent is not None:
			prefix = self.parent.get_node_name_prefix()
		else:
			prefix = ""
		my_prefix = self.resolve_name_with_var(self.node_name_prefix)
		if prefix != "" and my_prefix != "":
			return prefix + "_" + my_prefix
		return prefix + my_prefix
	
	def set_node_name_prefix(self, prefix: str) -> None:
		self.node_name_prefix = prefix

##### Arithmetic #####

class Expr():
	'''
	Lazily-evaluated arithmetic expression over NUMBERs, NAMEs (param/loop
	variables) and {var} references. Resolved against a Scope once the
	referenced names are in scope (e.g. inside a module/loop body).
	'''
	OPS: dict[str, Callable[[int, int], int]] = {
		"+": operator.add,
		"-": operator.sub,
		"*": operator.mul,
		"/": operator.floordiv,
	}

	def __init__(self, op: str, left: Any, right: Any):
		self.op: str = op
		self.left: Any = left
		self.right: Any = right

	def __repr__(self) -> str:
		return f"({self.left} {self.op} {self.right})"

	def evaluate(self, scope: "Scope") -> int:
		left = Expr.resolve(self.left, scope)
		right = Expr.resolve(self.right, scope)
		return Expr.OPS[self.op](left, right)

	@staticmethod
	def resolve(value: Any, scope: "Scope") -> int:
		if isinstance(value, Expr):
			return value.evaluate(scope)
		if isinstance(value, int):
			return value
		if isinstance(value, str):
			name = value[1:-1] if value.startswith("{") and value.endswith("}") else value
			return scope.lookup(name)
		raise RuntimeError(f"Cannot resolve {value!r} to an integer.")


##### Helpers #####

class Insn():
	def simple_print(self) -> str:
		pass

class NewNodeInsn(Insn):
	def __init__(self, name: str, node_type: str, attrs: Optional[dict[str, Any]] = None):
		self.name: str = name
		self.type: str = node_type
		self.attrs: dict[str, Any] = attrs or {}

	def __repr__(self) -> str:
		return f"NewNodeInsn{(self.name, self.type, self.attrs)}"
	
	def simple_print(self) -> str:
		return __repr__(self) + " "

class NewLinkInsn(Insn):
	def __init__(self, src: list[str], dst: list[str], **attrs: Any):
		self.src: list[str] = src
		self.dst: list[str] = dst
		self.attrs: dict[str, Any] = attrs

	def simple_print(self) -> str:
		return f"NewLinkInsn(src {self.src}, dst {self.dst}) "
	def __repr__(self) -> str:
		return f"NewLinkInsn(src {self.src}, dst {self.dst}, attrs {self.attrs})"

class RdmaConfigInsn(Insn):
	def __init__(self, attrs: dict[str, Any]):
		self.attrs: dict[str, Any] = attrs

	def __repr__(self) -> str:
		return f"RdmaConfigInsn({self.attrs})"

	def simple_print(self) -> str:
		return __repr__(self) + " "

class SubmoduleInsn(Insn):
	def __init__(self, name: str, submodule_name: str, *args: Any):
		self.name: str = name
		self.module_name: str = submodule_name
		self.args: Any = args

	def __repr__(self) -> str:
		return f"SubmoduleInsn({self.name, self.module_name}, args{self.args})"
	
	def simple_print(self) -> str:
		return __repr__(self) + " "

class IfInsn(Insn):
	def __init__(self, cond: list[Any], true_block: "Block"):
		self.cond = cond
		self.true_block = true_block
	def __repr__(self) -> str:
		return f"If(condition{self.cond}, true{self.true_block})"

class LoopInsn(Insn):
	def __init__(self, iterator_name: str, start: Any, end: Any, body: "Block"):
		self.iterator_name = iterator_name
		self.start = start
		self.end = end
		self.body: "Block" = body

	def __repr__(self) -> str:
		return f"Loop(iter {self.iterator_name}, start {self.start}, end {self.end}, body {self.body.simple_print()})"
	
class Block():
	_id_counter = 0

	def __init__(self, params: Optional[list[str]] = None, insns: Optional[list[Insn]] = None):
		self.id: int = Block._id_counter
		Block._id_counter += 1
		self.params: list[str] = params or []
		self.insns: list[Insn] = insns or []
		self.scope: Scope = Scope() 
		# transformer only initalize empty scopes as placeholder
		# scopes should be built individually by later top-down stages
	
	def get_scope(self) -> Scope:
		return self.scope

	def add_insn(self, insn: Insn) -> None:
		self.insns.append(insn)

	def __repr__(self) -> str:
		return f"Block(id={self.id}, insns={self.insns})"
		# return f"Block(id={self.id}, insns=[{i.simple_print() for i in self.insns}])"
	
	def simple_print(self) -> str: # prevent recursive printing
		return f"Block(id={self.id})"

##### Transformer #####

class TopoTransformer(Transformer):
	def __init__(self):
		super().__init__()
		self._name_to_module: dict[str, Block] = {}	


	def start(self, modules: Any) -> dict[str, Block]:
		return self._name_to_module

	def module(self, items) -> None:
		name = str(items[0])	
		params = items[1]
		insns = items[2:]
		block = Block(params=params, insns=insns)
		self._name_to_module[name] = block
		
	def params(self, items) -> list[str]:
		return [str(item) for item in items]	

	def stmt(self, stmts) -> Insn:
		return stmts[0]

	def	node_stmt(self, items) -> Insn:
		name = str(items[0])
		attrs = dict(items[1:])
		node_type = attrs.get("type", "default")
		return NewNodeInsn(name, node_type, attrs)


	def link_stmt(self, items) -> Insn:
		src_name = items[0]
		dst_name = items[1]	
		attrs = dict(items[2:])
		return NewLinkInsn(src_name, dst_name, **attrs)

	def rdma_stmt(self, items) -> Insn:
		return RdmaConfigInsn(dict(items))

	def use_stmt(self, items) -> Insn:
		submodule_name = items[0]
		args = items[1]
		name = items[2]
		if args is None:
			args = []
		return SubmoduleInsn(name, submodule_name, *args)
	
	def for_stmt(self, items) -> Insn:	
		iter_name = items[0]
		start = items[1]
		end = items[2]
		body_insn_list = items[3:]
		return LoopInsn(iter_name, start, end, Block(params=[iter_name], insns=body_insn_list))

	def if_stmt(self, items) -> Insn:
		cond = items[0]
		true_block = Block(insns=items[1:])
		return IfInsn(cond, true_block)

	def condition(self, items):
		op_str = items[1]
		if op_str not in ops:
			raise ValueError(f"Invalid operator: {op_str}")
		items[1] = ops[op_str]
		return items

	def name(self, items):
		s = str(items[0])
		if len(items) > 1:
			s += str(items[1])
		return s

	def attr(self, items):
		return (str(items[0]), items[1])
	
	def ref(self, items) -> list[str]:
		return items

	def args(self, items) -> list[Any]:
		return items

	def value(self, items):
		if len(items) == 2:
			# NUMBER + UNIT
			number = int(items[0])
			unit = str(items[1])
			return (number, unit)
		else:
			# result of expr: int, str (NAME/var), or Expr
			return items[0]
	
	def expr(self, items):
		return self._fold_binop(items)

	def term(self, items):
		return self._fold_binop(items)

	def atom(self, items):
		token = items[0]
		if isinstance(token, Expr) or type(token) is str:
			# Expr from "(" expr ")", or plain str from var ("{NAME}")
			return token
		if token.type == "NUMBER":
			return int(token)
		return str(token)

	def _fold_binop(self, items):
		result = items[0]
		i = 1
		while i < len(items):
			op = str(items[i])
			rhs = items[i + 1]
			result = Expr(op, result, rhs)
			i += 2
		return result

	def template_suffix(self, items):
		return items[0]
	
	def var(self, items) -> str:
		return "{" + items[0] + "}"

from typing import Optional, Any, Callable, TypedDict
from transformer import *

class NS3Insn():
	pass

class NS3MakeGPUs(NS3Insn):
	def __init__(self, n: int):
		self.n_gpus: int = n

	def __repr__(self) -> str:
		return f"Create {self.n_gpus} NS3 GPUs"

class NS3MakeRegSwitches(NS3Insn):
	def __init__(self, n: int):
		self.n_switches: int = n

	def __repr__(self) -> str:
		return f"Create {self.n_switches} NS3 RDMA switches (SwitchNode)"

class NS3MakeNVSwitches(NS3Insn):
	def __init__(self, n: int):
		self.n_nvswitches: int = n

	def __repr__(self) -> str:
		return f"Create {self.n_nvswitches} NS3 NVSwitches (NVSwitchNode)"

class NS3MakeLinkHelper(NS3Insn):
	def __init__(self, id: int, **attrs: Any):
		self.id = id
		self.delay = attrs["latency"]
		self.data_rate = attrs["bandwidth"]
		if "mtu" in attrs:
			self.mtu = attrs["mtu"]
		else:
			self.mtu = 9000
		self.type = attrs["type"] # "p2p" (gpu<->gpu) or "qbb" (anything touching a switch/nvswitch)

	def __repr__(self) -> str:
		return f"Create NS3 link helper {self.id}: latency {self.delay} bandwidth {self.data_rate} type {self.type}"

class NS3InstallLink(NS3Insn):
	def __init__(self, src: str, dst: str, helper_id: int, link_id: int):
		self.src: str = src
		self.dst: str = dst
		self.link_helper: int = helper_id
		# stable id naming this link's NetDeviceContainer (devs{helper_id}_{link_id});
		# assigned here (rather than by the writer) so the RDMA fabric routing pass
		# below can reference a link's devices before the writer ever runs
		self.link_id: int = link_id

	def __repr__(self) -> str:
		return f"Install NS3 link {self.src} -> {self.dst} with helper {self.link_helper} (link_id {self.link_id})"

class NS3BuildRdmaFabric(NS3Insn):
	'''
	Triggers RdmaFabricHelper::Build at simulation setup time. Unlike the old
	NS3InstallRdmaFabric, this carries no precomputed routing/IP/BDP data --
	BFS-ECMP routing, IP assignment, and MMU/PFC config all run in C++ at
	ns-3 runtime (RdmaFabricHelper discovers the qbb link graph from already
	-installed NetDevices/Channels), so the generated code stays a handful of
	lines regardless of topology size. Only DSL-level intent that the C++
	can't infer from topology alone is carried here.
	'''
	def __init__(self, rdma_attrs: dict[str, int]):
		# uniform RdmaHw attributes (e.g. L2AckInterval, Mtu, CcMode) from `rdma`
		# DSL statements, applied via Config::SetDefault before Build() creates
		# any RdmaHw instances
		self.rdma_attrs: dict[str, int] = rdma_attrs

	def __repr__(self) -> str:
		return f"Build RDMA fabric"

class NS3CodeGenerator():
	def __init__(self, modules: dict[str, Block]):
		self.gpus: dict[str, int] = {}
		self.switches: dict[str, int] = {}   # regular RDMA switches -> SwitchNode
		self.nvswitches: dict[str, int] = {} # NVLink-style fabric -> NVSwitchNode
		self.insns: list[NS3Insn] = []
		self.modules: dict[str, Block] = modules
		self.gpu_counter: int = 0
		self.switch_counter: int = 0
		self.nvswitch_counter: int = 0
		self.link_helpers: dict[tuple[Any], int] = {}
		self.link_helper_counter = 0
		self.link_id_counter = 0
		# set once any qbb (gpu<->switch / switch<->switch / gpu<->nvswitch)
		# link is created; gates whether an NS3BuildRdmaFabric insn is emitted
		# at all (a pure-p2p topology has no RDMA fabric to wire up)
		self.has_qbb_fabric: bool = False
		# uniform RdmaHw attributes (e.g. L2AckInterval, Mtu, CcMode) from `rdma`
		# DSL statements, applied to every GPU's RdmaHw instance; later
		# occurrences override earlier ones
		self.rdma_attrs: dict[str, int] = {}

	def Generate(self) -> None:
		self.GenerateModule(self.modules["main"])
		insns = [
			NS3MakeGPUs(self.gpu_counter),
			NS3MakeRegSwitches(self.switch_counter),
			NS3MakeNVSwitches(self.nvswitch_counter),
		]
		for tup, id in self.link_helpers.items():
			args = {"latency": tup[0], "bandwidth": tup[1], "mtu": tup[2], "type": tup[3]}
			insns.append(NS3MakeLinkHelper(id, **args))
		self.insns = insns + self.insns
		if self.has_qbb_fabric:
			self.insns.append(NS3BuildRdmaFabric(self.rdma_attrs))

	def GenerateModule(self, module: Block, *args: Any) -> None:
		scope = module.get_scope()
		if len(args) != len(module.params):
			raise RuntimeError(f"Arguments and parameters length mismatch for module {module}.\n Expected: {len(module.params)}, passed: {len(args)}.")
		for i in range(len(args)):
			scope.set_name_to_val(module.params[i], args[i])
		for insn in module.insns:
			self.GenerateInsn(scope, insn, *args)

	def GenerateInsn(self, this_scope: Scope, insn: Insn, *args: Any) -> None:
		match insn:
			case NewNodeInsn():
				return self.GenNewNode(this_scope, insn, *args)
			case NewLinkInsn():
				return self.GenNewLink(this_scope, insn, *args)
			case SubmoduleInsn():
				return self.GenSubmodule(this_scope, insn, *args)
			case IfInsn():
				return self.GenIf(this_scope, insn, *args)
			case LoopInsn():
				return self.GenLoop(this_scope, insn, *args)
			case RdmaConfigInsn():
				return self.GenRdmaConfig(this_scope, insn, *args)
			case _:
				raise RuntimeError(f"Unrecognized instruction {insn}.")

	def GenNewNode(self, this_scope: Scope, insn: NewNodeInsn, *args: Any):
		# add this to list of node to be built
		# don't individually create nodes in NS3
		prefix = this_scope.get_node_name_prefix()
		name = this_scope.resolve_name_with_var(insn.name)
		type = insn.type
		if prefix != "":
			name = prefix + "_" + name
		match type:
			case "gpu":
				self.gpus[name] = self.gpu_counter
				self.gpu_counter += 1
			case "switch":
				self.switches[name] = self.switch_counter
				self.switch_counter += 1
			case "nvswitch":
				self.nvswitches[name] = self.nvswitch_counter
				self.nvswitch_counter += 1
			case _:
				raise RuntimeError(f"Unrecognized node type {type}")

	def GenNewLink(self, this_scope: Scope, insn: NewLinkInsn, *args: Any):
		src = this_scope.resolve_name_with_var(insn.src[0])
		dst = this_scope.resolve_name_with_var(insn.dst[0])
		for i in range(1, len(insn.src)):
			src += "_" + this_scope.resolve_name_with_var(insn.src[i])
		for i in range(1, len(insn.dst)):
			dst += "_" + this_scope.resolve_name_with_var(insn.dst[i])
		pre = this_scope.get_node_name_prefix()
		if pre != "":
			src = pre + "_" + src
			dst = pre + "_" + dst
		# assumes nodes declared before building link
		# gpu<->gpu is a direct point-to-point link; anything touching a switch or
		# nvswitch goes over the RDMA/QBB fabric instead
		if src in self.gpus and dst in self.gpus:
			type = "p2p"
		else:
			type = "qbb"
		mtu = insn.attrs["mtu"] if "mtu" in insn.attrs else 9000
		attr = (insn.attrs["latency"], insn.attrs["bandwidth"], mtu, type)
		helper = self.link_helpers.get(attr)
		if helper is None:
			# add this to list of helpers to be built
			# build all at once for readability
			self.link_helpers[attr] = self.link_helper_counter
			self.link_helper_counter += 1
		link_id = self.link_id_counter
		self.link_id_counter += 1
		helper_id = self.link_helpers[attr]
		self.insns.append(NS3InstallLink(src, dst, helper_id, link_id))
		if type == "qbb":
			self.has_qbb_fabric = True

	def GenSubmodule(self, parent_scope: Scope, insn: SubmoduleInsn, *args: Any):
		module = self.modules.get(insn.module_name)
		if not module:
			raise RuntimeError("Module " + insn.module_name + " not defined.")
		scope = module.get_scope()
		scope.set_parent(parent_scope)
		scope.set_node_name_prefix(insn.name)
		resolved_args = [Expr.resolve(a, parent_scope) for a in insn.args]
		self.GenerateModule(module, *resolved_args)

	def GenIf(self, parent_scope: Scope, insn: IfInsn, *args: Any):
		cond: list[Any] = insn.cond
		true_block: Block = insn.true_block
		if (type(cond[0]) == tuple or type(cond[2]) == tuple):
			raise RuntimeError("Comparisons with units not yet supported.")
		left: int = Expr.resolve(cond[0], parent_scope)
		right: int = Expr.resolve(cond[2], parent_scope)
		evaluated: bool = cond[1](left, right)
		if not evaluated:
			return
		scope = true_block.get_scope()
		scope.set_parent(parent_scope)
		self.GenerateModule(true_block)

	def GenLoop(self, parent_scope: Scope, insn: LoopInsn, *args: Any):
		itername = insn.iterator_name
		start = Expr.resolve(insn.start, parent_scope)
		end = Expr.resolve(insn.end, parent_scope)
		loop_block = insn.body
		scope = loop_block.get_scope()
		scope.set_parent(parent_scope)
		for i in range(start, end + 1):
			self.GenerateModule(loop_block, i)

	def GenRdmaConfig(self, this_scope: Scope, insn: RdmaConfigInsn, *args: Any):
		for name, value in insn.attrs.items():
			self.rdma_attrs[name] = Expr.resolve(value, this_scope)

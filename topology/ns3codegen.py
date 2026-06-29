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
<<<<<<< HEAD
	def __init__(self):
		self.gpu_ip: dict[str, str] = {}
		self.gpus_per_server: int = 1
		# uniform RdmaHw attributes (e.g. L2AckInterval, Mtu, CcMode), applied to
		# every GPU's RdmaHw instance via SetAttribute
		self.rdma_attrs: dict[str, int] = {}
		# gpu_name -> [(container_expr, my_endpoint_idx)] for every qbb NIC the gpu
		# owns, regardless of whether it ends up on any shortest path (it still
		# needs an Ipv4 address and an RdmaHw-managed QbbNetDevice either way)
		self.gpu_links: dict[str, list[tuple[str, int]]] = {}
		# node_name -> [(dst_gpu_ip, container_expr, my_endpoint_idx)]
		self.switch_routes: dict[str, list[tuple[str, str, int]]] = {}
		self.nvswitch_routes: dict[str, list[tuple[str, str, int]]] = {}
		# gpu_name -> [(dst_gpu_ip, container_expr, my_endpoint_idx, is_nvswitch)]
		self.gpu_rdma_routes: dict[str, list[tuple[str, str, int, bool]]] = {}
		# gpu_name -> {peer_gpu_name: peer_ip}
		self.gpu_peer_ip: dict[str, dict[str, str]] = {}
		# gpu_name -> {peer_gpu_name: bandwidth-delay-product window in bytes}
		self.gpu_peer_win: dict[str, dict[str, int]] = {}
		# gpu_name -> {peer_gpu_name: base RTT in ns}
		self.gpu_peer_rtt: dict[str, dict[str, int]] = {}
		# switch_name -> [(flow_id, container_expr, my_endpoint_idx)], populated
		# separately once a switch opts into custom flow forwarding (codegen step 8)
		self.flow_rules: dict[str, list[tuple[int, str, int]]] = {}
		# switch_name/nvswitch_name -> [(container_expr, my_endpoint_idx, headroom_bytes,
		# kmin_KB, kmax_KB, pmax, pfc_alpha_shift)], one entry per qbb port on that switch
		self.mmu_config: dict[str, list[tuple[str, int, int, int, int, float, int]]] = {}
=======
	def __init__(self, rdma_attrs: dict[str, int]):
		# uniform RdmaHw attributes (e.g. L2AckInterval, Mtu, CcMode) from `rdma`
		# DSL statements, applied via Config::SetDefault before Build() creates
		# any RdmaHw instances
		self.rdma_attrs: dict[str, int] = rdma_attrs
>>>>>>> file_sync

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
<<<<<<< HEAD
		# adjacency over the qbb (RDMA-fabric) subgraph only -- gpu<->gpu p2p links
		# are intentionally absent here, they need no routing table at all.
		# node_name -> [(peer_name, container_expr, my_endpoint_idx)]
		self.qbb_adj: dict[str, list[tuple[str, str, int]]] = {}
		# container_expr -> (latency, bandwidth, mtu, type) attrs of that qbb link,
		# so path RTT/bandwidth can be derived from the same adjacency used for routing
		self.qbb_link_attrs: dict[str, tuple] = {}
		# regular switches with `flowid_forwarding=true` in the DSL: get an
		# explicit AddFlowForwardingRule per (src gpu, dst gpu) pair that crosses
		# them, instead of relying purely on ECMP
		self.flow_forwarding_switches: set[str] = set()
=======
		# set once any qbb (gpu<->switch / switch<->switch / gpu<->nvswitch)
		# link is created; gates whether an NS3BuildRdmaFabric insn is emitted
		# at all (a pure-p2p topology has no RDMA fabric to wire up)
		self.has_qbb_fabric: bool = False
>>>>>>> file_sync
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
		if "mtu" in insn.attrs:
			mtu = insn.attrs["mtu"]
		elif type == "qbb":
			# qbb links carry RdmaHw-chunked packets (RdmaHw::GetNxtPacket caps
			# payload at RdmaHw's own Mtu, independent of the device's L2 Mtu),
			# so default this link's device Mtu to match -- avoids a device Mtu
			# that silently disagrees with the chunk size RdmaHw actually sends.
			# Order-dependent: only sees `rdma` statements textually before this
			# link (module.insns are walked in source order); declare `rdma`
			# before any qbb links if you rely on this default.
			mtu = self.rdma_attrs.get("Mtu", 9000)
		else:
			mtu = 9000
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
<<<<<<< HEAD
			# must match the container variable name the writer emits in
			# _emit_link_install (devs{helper_id}_{link_id})
			container_expr = f"devs{helper_id}_{link_id}"
			self.qbb_adj.setdefault(src, []).append((dst, container_expr, 0))
			self.qbb_adj.setdefault(dst, []).append((src, container_expr, 1))
			self.qbb_link_attrs[container_expr] = attr
=======
			self.has_qbb_fabric = True
>>>>>>> file_sync

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
<<<<<<< HEAD

	# --------------------------------------------------
	# RDMA fabric routing (BFS-ECMP)
	# --------------------------------------------------

	def _bfs_dist(self, source: str) -> dict[str, int]:
		'''Shortest-hop distance from source to every node reachable over the qbb subgraph.'''
		dist = {source: 0}
		q = deque([source])
		while q:
			cur = q.popleft()
			for peer, _container_expr, _end in self.qbb_adj.get(cur, []):
				if peer not in dist:
					dist[peer] = dist[cur] + 1
					q.append(peer)
		return dist

	@staticmethod
	def _delay_to_ns(delay: tuple[float, str]) -> float:
		value, unit = delay
		factor = {"ns": 1.0, "us": 1e3, "ms": 1e6}[unit]
		return float(value) * factor

	@staticmethod
	def _rate_to_bps(rate: tuple[float, str]) -> float:
		value, unit = rate
		factor = {
			"Gbps": 1e9, "Mbps": 1e6, "Kbps": 1e3,
			"GBps": 8e9, "MBps": 8e6, "KBps": 8e3,
		}[unit]
		return float(value) * factor

	def _bfs_path_metrics(self, source: str) -> dict[str, tuple[int, float, float, float]]:
		'''
		Walk the same shortest-hop-count BFS tree as _bfs_dist (so this matches the
		ECMP routing tables actually installed), but also accumulate, per reachable
		node: (hop count, one-way propagation delay in ns, one-way transmission delay
		for an MTU-size packet in ns, bottleneck/min-hop bandwidth in bps). Used to
		derive a bandwidth-delay-product window and base RTT per GPU pair, mirroring
		astra-sim's pairBdp/pairRtt (rtt = delay*2 + txDelay, bdp = rtt*bw/1e9/8).
		'''
		metrics: dict[str, tuple[int, float, float, float]] = {source: (0, 0.0, 0.0, float("inf"))}
		q = deque([source])
		while q:
			cur = q.popleft()
			cur_hops, cur_delay_ns, cur_tx_ns, cur_bw_bps = metrics[cur]
			for peer, container_expr, _end in self.qbb_adj.get(cur, []):
				if peer in metrics:
					continue
				latency, bandwidth, mtu, _type = self.qbb_link_attrs[container_expr]
				delay_ns = self._delay_to_ns(latency)
				bw_bps = self._rate_to_bps(bandwidth)
				tx_ns = mtu * 8 / bw_bps * 1e9
				metrics[peer] = (cur_hops + 1, cur_delay_ns + delay_ns, cur_tx_ns + tx_ns, min(cur_bw_bps, bw_bps))
				q.append(peer)
		return metrics

	def _build_switch_mmu_config(self) -> dict[str, list[tuple[str, int, int, int, int, float, int]]]:
		'''
		Per-switch/nvswitch PFC headroom + ECN threshold config, one entry per qbb
		port, mirroring astra-sim's common.h switch-config loop (ConfigEcn/ConfigHdrm/
		pfc_a_shift). SwitchMmu's headroom[]/kmin[]/kmax[]/pmax[]/pfc_a_shift[] arrays
		are otherwise left completely uninitialized -- that silently disables realistic
		PFC backpressure under incast, and pfc_a_shift[port] in particular is read by
		GetPfcThreshold's bit-shift, so leaving it uninitialized is undefined behavior.
		'''
		if not self.qbb_link_attrs:
			return {}
		baseline_rate_bps = min(self._rate_to_bps(attrs[1]) for attrs in self.qbb_link_attrs.values())
		config: dict[str, list[tuple[str, int, int, int, int, float, int]]] = {}
		for name in list(self.switches) + list(self.nvswitches):
			ports = []
			for _peer, container_expr, end in self.qbb_adj.get(name, []):
				latency, bandwidth, _mtu, _type = self.qbb_link_attrs[container_expr]
				delay_s = self._delay_to_ns(latency) / 1e9
				rate_bps = self._rate_to_bps(bandwidth)
				headroom_bytes = round(rate_bps * delay_s * 3 / 8)  # 3 link-RTTs of headroom (astra-sim default)
				kmin_bytes = rate_bps / 8 * 4e-6  # ~4us of queueing before ECN starts marking
				kmax_bytes = kmin_bytes * 2
				shift = 3  # 1/8 of remaining buffer, astra-sim default
				rate = rate_bps
				while rate > baseline_rate_bps and shift > 0:
					shift -= 1
					rate /= 2
				ports.append((container_expr, end, headroom_bytes, round(kmin_bytes / 1000), round(kmax_bytes / 1000), 0.2, shift))
			if ports:
				config[name] = ports
		return config

	def _build_rdma_fabric(self) -> NS3InstallRdmaFabric:
		fabric = NS3InstallRdmaFabric()
		fabric.rdma_attrs = self.rdma_attrs
		base_ip = ipaddress.IPv4Address("10.0.0.1")
		for name, idx in sorted(self.gpus.items(), key=lambda kv: kv[1]):
			fabric.gpu_ip[name] = str(base_ip + idx)
			fabric.gpu_links[name] = [(container_expr, end) for (_peer, container_expr, end) in self.qbb_adj.get(name, [])]

		if self.nvswitches:
			# perf hint only: RdmaHw's actual routing is always governed by the
			# explicit is_nvswitch table entries below, regardless of this value
			first_nvsw = next(iter(self.nvswitches))
			attached_gpus = sum(1 for peer, _, _ in self.qbb_adj.get(first_nvsw, []) if peer in self.gpus)
			fabric.gpus_per_server = attached_gpus if attached_gpus > 0 else 1

		name_to_type: dict[str, str] = {}
		for n in self.gpus: name_to_type[n] = "gpu"
		for n in self.switches: name_to_type[n] = "switch"
		for n in self.nvswitches: name_to_type[n] = "nvswitch"

		# GPUs are always leaves of the qbb subgraph (gpu<->gpu never goes over qbb),
		# so BFS run from each destination GPU naturally never routes traffic through
		# an unrelated GPU -- every node's neighbors-at-distance-1 are exactly its
		# valid ECMP next hops toward that destination.
		for dst_name in self.gpus:
			metrics = self._bfs_path_metrics(dst_name)
			dist = {node: m[0] for node, m in metrics.items()}
			dst_ip = fabric.gpu_ip[dst_name]
			for node_name, node_links in self.qbb_adj.items():
				if node_name == dst_name or node_name not in dist:
					continue
				my_dist = dist[node_name]
				next_hops = [(peer, container_expr, end) for (peer, container_expr, end) in node_links
				             if peer in dist and dist[peer] == my_dist - 1]
				if not next_hops:
					continue
				ntype = name_to_type[node_name]
				if ntype == "switch":
					fabric.switch_routes.setdefault(node_name, []).extend(
						(dst_ip, container_expr, end) for (_, container_expr, end) in next_hops)
					if node_name in self.flow_forwarding_switches:
						# routing here is purely destination-based (same next hop
						# regardless of source), so any one of the ECMP next hops
						# is a valid, deterministic per-flow assignment
						_, fwd_container, fwd_end = next_hops[0]
						for src_name in self.gpus:
							if src_name == dst_name:
								continue
							flow_id = (self.gpus[src_name] << 16) | self.gpus[dst_name]
							fabric.flow_rules.setdefault(node_name, []).append(
								(flow_id, fwd_container, fwd_end))
				elif ntype == "nvswitch":
					fabric.nvswitch_routes.setdefault(node_name, []).extend(
						(dst_ip, container_expr, end) for (_, container_expr, end) in next_hops)
				else: # gpu
					fabric.gpu_peer_ip.setdefault(node_name, {})[dst_name] = dst_ip
					_, delay_ns, tx_ns, bw_bps = metrics[node_name]
					rtt_ns = delay_ns * 2 + tx_ns
					bdp_bytes = bw_bps * rtt_ns / 1e9 / 8
					fabric.gpu_peer_rtt.setdefault(node_name, {})[dst_name] = round(rtt_ns)
					fabric.gpu_peer_win.setdefault(node_name, {})[dst_name] = max(1, round(bdp_bytes))
					for (peer, container_expr, end) in next_hops:
						is_nvswitch = name_to_type[peer] == "nvswitch"
						fabric.gpu_rdma_routes.setdefault(node_name, []).append(
							(dst_ip, container_expr, end, is_nvswitch))
		fabric.mmu_config = self._build_switch_mmu_config()
		return fabric
=======
>>>>>>> file_sync

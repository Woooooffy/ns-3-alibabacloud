from ns3codegen import *

from typing import List

class NS3Writer:
	def __init__(self, filename, codegen):
		self.filename = filename
		self.gpus = codegen.gpus		   # dict[str, int]
		self.switches = codegen.switches  # dict[str, int] -- SwitchNode
		self.nvswitches = codegen.nvswitches # dict[str, int] -- NVSwitchNode
		self.link_helpers = codegen.link_helpers # dict[tuple, int]
		self.insns = codegen.insns

		self.lines = []
		self.indent = 0

		self.container_map = {} # (src_name, dst_name) -> container_expr

	def emit(self, line=""):
		self.lines.append("    " * self.indent + line)

	def write(self):
		self._emit_headers()
		self._emit_main_start()

		for insn in self.insns:
			self._handle_insn(insn)

		self._emit_gpu_setup()
		self._emit_main_end()

		with open(self.filename, "w") as f:
			f.write("\n".join(self.lines))

	# --------------------------------------------------
	# Headers + main
	# --------------------------------------------------

	def _emit_headers(self):
		self.emit('#include "ns3/core-module.h"')
		self.emit('#include "ns3/network-module.h"')
		self.emit('#include "ns3/internet-module.h"')
		self.emit('#include "ns3/point-to-point-module.h"')
		self.emit('#include "ns3/distributed-ml-module.h"')
		self.emit("")
		self.emit("#include <vector>")
		self.emit("")
		self.emit("using namespace ns3;")
		self.emit("")

	def _emit_main_start(self):
		self.emit("int main(int argc, char *argv[]) {")
		self.indent += 1

		self.emit("NodeContainer gpunodes;")
		self.emit("NodeContainer regswtches;")
		self.emit("NodeContainer nvswtches;")
		self.emit("")
		self.emit("// PFC backpressure (CheckAndSendPfc) runs unconditionally in SwitchNode, but only")
		self.emit("// has an effect once QcnEnabled lets a stalled NIC's queue resume; ECN marking is")
		self.emit("// separately gated per-switch by the EcnEnabled attribute set below.")
		self.emit('Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(true));')
		self.emit("")

	def _emit_main_end(self):
		self.emit("")
		self.emit("Simulator::Run();")
		self.emit("Simulator::Destroy();")
		self.emit("return 0;")
		self.indent -= 1
		self.emit("}")

	# --------------------------------------------------
	# Instruction handlers
	# --------------------------------------------------

	def _handle_insn(self, insn):
		if insn.__class__.__name__ == "NS3MakeGPUs":
			self._emit_make_typed_nodes("gpunodes", "GPU", insn.n_gpus)

		elif insn.__class__.__name__ == "NS3MakeRegSwitches":
			self._emit_make_typed_nodes("regswtches", "SwitchNode", insn.n_switches)

		elif insn.__class__.__name__ == "NS3MakeNVSwitches":
			self._emit_make_typed_nodes("nvswtches", "NVSwitchNode", insn.n_nvswitches)

		elif insn.__class__.__name__ == "NS3MakeLinkHelper":
			self._emit_link_helper(insn)

		elif insn.__class__.__name__ == "NS3InstallLink":
			self._emit_link_install(insn)

		elif insn.__class__.__name__ == "NS3InstallRdmaFabric":
			self._emit_rdma_fabric(insn)

		else:
			raise ValueError(f"Unknown instruction {insn}")

	def _emit_make_typed_nodes(self, container_name, cpp_type, n):
		# NodeContainer has no templated Create<T>(); build n nodes of the given
		# subclass directly and append them
		if n <= 0:
			return
		self.emit(f"for (uint32_t i = 0; i < {n}; ++i) {{ {container_name}.Add(CreateObject<{cpp_type}>()); }}")

	# --------------------------------------------------
	# Link helpers
	# --------------------------------------------------

	def _emit_link_helper(self, insn):
		hid = insn.id
		delay_val, delay_unit = insn.delay
		bw_val, bw_unit = insn.data_rate

		match insn.type:
			case "p2p":
				helper_name = "PointToPoint"
			case "qbb":
				helper_name = "Qbb"
			case _:
				raise RuntimeError(f"Unrecognized link type {insn.type}.")

		self.emit(f"{helper_name}Helper link_helper{hid};")
		self.emit(f"link_helper{hid}.SetDeviceAttribute(\"Mtu\", UintegerValue({insn.mtu}));")
		self.emit(f'link_helper{hid}.SetChannelAttribute("Delay", StringValue("{delay_val}{delay_unit}"));')
		# both PointToPointNetDevice and QbbNetDevice expose DataRate as a device
		# attribute (unlike e.g. Csma, which uses a channel attribute)
		self.emit(f'link_helper{hid}.SetDeviceAttribute("DataRate", StringValue("{bw_val}{bw_unit}"));')
		self.emit("")

	# --------------------------------------------------
	# Link installation
	# --------------------------------------------------

	def _resolve_node(self, name):
		if name in self.gpus:
			return f"gpunodes.Get({self.gpus[name]})", "gpu"
		elif name in self.switches:
			return f"regswtches.Get({self.switches[name]})", "switch"
		elif name in self.nvswitches:
			return f"nvswtches.Get({self.nvswitches[name]})", "nvswitch"
		else:
			raise ValueError(f"Unknown node {name}")

	def _emit_link_install(self, insn):
		src_expr, src_type = self._resolve_node(insn.src)
		dst_expr, dst_type = self._resolve_node(insn.dst)

		hid = insn.link_helper
		# must match the naming the codegen's routing pass already assumed
		# (NS3CodeGenerator.GenNewLink)
		container_expr = f"devs{hid}_{insn.link_id}"

		self.emit(
			f"NetDeviceContainer {container_expr} = "
			f"link_helper{hid}.Install({src_expr}, {dst_expr});"
		)
		self.emit("")
		self.container_map[(insn.src, insn.dst)] = container_expr

		if src_type == "gpu" and dst_type == "gpu":
			# direct p2p link between two GPUs: wire the existing socket-based
			# transport (PushSendPeerDevice/PushRecvPeerDevice/PushPeerAddr).
			# Everything touching a switch or nvswitch instead gets wired up later,
			# in _emit_rdma_fabric, once routing has been computed.
			self._emit_push_send_device(src_expr, insn.dst, f"{container_expr}.Get(0)")
			self._emit_push_recv_device(dst_expr, insn.src, f"{container_expr}.Get(1)")
			# p2p links are full-duplex, so the same pair of devices also
			# serves the reverse direction (dst -> src)
			self._emit_push_send_device(dst_expr, insn.src, f"{container_expr}.Get(1)")
			self._emit_push_recv_device(src_expr, insn.dst, f"{container_expr}.Get(0)")

			self.emit(
				f"DynamicCast<GPU>({src_expr})->PushPeerAddr("
				f"{self.gpus[insn.dst]}, ({container_expr}.Get(1))->GetAddress());"
			)
			self.emit(
				f"DynamicCast<GPU>({dst_expr})->PushPeerAddr("
				f"{self.gpus[insn.src]}, ({container_expr}.Get(0))->GetAddress());"
			)
			self.emit("")

	def _emit_push_send_device(self, src_expr, dst_name, dev_expr):
		self.emit(f"DynamicCast<GPU>({src_expr})->PushSendPeerDevice({self.gpus[dst_name]}, {dev_expr});")

	def _emit_push_recv_device(self, dst_expr, src_name, dev_expr):
		self.emit(f"DynamicCast<GPU>({dst_expr})->PushRecvPeerDevice({self.gpus[src_name]}, {dev_expr});")

	# --------------------------------------------------
	# RDMA fabric: addressing, switch/nvswitch routing tables, RdmaHw/RdmaDriver
	# --------------------------------------------------

	def _emit_rdma_fabric(self, insn):
		if not any(insn.gpu_links.values()):
			return # pure p2p topology, no RDMA fabric to wire up

		self.emit("// ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----")
		self.emit("InternetStackHelper internetStack;")
		self.emit("internetStack.Install(gpunodes);")
		self.emit("")

		self._emit_ip_assignment(insn)
		self._emit_routing_table(insn.switch_routes, self.switches, "regswtches", "SwitchNode",
			"SwitchNode routing tables (BFS ECMP)")
		self._emit_routing_table(insn.nvswitch_routes, self.nvswitches, "nvswtches", "NVSwitchNode",
			"NVSwitchNode routing tables (BFS ECMP)")
		self._emit_rdma_hw_setup(insn)
		self._emit_peer_ip_bookkeeping(insn)
		self._emit_peer_pacing(insn)
		self._emit_mmu_config(insn)
		self._emit_flow_rules(insn)

	def _emit_data_loop(self, struct_decls, vector_decl, entries, loop_var, loop_body):
		'''
		Emit `{ <struct decls>; std::vector<...> <vector_decl> = { <entries> }; for (auto& <loop_var> : ...) { <loop_body> } }`.
		The shared shape behind every block below: pack the per-port/per-gpu/per-switch
		data that scales with topology size into a vector literal (one line each, no
		repeated API calls), then run the actual setup logic once in a loop body whose
		size no longer grows with the topology.
		'''
		self.emit("{")
		self.indent += 1
		for decl in struct_decls:
			self.emit(decl)
		self.emit(f"{vector_decl} = {{")
		self.indent += 1
		for entry in entries:
			self.emit(f"{entry},")
		self.indent -= 1
		self.emit("};")
		self.emit(f"for (auto& {loop_var} : {vector_decl.split()[-1]}) {{")
		self.indent += 1
		for line in loop_body:
			self.emit(line)
		self.indent -= 1
		self.emit("}")
		self.indent -= 1
		self.emit("}")
		self.emit("")

	def _emit_ip_assignment(self, insn):
		# every qbb NIC a gpu owns gets the SAME identity address (the RdmaHw
		# fabric is multi-rail/ECMP capable)
		entries = []
		for name, links in insn.gpu_links.items():
			if not links:
				continue
			idx = self.gpus[name]
			for container_expr, end in links:
				entries.append(f'{{{container_expr}.Get({end}), "0.0.0.{idx + 1}"}}')
		if not entries:
			return
		self._emit_data_loop(
			struct_decls=["struct _IpAssign { Ptr<NetDevice> dev; const char* hostMask; };"],
			vector_decl="std::vector<_IpAssign> _ipAssigns",
			entries=entries,
			loop_var="a",
			loop_body=[
				"Ipv4AddressHelper _ipv4;",
				'_ipv4.SetBase("10.0.0.0", "255.0.0.0", a.hostMask);',
				"NetDeviceContainer _tmp;",
				"_tmp.Add(a.dev);",
				"_ipv4.Assign(_tmp);",
			],
		)

	def _emit_routing_table(self, routes_dict, idx_map, container_name, cpp_type, comment):
		# flattens every switch/nvswitch's routes into one vector + loop, regardless
		# of how many switches there are or how many routes each one owns
		entries = []
		for node_name, routes in routes_dict.items():
			node_expr = f"DynamicCast<{cpp_type}>({container_name}.Get({idx_map[node_name]}))"
			for dst_ip, container_expr, end in routes:
				entries.append(f'{{{node_expr}, Ipv4Address("{dst_ip}"), {container_expr}.Get({end})}}')
		if not entries:
			return
		self.emit(f"// {comment}")
		self._emit_data_loop(
			struct_decls=[f"struct _Route {{ Ptr<{cpp_type}> node; Ipv4Address dst; Ptr<NetDevice> dev; }};"],
			vector_decl="std::vector<_Route> _routes",
			entries=entries,
			loop_var="r",
			loop_body=["r.node->AddTableEntry(r.dst, r.dev->GetIfIndex());"],
		)

	def _emit_rdma_hw_setup(self, insn):
		# one entry per active GPU (its own RdmaHw/RdmaDriver, its routes, its IP);
		# the loop body that builds/wires them stays fixed-size as GPU count grows
		entries = []
		for name, idx in sorted(self.gpus.items(), key=lambda kv: kv[1]):
			routes = insn.gpu_rdma_routes.get(name, [])
			if not routes:
				continue
			route_strs = ", ".join(
				f'{{Ipv4Address("{dst_ip}"), {container_expr}.Get({end})->GetIfIndex(), {"true" if is_nvswitch else "false"}}}'
				for dst_ip, container_expr, end, is_nvswitch in routes
			)
			entries.append(
				f'{{gpunodes.Get({idx}), Ipv4Address("{insn.gpu_ip[name]}"), {{{route_strs}}}}}'
			)
		if not entries:
			return
		self.emit("// RdmaHw + RdmaDriver setup per GPU")
		self._emit_data_loop(
			struct_decls=[
				"struct _RdmaRoute { Ipv4Address dst; uint32_t ifIndex; bool isNvswitch; };",
				"struct _GpuRdmaSetup { Ptr<Node> gpu; Ipv4Address myIp; std::vector<_RdmaRoute> routes; };",
			],
			vector_decl="std::vector<_GpuRdmaSetup> _gpuSetups",
			entries=entries,
			loop_var="g",
			loop_body=[
				"Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();",
				f'rdmaHw->SetAttribute("GPUsPerServer", UintegerValue({insn.gpus_per_server}));',
				"Ptr<RdmaDriver> rdmaDriver = CreateObject<RdmaDriver>();",
				"rdmaDriver->SetNode(g.gpu);",
				"rdmaDriver->SetRdmaHw(rdmaHw);",
				"rdmaDriver->Init();",
				"for (auto& r : g.routes) {",
				"    rdmaHw->AddTableEntry(r.dst, r.ifIndex, r.isNvswitch);",
				"}",
				"DynamicCast<GPU>(g.gpu)->SetMyIp(g.myIp);",
				"g.gpu->AggregateObject(rdmaDriver);",
			],
		)

	def _emit_peer_ip_bookkeeping(self, insn):
		entries = []
		for name, peers in insn.gpu_peer_ip.items():
			gpu_expr = f"gpunodes.Get({self.gpus[name]})"
			for peer_name, peer_ip in peers.items():
				entries.append(f'{{{gpu_expr}, {self.gpus[peer_name]}, Ipv4Address("{peer_ip}")}}')
		if not entries:
			return
		self.emit("// peer IP bookkeeping for the collectives app's RDMA-routed peers")
		self._emit_data_loop(
			struct_decls=["struct _PeerIp { Ptr<Node> gpu; int16_t peerIdx; Ipv4Address peerIp; };"],
			vector_decl="std::vector<_PeerIp> _peerIps",
			entries=entries,
			loop_var="p",
			loop_body=["DynamicCast<GPU>(p.gpu)->PushPeerIpAddr(p.peerIdx, p.peerIp);"],
		)

	def _emit_peer_pacing(self, insn):
		entries = []
		for name, peer_wins in insn.gpu_peer_win.items():
			gpu_expr = f"gpunodes.Get({self.gpus[name]})"
			peer_rtts = insn.gpu_peer_rtt[name]
			for peer_name, win_bytes in peer_wins.items():
				entries.append(
					f"{{{gpu_expr}, {self.gpus[peer_name]}, {win_bytes}, {peer_rtts[peer_name]}}}"
				)
		if not entries:
			return
		self.emit("// peer RDMA pacing: bandwidth-delay-product window + base RTT per peer")
		self._emit_data_loop(
			struct_decls=["struct _PeerPacing { Ptr<Node> gpu; int16_t peerIdx; uint32_t winBytes; uint64_t baseRttNs; };"],
			vector_decl="std::vector<_PeerPacing> _peerPacing",
			entries=entries,
			loop_var="p",
			loop_body=[
				"DynamicCast<GPU>(p.gpu)->PushPeerWin(p.peerIdx, p.winBytes);",
				"DynamicCast<GPU>(p.gpu)->PushPeerBaseRtt(p.peerIdx, p.baseRttNs);",
			],
		)

	def _emit_mmu_config(self, insn):
		if not insn.mmu_config:
			return

		node_entries = []
		port_entries = []
		for name, ports in insn.mmu_config.items():
			if name in self.switches:
				node_expr = f"regswtches.Get({self.switches[name]})"
				mmu_expr = f"DynamicCast<SwitchNode>({node_expr})->m_mmu"
				is_switch = "true"
			else:
				node_expr = f"nvswtches.Get({self.nvswitches[name]})"
				mmu_expr = f"DynamicCast<NVSwitchNode>({node_expr})->m_mmu"
				is_switch = "false"
			node_entries.append(f"{{{node_expr}, {mmu_expr}, {len(ports)}, {is_switch}}}")
			for container_expr, end, headroom_bytes, kmin_kb, kmax_kb, pmax, shift in ports:
				port_entries.append(
					f"{{{mmu_expr}, {container_expr}.Get({end})->GetIfIndex(), "
					f"{headroom_bytes}, {kmin_kb}, {kmax_kb}, {pmax}, {shift}}}"
				)

		self.emit("// switch/nvswitch MMU: PFC headroom + ECN thresholds per port (otherwise")
		self.emit("// SwitchMmu's headroom[]/kmin[]/kmax[]/pmax[]/pfc_a_shift[] are uninitialized,")
		self.emit("// which disables realistic PFC backpressure under incast)")
		self._emit_data_loop(
			struct_decls=["struct _MmuNode { Ptr<Node> node; Ptr<SwitchMmu> mmu; uint32_t nPorts; bool isSwitch; };"],
			vector_decl="std::vector<_MmuNode> _mmuNodes",
			entries=node_entries,
			loop_var="n",
			loop_body=[
				'if (n.isSwitch) n.node->SetAttribute("EcnEnabled", BooleanValue(true));',
				"n.mmu->ConfigNPort(n.nPorts);",
				"n.mmu->node_id = n.node->GetId();",
			],
		)
		self._emit_data_loop(
			struct_decls=["struct _MmuPort { Ptr<SwitchMmu> mmu; uint32_t port; uint32_t headroomBytes; "
				"uint32_t kminKB; uint32_t kmaxKB; double pmax; uint32_t shift; };"],
			vector_decl="std::vector<_MmuPort> _mmuPorts",
			entries=port_entries,
			loop_var="p",
			loop_body=[
				"p.mmu->ConfigEcn(p.port, p.kminKB, p.kmaxKB, p.pmax);",
				"p.mmu->ConfigHdrm(p.port, p.headroomBytes);",
				"p.mmu->pfc_a_shift[p.port] = p.shift;",
			],
		)

	def _emit_flow_rules(self, insn):
		if not any(insn.flow_rules.values()):
			return

		self.emit("// custom flow-id forwarding rules (bypasses ECMP for matching flows)")
		switch_entries = [
			f"DynamicCast<SwitchNode>(regswtches.Get({self.switches[sw_name]}))"
			for sw_name in insn.flow_rules
		]
		self._emit_data_loop(
			struct_decls=[],
			vector_decl="std::vector<Ptr<SwitchNode>> _flowSwitches",
			entries=switch_entries,
			loop_var="sw",
			loop_body=['sw->SetAttribute("CustomFlowForwarding", BooleanValue(true));'],
		)

		rule_entries = []
		for sw_name, rules in insn.flow_rules.items():
			sw_expr = f"DynamicCast<SwitchNode>(regswtches.Get({self.switches[sw_name]}))"
			for flow_id, container_expr, end in rules:
				rule_entries.append(f"{{{sw_expr}, {flow_id}, {container_expr}.Get({end})->GetIfIndex()}}")
		self._emit_data_loop(
			struct_decls=["struct _FlowRule { Ptr<SwitchNode> sw; uint32_t flowId; uint32_t port; };"],
			vector_decl="std::vector<_FlowRule> _flowRules",
			entries=rule_entries,
			loop_var="r",
			loop_body=["r.sw->AddFlowForwardingRule(r.flowId, r.port);"],
		)

	# --------------------------------------------------
	# GPU handling
	# --------------------------------------------------

	# For now just print container map
	def _emit_gpu_setup(self):
		self.emit("")
		self.emit("/*")
		self.indent += 1
		for pair, container in self.container_map.items():
			self.emit(f"{pair[0]} -> {pair[1]}: {container}")
		self.indent -= 1
		self.emit("*/")

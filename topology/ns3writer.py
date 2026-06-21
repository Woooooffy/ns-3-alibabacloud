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
		self.emit("using namespace ns3;")
		self.emit("")

	def _emit_main_start(self):
		self.emit("int main(int argc, char *argv[]) {")
		self.indent += 1

		self.emit("NodeContainer gpunodes;")
		self.emit("NodeContainer regswtches;")
		self.emit("NodeContainer nvswtches;")
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

		for name, links in insn.gpu_links.items():
			if not links:
				continue
			idx = self.gpus[name]
			# every qbb NIC this gpu owns gets the SAME identity address (the
			# RdmaHw fabric is multi-rail/ECMP capable), one nested scope per
			# device so repeated _ipv4/_tmp declarations don't collide
			for container_expr, end in links:
				self.emit("{")
				self.indent += 1
				self.emit("Ipv4AddressHelper _ipv4;")
				self.emit(f'_ipv4.SetBase("10.0.0.0", "255.0.0.0", "0.0.0.{idx + 1}");')
				self.emit("NetDeviceContainer _tmp;")
				self.emit(f"_tmp.Add({container_expr}.Get({end}));")
				self.emit("_ipv4.Assign(_tmp);")
				self.indent -= 1
				self.emit("}")
		self.emit("")

		if insn.switch_routes:
			self.emit("// SwitchNode routing tables (BFS ECMP)")
		for sw_name, routes in insn.switch_routes.items():
			sw_idx = self.switches[sw_name]
			for dst_ip, container_expr, end in routes:
				self.emit("{")
				self.indent += 1
				self.emit(f'Ipv4Address _dst("{dst_ip}");')
				self.emit(f"DynamicCast<SwitchNode>(regswtches.Get({sw_idx}))->AddTableEntry(_dst, {container_expr}.Get({end})->GetIfIndex());")
				self.indent -= 1
				self.emit("}")
		if insn.switch_routes:
			self.emit("")

		if insn.nvswitch_routes:
			self.emit("// NVSwitchNode routing tables (BFS ECMP)")
		for nvsw_name, routes in insn.nvswitch_routes.items():
			nvsw_idx = self.nvswitches[nvsw_name]
			for dst_ip, container_expr, end in routes:
				self.emit("{")
				self.indent += 1
				self.emit(f'Ipv4Address _dst("{dst_ip}");')
				self.emit(f"DynamicCast<NVSwitchNode>(nvswtches.Get({nvsw_idx}))->AddTableEntry(_dst, {container_expr}.Get({end})->GetIfIndex());")
				self.indent -= 1
				self.emit("}")
		if insn.nvswitch_routes:
			self.emit("")

		self.emit("// RdmaHw + RdmaDriver setup per GPU")
		for name, idx in sorted(self.gpus.items(), key=lambda kv: kv[1]):
			routes = insn.gpu_rdma_routes.get(name, [])
			if not routes:
				continue
			gpu_expr = f"gpunodes.Get({idx})"
			self.emit("{")
			self.indent += 1
			self.emit(f"Ptr<RdmaHw> rdmaHw_gpu{idx} = CreateObject<RdmaHw>();")
			self.emit(f'rdmaHw_gpu{idx}->SetAttribute("GPUsPerServer", UintegerValue({insn.gpus_per_server}));')
			self.emit(f"Ptr<RdmaDriver> rdmaDriver_gpu{idx} = CreateObject<RdmaDriver>();")
			self.emit(f"rdmaDriver_gpu{idx}->SetNode({gpu_expr});")
			self.emit(f"rdmaDriver_gpu{idx}->SetRdmaHw(rdmaHw_gpu{idx});")
			self.emit(f"rdmaDriver_gpu{idx}->Init();")
			self.emit("")
			for dst_ip, container_expr, end, is_nvswitch in routes:
				self.emit("{")
				self.indent += 1
				self.emit(f'Ipv4Address _peerIp("{dst_ip}");')
				is_nv = "true" if is_nvswitch else "false"
				self.emit(f"rdmaHw_gpu{idx}->AddTableEntry(_peerIp, {container_expr}.Get({end})->GetIfIndex(), {is_nv});")
				self.indent -= 1
				self.emit("}")
			self.emit("")
			self.emit(f"DynamicCast<GPU>({gpu_expr})->SetMyIp(Ipv4Address(\"{insn.gpu_ip[name]}\"));")
			self.emit(f"{gpu_expr}->AggregateObject(rdmaDriver_gpu{idx});")
			self.indent -= 1
			self.emit("}")
		self.emit("")

		self.emit("// peer IP bookkeeping for the collectives app's RDMA-routed peers")
		for name, peers in insn.gpu_peer_ip.items():
			gpu_expr = f"gpunodes.Get({self.gpus[name]})"
			for peer_name, peer_ip in peers.items():
				self.emit(
					f"DynamicCast<GPU>({gpu_expr})->PushPeerIpAddr("
					f"{self.gpus[peer_name]}, Ipv4Address(\"{peer_ip}\"));"
				)
		self.emit("")

		if any(insn.flow_rules.values()):
			self.emit("// custom flow-id forwarding rules (bypasses ECMP for matching flows)")
			for sw_name, rules in insn.flow_rules.items():
				sw_idx = self.switches[sw_name]
				self.emit(f"DynamicCast<SwitchNode>(regswtches.Get({sw_idx}))->SetAttribute(\"CustomFlowForwarding\", BooleanValue(true));")
				for flow_id, container_expr, end in rules:
					self.emit(f"DynamicCast<SwitchNode>(regswtches.Get({sw_idx}))->AddFlowForwardingRule({flow_id}, {container_expr}.Get({end})->GetIfIndex());")
			self.emit("")

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

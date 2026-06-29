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

		elif insn.__class__.__name__ == "NS3BuildRdmaFabric":
			self._emit_build_rdma_fabric(insn)

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
			# Everything touching a switch or nvswitch instead gets wired up by
			# RdmaFabricHelper::Build, once all links are installed.
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
	# RDMA fabric: handed off to RdmaFabricHelper, which discovers the qbb
	# link graph from already-installed NetDevices/Channels and runs
	# BFS-ECMP routing, IP assignment, and MMU/PFC config in C++ at ns-3
	# runtime. Only DSL-level intent that the C++ can't infer from topology
	# alone (rdma attrs, flow-forwarding switches) is emitted here.
	# --------------------------------------------------

	def _emit_build_rdma_fabric(self, insn):
		for name, value in sorted(insn.rdma_attrs.items()):
			self.emit(f'Config::SetDefault("ns3::RdmaHw::{name}", UintegerValue({value}));')
		if insn.rdma_attrs:
			self.emit("")

		self.emit("// ---- RDMA fabric: addressing, switch/nvswitch routing, RdmaHw/RdmaDriver ----")
		self.emit("RdmaFabricHelper rdmaFabric;")
		self.emit("rdmaFabric.Build(gpunodes, regswtches, nvswtches);")
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

from ns3codegen import *

from typing import List

class NS3Writer:
	def __init__(self, filename, codegen):
		self.filename = filename
		self.gpus = codegen.gpus		  # dict[str, int]
		self.switches = codegen.switches  # dict[str, int]
		self.link_helpers = codegen.link_helpers # dict[tuple, int]
		self.insns = codegen.insns

		self.lines = []
		self.indent = 0
		self.container_uid = 0

		self.container_map = {} # (src_name, dst_name) -> container_expr

	def emit(self, line=""):
		self.lines.append("    " * self.indent + line)

	def write(self):
		self._emit_headers()
		self._emit_main_start()

		for insn in self.insns:
			self._handle_insn(insn)

		self._emit_forwarding_setup()
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
		self.emit('#include "ns3/csma-module.h"')
		self.emit('#include "ns3/ethernet-switch-module.h"')
		self.emit('#include "ns3/distributed-training-module.h"')
		self.emit("")
		self.emit("using namespace ns3;")
		self.emit("")

	def _emit_main_start(self):
		self.emit("int main(int argc, char *argv[]) {")
		self.indent += 1

		self.emit("NodeContainer gpunodes;")
		self.emit("NodeContainer swtches;")
		self.emit("P4Helper sw_helper;")
		self.emit("sw_helper.SetDeviceAttribute(\"EnableCustomImpl\", BooleanValue(true));")
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
		from types import SimpleNamespace

		if insn.__class__.__name__ == "NS3MakeGPUs":
			self.emit(f"gpunodes.Create<{ 'GPU' }>({insn.n_gpus});")

		elif insn.__class__.__name__ == "NS3MakeSwitches":
			self.emit(f"swtches.Create({insn.n_switches});")

		elif insn.__class__.__name__ == "NS3MakeLinkHelper":
			self._emit_link_helper(insn)

		elif insn.__class__.__name__ == "NS3MakeSwitchHelper":
			self._emit_switch_helper(insn)

		elif insn.__class__.__name__ == "NS3InstallLink":
			self._emit_link_install(insn)

		else:
			raise ValueError(f"Unknown instruction {insn}")

	# --------------------------------------------------
	# Link and switch helpers
	# --------------------------------------------------

	def _emit_switch_helper(self, insn):
		self.emit(f"sw_helper.SetDeviceAttribute(\"Mtu\", UintegerValue({insn.mtu}));")
		for id in insn.switch_ids:
			self.emit(f"NetDeviceContainer sw_dev{id} = sw_helper.Install(swtches.Get({id}));")
			self.emit(f"Ptr<P4SwitchNetDevice> sw{id} = DynamicCast<P4SwitchNetDevice>(sw_dev{id}.Get(0));")
		self.emit("")

	def _emit_link_helper(self, insn):
		hid = insn.id
		delay_val, delay_unit = insn.delay
		bw_val, bw_unit = insn.data_rate
		mtu = insn.mtu

		match insn.type:
			case "eth":
				helper_name = "SwitchedEthernet"
			case "p2p":
				helper_name = "PointToPoint"
			case "default":
				helper_name = "Csma"
			case _:
				raise RuntimeError(f"Unrecognized link type {insn.type}.")

		self.emit(f"{helper_name}Helper link_helper{hid};")
		self.emit(f"link_helper{hid}.SetDeviceAttribute(\"Mtu\", UintegerValue({insn.mtu}));")
		self.emit(f'link_helper{hid}.SetChannelAttribute("Delay", StringValue("{delay_val}{delay_unit}"));')
		if insn.type == "p2p":
			# PointToPointNetDevice (unlike Csma/SwitchedEthernet) exposes DataRate
			# as a device attribute, not a channel attribute
			self.emit(f'link_helper{hid}.SetDeviceAttribute("DataRate", StringValue("{bw_val}{bw_unit}"));')
		else:
			self.emit(f'link_helper{hid}.SetChannelAttribute("DataRate", StringValue("{bw_val}{bw_unit}"));')
		self.emit("")

	# --------------------------------------------------
	# Link installation
	# --------------------------------------------------

	def _resolve_node(self, name):
		if name in self.gpus:
			return f"gpunodes.Get({self.gpus[name]})", "gpu"
		elif name in self.switches:
			return f"swtches.Get({self.switches[name]})", "switch"
		else:
			raise ValueError(f"Unknown node {name}")

	def _emit_link_install(self, insn):
		src_expr, src_type = self._resolve_node(insn.src)
		dst_expr, dst_type = self._resolve_node(insn.dst)

		hid = insn.link_helper

		# --------------------------------------------------
		# GPU <-> GPU : point-to-point
		# --------------------------------------------------

		if src_type == "gpu" and dst_type == "gpu":

			container_expr = f"devs{hid}_{self.container_uid}"

			self.emit(
				f"NetDeviceContainer {container_expr} = "
				f"link_helper{hid}.Install({src_expr}, {dst_expr});"
			)

			self.emit("")

			self.container_map[(insn.src, insn.dst)] = container_expr

			self._emit_push_send_device(
				src_expr,
				insn.dst,
				f"{container_expr}.Get(0)"
			)

			self._emit_push_recv_device(
				dst_expr,
				insn.src,
				f"{container_expr}.Get(1)"
			)

			# p2p links are full-duplex, so the same pair of devices also
			# serves the reverse direction (dst -> src)
			self._emit_push_send_device(
				dst_expr,
				insn.src,
				f"{container_expr}.Get(1)"
			)

			self._emit_push_recv_device(
				src_expr,
				insn.dst,
				f"{container_expr}.Get(0)"
			)

			# a single p2p link replaces what used to be 2 unidirectional CSMA
			# links, so register both peers' addresses here
			self.emit(
				f"DynamicCast<GPU>({src_expr})->PushPeerAddr("
				f"{self.gpus[insn.dst]}, ({container_expr}.Get(1))->GetAddress());"
			)
			self.emit(
				f"DynamicCast<GPU>({dst_expr})->PushPeerAddr("
				f"{self.gpus[insn.src]}, ({container_expr}.Get(0))->GetAddress());"
			)

			self.emit("")

		# --------------------------------------------------
		# GPU <-> Switch
		# --------------------------------------------------

		elif src_type == "gpu" and dst_type == "switch":

			container_expr = f"devs{hid}_{self.container_uid}"

			sw_idx = self.switches[insn.dst]

			self.emit(
				f"NetDeviceContainer {container_expr} = "
				f"link_helper{hid}.ConnectHost(sw{sw_idx}, {src_expr});"
			)

			self.emit("")

			self._emit_loop_push_peer_device(
				src_expr,
				insn.src,
				f"{container_expr}.Get(0)"
			)

			self._emit_loop_push_peer_addr(
				insn.src,
				f"{container_expr}.Get(0)"
			)

			self._record_host_attachment(
				insn.dst,
				insn.src,
				f"{container_expr}.Get(0)"
			)

		# --------------------------------------------------
		# Switch <-> GPU
		# --------------------------------------------------

		elif src_type == "switch" and dst_type == "gpu":

			container_expr = f"devs{hid}_{self.container_uid}"

			sw_idx = self.switches[insn.src]

			self.emit(
				f"NetDeviceContainer {container_expr} = "
				f"link_helper{hid}.ConnectHost(sw{sw_idx}, {dst_expr});"
			)

			self.emit("")

			self._emit_loop_push_peer_device(
				dst_expr,
				insn.dst,
				f"{container_expr}.Get(0)"
			)

			self._emit_loop_push_peer_addr(
				insn.dst,
				f"{container_expr}.Get(0)"
			)

			self._record_host_attachment(
				insn.src,
				insn.dst,
				f"{container_expr}.Get(0)"
			)

		# --------------------------------------------------
		# Switch <-> Switch
		# --------------------------------------------------

		elif src_type == "switch" and dst_type == "switch":

			src_idx = self.switches[insn.src]
			dst_idx = self.switches[insn.dst]

			self.emit(
				f"link_helper{hid}.ConnectSwitches(sw{src_idx}, sw{dst_idx});"
			)

			self.emit("")

		else:
			raise RuntimeError("Unsupported link type")

		self.container_uid += 1

	def _emit_push_send_device(self, src_expr, dst_name, dev_expr):
		self.emit(f"DynamicCast<GPU>({src_expr})->PushSendPeerDevice({self.gpus[dst_name]}, {dev_expr});")

	def _emit_push_recv_device(self, dst_expr, src_name, dev_expr):
		self.emit(f"DynamicCast<GPU>({dst_expr})->PushRecvPeerDevice({self.gpus[src_name]}, {dev_expr});")

	def _emit_loop_push_peer_device(self, gpu_expr, gpu_name, dev_expr):
		self.emit(f"for (int i = 0; i < {len(self.gpus)}; ++i)" + "{")
		self.indent += 1

		self.emit(f"if (i != {self.gpus[gpu_name]})" + "{")
		self.indent += 1

		self.emit(
			f"DynamicCast<GPU>({gpu_expr})->PushSendPeerDevice(i, {dev_expr});"
		)

		self.emit(
			f"DynamicCast<GPU>({gpu_expr})->PushRecvPeerDevice(i, {dev_expr});"
		)

		self.indent -= 1
		self.emit("}")

		self.indent -= 1
		self.emit("}")

		self.emit("")

	def _emit_loop_push_peer_addr(self, gpu_name, dev_expr):
		gpu_idx = self.gpus[gpu_name]

		self.emit(f"for (int i = 0; i < {len(self.gpus)}; ++i)" + "{")
		self.indent += 1

		self.emit(f"if (i != {gpu_idx})" + "{")
		self.indent += 1

		self.emit(
			f"DynamicCast<GPU>(gpunodes.Get(i))->PushPeerAddr({gpu_idx}, {dev_expr}->GetAddress());"
		)

		self.indent -= 1
		self.emit("}")

		self.indent -= 1
		self.emit("}")

		self.emit("")
	# --------------------------------------------------
	# Switch handling
	# --------------------------------------------------

	def _record_host_attachment(self, switch_name, gpu_name, dev_expr):
		if not hasattr(self, "host_attachments"):
			self.host_attachments = {}

		self.host_attachments.setdefault(switch_name, []).append(
			(gpu_name, dev_expr)
		)

	def _emit_forwarding_setup(self):
		return
		if not hasattr(self, "host_attachments"):
			return

		self.emit("")
		self.emit("// Switch forwarding tables")

		for sw_name, hosts in self.host_attachments.items():

			sw_idx = self.switches[sw_name]

			for port_idx, (gpu_name, dev_expr) in enumerate(hosts):

				for other_gpu_name in self.gpus.keys():

					self.emit(
						f"sw{sw_idx}->GetCustomImpl()->AddAddrForwarding("
						f"DynamicCast<GPU>(gpunodes.Get({self.gpus[other_gpu_name]}))"
						f"->GetAddress(), "
						f"{port_idx}"
						f");"
					)

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

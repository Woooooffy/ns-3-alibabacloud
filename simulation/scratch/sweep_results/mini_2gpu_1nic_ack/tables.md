### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline | flowId+nic | all | p2p |
|---:|---:|---:|---:|---:|
| 4GB | 6,274,494.7 | 5,593,538.1 (+10.9%) | 5,592,081.0 (+10.9%) | 8,349,513.3 (-33.1%) |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline | flowId+nic | all | p2p |
|---:|---:|---:|---:|---:|
| 4GB | 0 | 0 | 0 | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline | flowId+nic | all | p2p |
|---:|---:|---:|---:|---:|
| 4GB | 1,903.9 | 1,785.4 | 1,752.1 | 1,323.2 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | baseline | flowId+nic | all | p2p |
|---:|---:|---:|---:|---:|
| 4GB | 0.0 | 0.0 | 100.0 / 0 | 0.0 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 100.

| size/pair | baseline | flowId+nic | all | p2p |
|---:|---:|---:|---:|---:|
| 4GB | - | - | - | - |

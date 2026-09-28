### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps+sync |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 4,530,546.9 | 4,525,571.8 (+0.1%) | 3,146,011.5 (+30.6%) | 3,140,553.1 (+30.7%) | 3,143,778.9 (+30.6%) | 4,290,535.9 (+5.3%) | 3,487,786.1 (+23.0%) | 3,501,145.1 (+22.7%) | 4,332,702.6 (+4.4%) |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps+sync |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps+sync |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 1,439.4 | 1,439.4 | 971.6 | 971.6 | 971.6 | 668.5 | 547.5 | 601.5 | 672.6 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps+sync |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 0.0 | 0.0 | 0.0 | 0.0 | 100.0 / 0 | 100.0 / 0 | 0.0 | 0.0 | 100.0 / 0 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 100.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps+sync |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | - | - | - | - | - | - | - | - | - |

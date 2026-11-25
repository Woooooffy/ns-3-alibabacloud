### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 3,129,896.6 | 3,764,944.9 (-20.3%) | 2,839,299.4 (+9.3%) | 3,501,028.7 (-11.9%) | 3,314,021.7 (-5.9%) | 3,605,644.4 (-15.2%) | 4,167,106.4 (-33.1%) | 4,166,539.2 (-33.1%) | 3,314,021.7 (-5.9%) |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 36,636 | 28,118 | 52,120 | 22,725 | 0 | 0 | 0 | 0 | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 2,285.5 | 2,210.9 | 2,022.0 | 2,084.3 | 1,540.4 | 955.0 | 1,605.3 | 1,597.0 | 1,540.4 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 0.0 | 0.0 | 0.0 | 0.0 | 92.9 / 0 | 92.9 / 0 | 0.0 | 0.0 | 92.9 / 0 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 100.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | - | - | - | - | - | - | - | - | - |

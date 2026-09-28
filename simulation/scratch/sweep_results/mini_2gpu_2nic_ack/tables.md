### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 3,129,805.9 | 3,827,179.3 (-22.3%) | 2,824,632.8 (+9.8%) | 3,827,673.7 (-22.3%) | 3,605,644.4 (-15.2%) | 4,167,106.4 (-33.1%) | 4,166,539.2 (-33.1%) | 3,314,021.7 (-5.9%) |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 51,449 | 3,403 | 50,443 | 22,031 | 0 | 0 | 0 | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 2,235.8 | 1,991.0 | 2,034.5 | 1,901.6 | 955.0 | 1,605.3 | 1,597.0 | 1,540.4 |

### Bytes shaped by the XML rate (%, paced / unshapeable messages)

Left: share of transmitted bytes whose gap the schedule's `rate` actually set. Right: share of rate-carrying messages that fit in one MTU, which have no inter-packet gap to stretch. A 0 on the left means the run is identical to one with `rate` off.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | 0.0 | 0.0 | 0.0 | 0.0 | 92.9 / 0 | 0.0 | 0.0 | 92.9 / 0 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 100.

| size/pair | baseline | baseline+sync | flowId+nic | flowId+nic+sync | all+sync | p2p | p2p+sync | noRemoteDeps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 4GB | - | - | - | - | - | - | - | - |

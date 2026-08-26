### Latency

Simulated completion time in us; in parentheses, improvement over the baseline column (positive = faster).

| size/pair | baseline | rate | netDeps | flowId | nicSel | all |
|---:|---:|---:|---:|---:|---:|---:|
| 1KB | 4.7 | 4.7 (+0.0%) | 4.6 (+2.1%) | 4.7 (+0.0%) | 4.7 (+0.0%) | 4.5 (+4.3%) |
| 2KB | 5.7 | 5.7 (+0.0%) | 5.6 (+1.8%) | 5.7 (+0.0%) | 5.7 (+0.0%) | 5.3 (+7.0%) |
| 4KB | 8.0 | 8.0 (+0.0%) | 7.4 (+7.5%) | 8.0 (+0.0%) | 8.0 (+0.0%) | 7.3 (+8.8%) |
| 8KB | 12.5 | 12.5 (+0.0%) | 11.4 (+8.8%) | 12.5 (+0.0%) | 12.7 (-1.6%) | 11.3 (+9.6%) |
| 16KB | 21.5 | 21.5 (+0.0%) | 19.2 (+10.7%) | 21.7 (-0.9%) | 20.9 (+2.8%) | 18.8 (+12.6%) |
| 32KB | 38.4 | 38.4 (+0.0%) | 34.2 (+10.9%) | 38.7 (-0.8%) | 35.4 (+7.8%) | 33.5 (+12.8%) |
| 64KB | 68.2 | 68.2 (+0.0%) | 64.8 (+5.0%) | 68.2 (+0.0%) | 66.0 (+3.2%) | 62.8 (+7.9%) |

### PFC pause / resume frames

Count over the whole run. A single number means pause and resume agreed; `pause/resume` shows them separately when they did not.

| size/pair | baseline | rate | netDeps | flowId | nicSel | all |
|---:|---:|---:|---:|---:|---:|---:|
| 1KB | 0 | 0 | 0 | 0 | 0 | 0 |
| 2KB | 0 | 0 | 0 | 0 | 0 | 0 |
| 4KB | 0 | 0 | 0 | 0 | 0 | 0 |
| 8KB | 0 | 0 | 0 | 0 | 0 | 0 |
| 16KB | 58 | 58 | 0 | 68 | 0 | 0 |
| 32KB | 272 | 272 | 0 | 293 | 50 | 0 |
| 64KB | 681 | 681 | 129 | 886 | 585 | 0 |

### Peak queue depth (KB)

Deepest egress queue reached on any switch port, at any instant, anywhere in the network.

| size/pair | baseline | rate | netDeps | flowId | nicSel | all |
|---:|---:|---:|---:|---:|---:|---:|
| 1KB | 12.6 | 12.6 | 7.3 | 12.8 | 17.9 | 2.2 |
| 2KB | 23.4 | 23.4 | 14.3 | 23.9 | 34.3 | 13.2 |
| 4KB | 43.4 | 43.4 | 32.4 | 45.6 | 64.4 | 29.8 |
| 8KB | 95.5 | 95.5 | 66.8 | 94.7 | 126.9 | 64.9 |
| 16KB | 209.0 | 209.0 | 123.1 | 215.4 | 203.3 | 99.6 |
| 32KB | 298.7 | 298.7 | 178.4 | 278.2 | 253.0 | 103.8 |
| 64KB | 510.2 | 510.2 | 356.7 | 431.8 | 472.9 | 112.1 |

### GPU fabric NIC bandwidth (Gbps, mean / peak per NIC)

Mean is per fabric NIC over the window in which any NIC was transmitting; peak is the busiest single sample. Line rate is 400.

| size/pair | baseline | rate | netDeps | flowId | nicSel | all |
|---:|---:|---:|---:|---:|---:|---:|
| 1KB | 396.8 / 500 | 396.8 / 500 | 396.8 / 500 | 403.6 / 509 | 352.3 / 492 | 355.7 / 495 |
| 2KB | 369.1 / 492 | 369.1 / 492 | 369.1 / 492 | 372.7 / 495 | 382.2 / 574 | 384.2 / 564 |
| 4KB | 382.2 / 562 | 382.2 / 562 | 382.2 / 574 | 384.2 / 564 | 379.3 / 676 | 370.3 / 678 |
| 8KB | 379.3 / 676 | 379.3 / 676 | 379.3 / 676 | 380.3 / 678 | 378.1 / 668 | 373.5 / 669 |
| 16KB | 378.1 / 668 | 378.1 / 668 | 378.1 / 668 | 376.1 / 669 | 381.0 / 664 | 376.1 / 664 |
| 32KB | 339.7 / 664 | 339.7 / 664 | 381.0 / 664 | 336.9 / 664 | 359.8 / 664 | 376.1 / 664 |
| 64KB | 351.7 / 498 | 351.7 / 498 | 359.8 / 498 | 347.6 / 498 | 365.8 / 498 | 376.1 / 498 |

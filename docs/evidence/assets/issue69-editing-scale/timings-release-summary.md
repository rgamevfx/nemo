| workload | networks | nodes | edges | params (bool/int/float/string/choice/vec2/vec3/color) | anim ch | anim keys | media entries/bins/marks | serialized B |
|---|---|---|---|---|---|---|---|---|
| graph.chain.small | 1 | 82 | 63 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 14652 |
| graph.chain.large | 1 | 642 | 511 | 128/0/128/1/128/0/0/128 | 0 | 0 | 0/0/0 | 114499 |
| graph.fanin.small | 1 | 81 | 62 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 14730 |
| graph.fanin.large | 1 | 641 | 510 | 128/0/128/1/128/0/0/128 | 0 | 0 | 0/0/0 | 116107 |
| graph.multinetwork.small | 5 | 278 | 252 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 42533 |
| graph.multinetwork.large | 9 | 2186 | 2040 | 128/0/128/1/128/0/0/128 | 0 | 0 | 0/0/0 | 336938 |
| params.small | 1 | 114 | 0 | 400/0/112/1/208/0/0/688 | 0 | 0 | 0/0/0 | 103243 |
| params.large | 1 | 1026 | 0 | 3712/0/1024/1/1920/0/0/6400 | 0 | 0 | 0/0/0 | 951816 |
| animation.small | 1 | 18 | 0 | 16/0/16/1/16/0/0/16 | 16 | 128 | 0/0/0 | 34413 |
| animation.large | 1 | 130 | 0 | 128/0/128/1/128/0/0/128 | 128 | 4096 | 0/0/0 | 933331 |
| media.small | 1 | 18 | 0 | 16/0/16/1/16/0/0/16 | 0 | 0 | 64/4/128 | 49015 |
| media.large | 1 | 130 | 0 | 128/0/128/1/128/0/0/128 | 0 | 0 | 512/16/1024 | 388161 |
| history.below | 1 | 82 | 63 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 14652 |
| history.at | 1 | 82 | 63 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 14652 |
| history.above | 1 | 82 | 63 | 16/0/16/1/16/0/0/16 | 0 | 0 | 0/0/0 | 14652 |

| workload | op | cold n | cold p50 | cold p95 | warm n | warm p50 | warm p95 | warm min | warm max | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| graph.chain.small | snapshot | 20 | 13.638 | 32.960 | 200 | 12.765 | 28.564 | 11.730 | 34.833 | `{"edit_targets": 81, "history_capacity": 256}` |
| graph.chain.small | prepare_save | 20 | 543.872 | 779.966 | 60 | 543.011 | 641.038 | 535.665 | 1406.177 | `{"edit_targets": 81, "history_capacity": 256}` |
| graph.chain.small | serialize_content | 20 | 525.572 | 562.162 | 60 | 529.273 | 1105.925 | 522.352 | 1416.179 | `{"edit_targets": 81, "history_capacity": 256}` |
| graph.chain.small | undo_sequence | 64 | 21.792 | 24.793 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.chain.small | redo_sequence | 64 | 21.462 | 22.902 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.chain.small | submit.rename | 20 | 24.375 | 24.871 | 200 | 25.142 | 36.498 | 22.580 | 55.272 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.set_layout | 20 | 25.665 | 38.764 | 200 | 25.335 | 27.802 | 23.349 | 37.586 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.add_node | 20 | 43.883 | 52.198 | 200 | 48.296 | 53.449 | 45.939 | 139.010 | `{"declared_nodes": 82, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.connect | 20 | 33.846 | 36.850 | 200 | 33.334 | 64.616 | 29.817 | 98.279 | `{"declared_edges": 63, "declared_nodes": 82, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.set_param | 20 | 26.133 | 53.599 | 200 | 25.283 | 31.548 | 23.069 | 58.018 | `{"edit_targets": 81, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.set_parameters.8 | 20 | 27.471 | 29.740 | 200 | 25.670 | 32.981 | 24.035 | 57.631 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.set_parameters.64 | 20 | 39.308 | 44.473 | 200 | 39.034 | 57.707 | 35.566 | 113.303 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.small | submit.transaction.8 | 20 | 26.647 | 30.192 | 200 | 27.139 | 30.041 | 25.347 | 41.137 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.chain.small | gesture.begin | 20 | 10.808 | 11.638 | 200 | 10.730 | 10.897 | 7.888 | 14.176 | `{"edits": 1, "publishes": false}` |
| graph.chain.small | gesture.update | 20 | 17.665 | 18.629 | 200 | 17.285 | 18.867 | 15.419 | 59.103 | `{"edits": 1, "publishes": false}` |
| graph.chain.small | gesture.cancel | 20 | 4.344 | 4.408 | 200 | 4.355 | 11.838 | 4.249 | 14.979 | `{"edits": 1, "publishes": false}` |
| graph.chain.small | gesture.commit | 20 | 30.916 | 50.186 | 200 | 29.736 | 38.319 | 28.820 | 68.296 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| graph.chain.large | snapshot | 20 | 103.346 | 119.670 | 200 | 102.903 | 111.360 | 101.854 | 329.231 | `{"edit_targets": 641, "history_capacity": 256}` |
| graph.chain.large | prepare_save | 20 | 4130.158 | 5184.194 | 60 | 4245.870 | 5196.329 | 3944.967 | 6126.371 | `{"edit_targets": 641, "history_capacity": 256}` |
| graph.chain.large | serialize_content | 20 | 4018.830 | 4342.514 | 60 | 3952.988 | 4393.034 | 3929.675 | 5239.117 | `{"edit_targets": 641, "history_capacity": 256}` |
| graph.chain.large | undo_sequence | 64 | 373.592 | 426.910 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.chain.large | redo_sequence | 64 | 381.819 | 517.762 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.chain.large | submit.rename | 20 | 365.045 | 404.951 | 200 | 371.528 | 378.905 | 365.927 | 622.324 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.set_layout | 20 | 371.268 | 381.896 | 200 | 372.528 | 408.262 | 366.976 | 785.917 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.add_node | 20 | 506.758 | 602.558 | 200 | 510.108 | 563.026 | 499.601 | 827.279 | `{"declared_nodes": 642, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.connect | 20 | 449.901 | 460.932 | 200 | 452.596 | 493.670 | 433.540 | 943.931 | `{"declared_edges": 511, "declared_nodes": 642, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.set_param | 20 | 383.117 | 403.363 | 200 | 383.091 | 396.347 | 377.894 | 826.929 | `{"edit_targets": 641, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.set_parameters.8 | 20 | 387.893 | 477.320 | 200 | 396.901 | 435.711 | 386.193 | 827.030 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.set_parameters.64 | 20 | 429.282 | 449.592 | 200 | 430.450 | 453.821 | 406.874 | 810.401 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.chain.large | submit.transaction.8 | 20 | 406.086 | 420.150 | 200 | 402.998 | 459.770 | 395.754 | 822.685 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.chain.large | gesture.begin | 20 | 73.610 | 103.257 | 200 | 72.728 | 75.847 | 67.948 | 120.160 | `{"edits": 1, "publishes": false}` |
| graph.chain.large | gesture.update | 20 | 117.527 | 122.898 | 200 | 118.554 | 131.077 | 115.364 | 259.571 | `{"edits": 1, "publishes": false}` |
| graph.chain.large | gesture.cancel | 20 | 51.656 | 58.533 | 200 | 51.263 | 56.569 | 50.443 | 101.655 | `{"edits": 1, "publishes": false}` |
| graph.chain.large | gesture.commit | 20 | 432.113 | 456.611 | 200 | 430.006 | 518.458 | 416.724 | 728.855 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| graph.fanin.small | snapshot | 20 | 10.911 | 12.722 | 200 | 10.722 | 11.057 | 9.818 | 18.362 | `{"edit_targets": 49, "history_capacity": 256}` |
| graph.fanin.small | prepare_save | 20 | 542.957 | 796.678 | 60 | 540.537 | 701.139 | 531.780 | 857.009 | `{"edit_targets": 49, "history_capacity": 256}` |
| graph.fanin.small | serialize_content | 20 | 525.963 | 643.799 | 60 | 525.969 | 1065.025 | 519.176 | 1178.207 | `{"edit_targets": 49, "history_capacity": 256}` |
| graph.fanin.small | undo_sequence | 64 | 18.935 | 20.423 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.fanin.small | redo_sequence | 64 | 19.019 | 20.260 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.fanin.small | submit.rename | 20 | 20.991 | 21.484 | 200 | 20.551 | 21.734 | 19.401 | 25.174 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.set_layout | 20 | 19.429 | 20.219 | 200 | 19.459 | 41.586 | 17.867 | 50.638 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.add_node | 20 | 38.865 | 40.553 | 200 | 35.608 | 37.649 | 32.688 | 43.198 | `{"declared_nodes": 81, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.connect | 20 | 21.439 | 25.255 | 200 | 21.654 | 24.572 | 19.568 | 34.333 | `{"declared_edges": 62, "declared_nodes": 81, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.set_param | 20 | 20.485 | 21.460 | 200 | 19.963 | 21.126 | 18.486 | 48.753 | `{"edit_targets": 49, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.set_parameters.8 | 20 | 22.333 | 25.471 | 200 | 22.159 | 28.846 | 20.522 | 48.730 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.small | submit.transaction.8 | 20 | 23.145 | 24.021 | 200 | 22.662 | 23.906 | 21.523 | 34.837 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.fanin.small | gesture.begin | 20 | 7.439 | 8.094 | 200 | 7.386 | 7.651 | 6.617 | 11.709 | `{"edits": 1, "publishes": false}` |
| graph.fanin.small | gesture.update | 20 | 12.531 | 13.765 | 200 | 13.165 | 14.239 | 11.561 | 24.151 | `{"edits": 1, "publishes": false}` |
| graph.fanin.small | gesture.cancel | 20 | 4.226 | 4.435 | 200 | 4.205 | 4.282 | 4.121 | 7.942 | `{"edits": 1, "publishes": false}` |
| graph.fanin.small | gesture.commit | 20 | 25.604 | 40.624 | 200 | 26.615 | 27.622 | 25.171 | 54.792 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| graph.fanin.large | snapshot | 20 | 81.168 | 88.850 | 200 | 80.238 | 84.099 | 79.475 | 121.740 | `{"edit_targets": 385, "history_capacity": 256}` |
| graph.fanin.large | prepare_save | 20 | 4048.152 | 4274.601 | 60 | 4117.954 | 4631.177 | 3921.592 | 4891.650 | `{"edit_targets": 385, "history_capacity": 256}` |
| graph.fanin.large | serialize_content | 20 | 4081.134 | 4558.120 | 60 | 4029.931 | 4527.910 | 3934.487 | 5560.572 | `{"edit_targets": 385, "history_capacity": 256}` |
| graph.fanin.large | undo_sequence | 64 | 351.586 | 361.408 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.fanin.large | redo_sequence | 64 | 355.355 | 372.746 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.fanin.large | submit.rename | 20 | 349.833 | 371.740 | 200 | 348.775 | 390.549 | 338.347 | 534.623 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.set_layout | 20 | 341.189 | 430.220 | 200 | 351.208 | 381.925 | 338.111 | 687.643 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.add_node | 20 | 460.465 | 485.539 | 200 | 466.026 | 579.757 | 445.875 | 959.074 | `{"declared_nodes": 641, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.connect | 20 | 360.809 | 666.124 | 200 | 364.615 | 527.327 | 358.545 | 717.095 | `{"declared_edges": 510, "declared_nodes": 641, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.set_param | 20 | 359.081 | 363.847 | 200 | 361.511 | 527.027 | 349.984 | 707.528 | `{"edit_targets": 385, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.set_parameters.8 | 20 | 388.486 | 611.034 | 200 | 373.419 | 578.974 | 363.609 | 761.517 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.set_parameters.64 | 20 | 397.615 | 697.882 | 200 | 399.541 | 719.117 | 369.280 | 856.934 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.fanin.large | submit.transaction.8 | 20 | 372.716 | 377.284 | 200 | 375.251 | 494.572 | 368.184 | 648.068 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.fanin.large | gesture.begin | 20 | 59.065 | 62.268 | 200 | 58.986 | 97.149 | 51.606 | 163.442 | `{"edits": 1, "publishes": false}` |
| graph.fanin.large | gesture.update | 20 | 91.719 | 98.797 | 200 | 91.803 | 106.535 | 90.184 | 261.251 | `{"edits": 1, "publishes": false}` |
| graph.fanin.large | gesture.cancel | 20 | 36.089 | 37.276 | 200 | 35.861 | 39.312 | 35.470 | 103.947 | `{"edits": 1, "publishes": false}` |
| graph.fanin.large | gesture.commit | 20 | 400.340 | 452.011 | 200 | 396.492 | 552.020 | 382.546 | 680.789 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| graph.multinetwork.small | snapshot | 20 | 42.480 | 48.196 | 200 | 42.096 | 44.840 | 36.719 | 68.897 | `{"edit_targets": 273, "history_capacity": 256}` |
| graph.multinetwork.small | prepare_save | 20 | 1646.981 | 1754.582 | 60 | 1650.865 | 2119.452 | 1638.441 | 2314.888 | `{"edit_targets": 273, "history_capacity": 256}` |
| graph.multinetwork.small | serialize_content | 20 | 1629.598 | 1690.700 | 60 | 1630.148 | 2286.283 | 1619.088 | 2435.402 | `{"edit_targets": 273, "history_capacity": 256}` |
| graph.multinetwork.small | undo_sequence | 64 | 64.657 | 73.203 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.multinetwork.small | redo_sequence | 64 | 64.297 | 69.266 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.multinetwork.small | submit.rename | 20 | 65.582 | 68.794 | 200 | 66.648 | 109.784 | 60.167 | 158.807 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.set_layout | 20 | 66.645 | 70.644 | 200 | 67.135 | 74.391 | 61.153 | 122.316 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.add_node | 20 | 84.017 | 85.807 | 200 | 85.421 | 90.851 | 75.134 | 151.550 | `{"declared_nodes": 65, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.connect | 20 | 79.873 | 113.282 | 200 | 82.217 | 147.194 | 76.149 | 169.397 | `{"declared_edges": 63, "declared_nodes": 65, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.set_param | 20 | 73.417 | 95.149 | 200 | 73.777 | 77.952 | 64.873 | 163.196 | `{"edit_targets": 273, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.set_parameters.8 | 20 | 77.653 | 85.767 | 200 | 77.261 | 80.740 | 70.667 | 92.312 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.set_parameters.64 | 20 | 92.055 | 105.285 | 200 | 92.069 | 155.863 | 82.873 | 185.940 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.small | submit.transaction.8 | 20 | 77.443 | 85.895 | 200 | 77.839 | 95.440 | 70.568 | 170.925 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.multinetwork.small | gesture.begin | 20 | 34.680 | 36.913 | 200 | 34.748 | 37.863 | 26.461 | 72.589 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.small | gesture.update | 20 | 54.984 | 87.762 | 200 | 54.083 | 56.082 | 41.150 | 78.941 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.small | gesture.cancel | 20 | 13.018 | 13.858 | 200 | 13.037 | 13.207 | 12.832 | 16.325 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.small | gesture.commit | 20 | 79.953 | 106.058 | 200 | 79.580 | 120.215 | 78.336 | 161.814 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| graph.multinetwork.large | snapshot | 20 | 331.625 | 401.669 | 200 | 327.979 | 354.664 | 274.236 | 502.338 | `{"edit_targets": 2177, "history_capacity": 256}` |
| graph.multinetwork.large | prepare_save | 20 | 14146.632 | 15432.810 | 60 | 13989.065 | 15030.976 | 13232.188 | 17120.787 | `{"edit_targets": 2177, "history_capacity": 256}` |
| graph.multinetwork.large | serialize_content | 20 | 13687.082 | 14442.444 | 60 | 13665.434 | 14871.548 | 12841.151 | 15440.554 | `{"edit_targets": 2177, "history_capacity": 256}` |
| graph.multinetwork.large | undo_sequence | 64 | 830.740 | 960.022 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.multinetwork.large | redo_sequence | 64 | 821.006 | 893.200 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| graph.multinetwork.large | submit.rename | 20 | 775.352 | 848.082 | 200 | 780.690 | 938.540 | 750.956 | 1236.996 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.set_layout | 20 | 758.184 | 833.221 | 200 | 776.921 | 826.199 | 766.646 | 1508.974 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.add_node | 20 | 828.565 | 865.789 | 200 | 855.043 | 937.759 | 794.082 | 1207.400 | `{"declared_nodes": 257, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.connect | 20 | 847.448 | 893.559 | 200 | 887.542 | 1666.892 | 764.504 | 1822.424 | `{"declared_edges": 255, "declared_nodes": 257, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.set_param | 20 | 1498.603 | 1724.117 | 200 | 877.471 | 1631.313 | 748.619 | 1996.978 | `{"edit_targets": 2177, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.set_parameters.8 | 20 | 827.774 | 921.500 | 200 | 832.360 | 931.330 | 761.099 | 1653.439 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.set_parameters.64 | 20 | 860.526 | 933.237 | 200 | 861.897 | 1011.860 | 840.324 | 1348.710 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true}` |
| graph.multinetwork.large | submit.transaction.8 | 20 | 860.160 | 912.662 | 200 | 847.937 | 947.601 | 831.492 | 1327.543 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| graph.multinetwork.large | gesture.begin | 20 | 210.521 | 236.902 | 200 | 209.236 | 240.383 | 206.441 | 419.546 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.large | gesture.update | 20 | 350.657 | 643.081 | 200 | 344.012 | 370.360 | 337.480 | 500.470 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.large | gesture.cancel | 20 | 155.265 | 166.300 | 200 | 155.585 | 178.051 | 152.847 | 347.015 | `{"edits": 1, "publishes": false}` |
| graph.multinetwork.large | gesture.commit | 20 | 975.684 | 1155.338 | 200 | 948.142 | 1197.118 | 880.340 | 1525.092 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 65, "history_entries": 1, "publishes": true}` |
| params.small | snapshot | 20 | 134.244 | 180.724 | 200 | 79.440 | 125.299 | 66.795 | 185.546 | `{"edit_targets": 113, "history_capacity": 256}` |
| params.small | prepare_save | 20 | 2498.450 | 2807.482 | 60 | 2507.517 | 3911.667 | 2479.159 | 5369.491 | `{"edit_targets": 113, "history_capacity": 256}` |
| params.small | serialize_content | 20 | 2447.026 | 3135.486 | 60 | 2414.003 | 2633.531 | 2395.436 | 4020.105 | `{"edit_targets": 113, "history_capacity": 256}` |
| params.small | undo_sequence | 64 | 112.950 | 130.421 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| params.small | redo_sequence | 64 | 115.600 | 150.225 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| params.small | submit.set_param | 20 | 108.355 | 110.457 | 200 | 107.474 | 112.278 | 95.746 | 187.796 | `{"edit_targets": 113, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.small | submit.set_parameters.8 | 20 | 111.234 | 216.965 | 200 | 109.848 | 113.471 | 100.603 | 116.718 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.small | submit.set_parameters.64 | 20 | 119.858 | 126.025 | 200 | 129.932 | 145.815 | 111.543 | 149.943 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.small | submit.transaction.8 | 20 | 127.015 | 262.736 | 200 | 126.730 | 130.956 | 111.830 | 135.180 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| params.small | gesture.begin | 20 | 67.644 | 71.569 | 200 | 67.793 | 71.001 | 53.146 | 100.596 | `{"edits": 1, "publishes": false}` |
| params.small | gesture.update | 20 | 85.266 | 91.185 | 200 | 87.823 | 92.632 | 68.418 | 137.958 | `{"edits": 1, "publishes": false}` |
| params.small | gesture.cancel | 20 | 20.535 | 20.599 | 200 | 20.447 | 20.693 | 20.247 | 23.661 | `{"edits": 1, "publishes": false}` |
| params.small | gesture.commit | 20 | 121.756 | 123.782 | 200 | 123.729 | 128.143 | 120.664 | 157.633 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| params.large | snapshot | 20 | 730.144 | 788.247 | 200 | 722.920 | 809.362 | 710.771 | 1142.519 | `{"edit_targets": 1025, "history_capacity": 256}` |
| params.large | prepare_save | 20 | 29793.898 | 32272.947 | 60 | 30259.327 | 31837.681 | 29214.953 | 32794.273 | `{"edit_targets": 1025, "history_capacity": 256}` |
| params.large | serialize_content | 20 | 29303.234 | 30509.853 | 60 | 29302.669 | 31167.786 | 28371.228 | 33839.409 | `{"edit_targets": 1025, "history_capacity": 256}` |
| params.large | undo_sequence | 64 | 1604.442 | 1807.082 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| params.large | redo_sequence | 64 | 1592.682 | 1794.181 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| params.large | submit.set_param | 20 | 1446.295 | 1637.715 | 200 | 1479.239 | 2377.625 | 1393.135 | 3181.305 | `{"edit_targets": 1025, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.large | submit.set_parameters.8 | 20 | 1466.221 | 2047.996 | 200 | 1500.247 | 1845.895 | 1374.116 | 2407.406 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.large | submit.set_parameters.64 | 20 | 1534.426 | 1645.404 | 200 | 1566.611 | 1806.012 | 1456.704 | 2378.691 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| params.large | submit.transaction.8 | 20 | 1489.420 | 1654.627 | 200 | 1524.668 | 1828.658 | 1402.250 | 3038.728 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| params.large | gesture.begin | 20 | 432.907 | 552.309 | 200 | 428.699 | 478.308 | 416.098 | 1084.059 | `{"edits": 1, "publishes": false}` |
| params.large | gesture.update | 20 | 767.674 | 880.180 | 200 | 748.597 | 958.811 | 725.215 | 1337.675 | `{"edits": 1, "publishes": false}` |
| params.large | gesture.cancel | 20 | 311.058 | 507.466 | 200 | 303.836 | 335.030 | 298.994 | 743.790 | `{"edits": 1, "publishes": false}` |
| params.large | gesture.commit | 20 | 2002.633 | 3014.949 | 200 | 1929.234 | 2491.380 | 1769.670 | 3150.094 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| animation.small | snapshot | 20 | 8.187 | 9.955 | 200 | 8.160 | 8.269 | 7.564 | 17.317 | `{"edit_targets": 17, "history_capacity": 256}` |
| animation.small | prepare_save | 20 | 787.618 | 803.205 | 60 | 787.509 | 887.377 | 778.558 | 1278.181 | `{"edit_targets": 17, "history_capacity": 256}` |
| animation.small | serialize_content | 20 | 777.693 | 789.968 | 60 | 776.190 | 815.002 | 769.171 | 887.475 | `{"edit_targets": 17, "history_capacity": 256}` |
| animation.small | undo_sequence | 64 | 18.187 | 18.839 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| animation.small | redo_sequence | 64 | 17.848 | 18.545 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| animation.small | submit.set_param | 20 | 18.473 | 18.703 | 200 | 18.231 | 18.698 | 17.682 | 21.964 | `{"edit_targets": 17, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| animation.small | submit.set_parameters.8 | 20 | 19.821 | 21.098 | 200 | 19.280 | 20.097 | 18.647 | 61.228 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| animation.small | submit.transaction.8 | 20 | 19.963 | 21.263 | 200 | 19.433 | 20.035 | 18.814 | 29.439 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| animation.small | gesture.begin | 20 | 5.759 | 6.397 | 200 | 5.707 | 5.799 | 5.372 | 8.648 | `{"edits": 1, "publishes": false}` |
| animation.small | gesture.update | 20 | 8.943 | 9.748 | 200 | 8.996 | 9.820 | 8.056 | 11.432 | `{"edits": 1, "publishes": false}` |
| animation.small | gesture.cancel | 20 | 2.296 | 2.356 | 200 | 2.296 | 2.344 | 2.255 | 6.432 | `{"edits": 1, "publishes": false}` |
| animation.small | gesture.commit | 20 | 20.578 | 21.530 | 200 | 20.438 | 21.607 | 19.813 | 127.119 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| animation.small | submit.insert_keyframe | 20 | 41.220 | 43.761 | 200 | 38.083 | 41.229 | 37.073 | 50.808 | `{"history_capacity": 256, "keys_before": 128}` |
| animation.small | gesture.keyed_begin | 20 | 27.252 | 32.223 | 200 | 27.168 | 27.626 | 25.790 | 38.507 | `{"edits": 1, "publishes": false}` |
| animation.small | gesture.keyed_update | 20 | 31.917 | 68.496 | 200 | 31.557 | 33.553 | 29.050 | 45.395 | `{"edits": 1, "publishes": false}` |
| animation.small | gesture.keyed_commit | 20 | 44.432 | 48.400 | 200 | 44.228 | 48.965 | 42.248 | 95.678 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| animation.large | snapshot | 20 | 128.799 | 291.740 | 200 | 126.073 | 131.907 | 123.910 | 195.002 | `{"edit_targets": 129, "history_capacity": 256}` |
| animation.large | prepare_save | 20 | 22226.593 | 26941.030 | 60 | 22235.222 | 23379.489 | 20954.795 | 23681.225 | `{"edit_targets": 129, "history_capacity": 256}` |
| animation.large | serialize_content | 20 | 22017.287 | 23680.968 | 60 | 21767.882 | 24279.677 | 20987.055 | 24904.640 | `{"edit_targets": 129, "history_capacity": 256}` |
| animation.large | undo_sequence | 64 | 429.361 | 815.917 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| animation.large | redo_sequence | 64 | 429.925 | 471.686 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| animation.large | submit.set_param | 20 | 393.296 | 410.834 | 200 | 394.797 | 418.868 | 384.237 | 897.570 | `{"edit_targets": 129, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| animation.large | submit.set_parameters.8 | 20 | 397.848 | 407.141 | 200 | 399.601 | 453.316 | 389.338 | 688.106 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| animation.large | submit.set_parameters.64 | 20 | 411.978 | 426.395 | 200 | 416.896 | 453.072 | 405.112 | 815.805 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| animation.large | submit.transaction.8 | 20 | 407.666 | 523.300 | 200 | 404.811 | 465.938 | 397.729 | 665.535 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| animation.large | gesture.begin | 20 | 97.098 | 100.029 | 200 | 97.287 | 117.900 | 96.501 | 214.950 | `{"edits": 1, "publishes": false}` |
| animation.large | gesture.update | 20 | 138.611 | 156.532 | 200 | 139.075 | 208.050 | 129.858 | 311.239 | `{"edits": 1, "publishes": false}` |
| animation.large | gesture.cancel | 20 | 37.561 | 67.488 | 200 | 37.500 | 41.581 | 37.262 | 131.214 | `{"edits": 1, "publishes": false}` |
| animation.large | gesture.commit | 20 | 447.108 | 545.817 | 200 | 439.503 | 566.327 | 428.441 | 897.785 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| animation.large | submit.insert_keyframe | 20 | 1136.800 | 1245.619 | 200 | 1150.958 | 1269.960 | 1125.741 | 1880.227 | `{"history_capacity": 256, "keys_before": 4096}` |
| animation.large | gesture.keyed_begin | 20 | 814.232 | 830.218 | 200 | 818.013 | 865.838 | 806.965 | 1791.201 | `{"edits": 1, "publishes": false}` |
| animation.large | gesture.keyed_update | 20 | 927.652 | 1051.441 | 200 | 898.459 | 1418.410 | 869.907 | 1990.470 | `{"edits": 1, "publishes": false}` |
| animation.large | gesture.keyed_commit | 20 | 1247.447 | 1362.958 | 200 | 1244.628 | 1422.929 | 1208.764 | 2178.900 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| media.small | snapshot | 20 | 34.013 | 37.714 | 200 | 33.312 | 34.452 | 32.238 | 41.088 | `{"edit_targets": 17, "history_capacity": 256}` |
| media.small | prepare_save | 20 | 969.096 | 1600.378 | 60 | 970.362 | 978.489 | 964.047 | 1016.893 | `{"edit_targets": 17, "history_capacity": 256}` |
| media.small | serialize_content | 20 | 922.888 | 945.818 | 60 | 922.681 | 1312.084 | 916.050 | 1801.531 | `{"edit_targets": 17, "history_capacity": 256}` |
| media.small | undo_sequence | 64 | 52.700 | 60.842 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| media.small | redo_sequence | 64 | 53.449 | 56.301 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| media.small | submit.set_param | 20 | 55.508 | 60.846 | 200 | 56.517 | 59.995 | 54.880 | 99.872 | `{"edit_targets": 17, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| media.small | submit.set_parameters.8 | 20 | 58.286 | 62.642 | 200 | 58.638 | 62.407 | 56.823 | 98.375 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| media.small | submit.transaction.8 | 20 | 60.280 | 69.333 | 200 | 60.087 | 63.327 | 57.236 | 76.386 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| media.small | gesture.begin | 20 | 23.829 | 25.396 | 200 | 23.444 | 24.392 | 20.791 | 67.541 | `{"edits": 1, "publishes": false}` |
| media.small | gesture.update | 20 | 34.683 | 36.350 | 200 | 33.994 | 79.689 | 32.740 | 89.815 | `{"edits": 1, "publishes": false}` |
| media.small | gesture.cancel | 20 | 10.310 | 10.532 | 200 | 10.180 | 10.407 | 9.988 | 13.951 | `{"edits": 1, "publishes": false}` |
| media.small | gesture.commit | 20 | 70.820 | 76.740 | 200 | 70.585 | 71.954 | 69.134 | 188.188 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| media.small | submit.set_media_metadata | 20 | 59.111 | 65.600 | 200 | 58.170 | 61.671 | 56.616 | 101.607 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 4, "media_entries": 64, "samples_undone_between_runs": true}` |
| media.small | submit.set_media_marks | 20 | 57.300 | 61.524 | 200 | 57.653 | 61.051 | 54.581 | 63.534 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 4, "media_entries": 64, "samples_undone_between_runs": true}` |
| media.small | submit.move_media | 20 | 57.578 | 59.790 | 200 | 57.166 | 98.100 | 54.241 | 160.281 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 4, "media_entries": 64, "samples_undone_between_runs": true}` |
| media.small | submit.import_media_reference | 20 | 64.834 | 68.894 | 200 | 65.718 | 69.214 | 62.094 | 182.052 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "source_registered_outside_timer": true}` |
| media.large | snapshot | 20 | 276.955 | 510.421 | 200 | 286.565 | 336.270 | 241.274 | 630.715 | `{"edit_targets": 129, "history_capacity": 256}` |
| media.large | prepare_save | 20 | 7711.991 | 9503.192 | 60 | 7658.999 | 9163.357 | 7213.458 | 9333.500 | `{"edit_targets": 129, "history_capacity": 256}` |
| media.large | serialize_content | 20 | 7290.372 | 7986.226 | 60 | 7117.333 | 8675.954 | 6919.120 | 9000.784 | `{"edit_targets": 129, "history_capacity": 256}` |
| media.large | undo_sequence | 64 | 542.233 | 721.944 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| media.large | redo_sequence | 64 | 562.700 | 755.499 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "entries": 64, "history_capacity": 256, "history_depth_before": 64}` |
| media.large | submit.set_param | 20 | 530.347 | 590.605 | 200 | 603.312 | 706.892 | 553.001 | 1096.413 | `{"edit_targets": 129, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| media.large | submit.set_parameters.8 | 20 | 619.636 | 694.267 | 200 | 634.342 | 823.419 | 607.246 | 1391.533 | `{"batch_edits": 8, "distinct_nodes": 8, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| media.large | submit.set_parameters.64 | 20 | 682.649 | 722.277 | 200 | 687.303 | 889.965 | 629.614 | 1428.764 | `{"batch_edits": 64, "distinct_nodes": 64, "history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true}` |
| media.large | submit.transaction.8 | 20 | 632.213 | 654.712 | 200 | 653.762 | 687.944 | 615.709 | 1306.103 | `{"history_capacity": 256, "history_depth_at_start": 64, "samples_undone_between_runs": true, "transaction_commands": 8}` |
| media.large | gesture.begin | 20 | 215.134 | 444.987 | 200 | 211.062 | 234.575 | 179.159 | 397.360 | `{"edits": 1, "publishes": false}` |
| media.large | gesture.update | 20 | 297.932 | 325.304 | 200 | 331.732 | 386.514 | 289.478 | 765.266 | `{"edits": 1, "publishes": false}` |
| media.large | gesture.cancel | 20 | 126.756 | 134.450 | 200 | 128.685 | 136.453 | 126.194 | 172.804 | `{"edits": 1, "publishes": false}` |
| media.large | gesture.commit | 20 | 741.857 | 907.497 | 200 | 757.519 | 909.395 | 734.150 | 1502.236 | `{"committed_entry_undone_between_runs": true, "edits": 1, "history_depth_at_start": 64, "history_entries": 1, "publishes": true}` |
| media.large | submit.set_media_metadata | 20 | 655.277 | 1066.842 | 200 | 654.837 | 772.912 | 633.377 | 1455.442 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 16, "media_entries": 512, "samples_undone_between_runs": true}` |
| media.large | submit.set_media_marks | 20 | 648.410 | 663.109 | 200 | 653.596 | 687.467 | 625.148 | 941.303 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 16, "media_entries": 512, "samples_undone_between_runs": true}` |
| media.large | submit.move_media | 20 | 657.865 | 707.056 | 200 | 660.232 | 721.942 | 642.941 | 1424.675 | `{"history_capacity": 256, "history_depth_at_start": 64, "media_bins": 16, "media_entries": 512, "samples_undone_between_runs": true}` |
| media.large | submit.import_media_reference | 20 | 661.950 | 694.023 | 200 | 681.839 | 741.876 | 634.056 | 1005.152 | `{"history_capacity": 256, "history_depth_at_start": 65, "samples_undone_between_runs": true, "source_registered_outside_timer": true}` |
| history.below | submit.set_param | 64 | 17.108 | 22.743 | - | - | - | - | - | `{"cold_warm_split": "not applicable: accumulation is confounded with history depth", "commits": 64, "depth_end": 64, "depth_start": 1, "history_capacity": 256}` |
| history.below | submit.set_param.depth_window | 64 | 17.108 | 22.743 | - | - | - | - | - | `{"depth_end": 64, "depth_start": 1, "history_capacity": 256, "window_commits": 64}` |
| history.below | undo_sequence | 64 | 22.705 | 23.905 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "history_capacity": 256, "observed_retained_entries": 64, "timed_entries": 64}` |
| history.below | redo_sequence | 64 | 22.667 | 24.731 | - | - | - | - | - | `{"history_capacity": 256, "timed_entries": 64}` |
| history.at | submit.set_param | 256 | 18.319 | 18.977 | - | - | - | - | - | `{"cold_warm_split": "not applicable: accumulation is confounded with history depth", "commits": 256, "depth_end": 256, "depth_start": 1, "history_capacity": 256}` |
| history.at | submit.set_param.depth_window | 64 | 18.355 | 18.970 | - | - | - | - | - | `{"depth_end": 256, "depth_start": 193, "history_capacity": 256, "window_commits": 64}` |
| history.at | undo_sequence | 256 | 24.661 | 27.881 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "history_capacity": 256, "observed_retained_entries": 256, "timed_entries": 256}` |
| history.at | redo_sequence | 256 | 25.516 | 26.632 | - | - | - | - | - | `{"history_capacity": 256, "timed_entries": 256}` |
| history.above | submit.set_param | 384 | 18.473 | 38.036 | - | - | - | - | - | `{"cold_warm_split": "not applicable: accumulation is confounded with history depth", "commits": 384, "depth_end": 256, "depth_start": 1, "history_capacity": 256}` |
| history.above | submit.set_param.depth_window | 64 | 37.344 | 45.670 | - | - | - | - | - | `{"depth_end": 256, "depth_start": 256, "history_capacity": 256, "window_commits": 64}` |
| history.above | undo_sequence | 256 | 25.392 | 27.469 | - | - | - | - | - | `{"cold_warm_split": "not applicable: one ordered sequence", "history_capacity": 256, "observed_retained_entries": 256, "timed_entries": 256}` |
| history.above | redo_sequence | 256 | 25.552 | 27.028 | - | - | - | - | - | `{"history_capacity": 256, "timed_entries": 256}` |

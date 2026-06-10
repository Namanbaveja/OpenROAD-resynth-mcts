
# CPA-remap test on UART post-CTS benchmark
# Usage: openroad -no_init uart_cpa_remap.tcl
#
# Env-var overrides (all optional):
#   CORNER             slow|fast   (default slow)
#   BFS_HOPS           K-hop BFS expansion depth (default 1)
#   MAX_INST           max instances per cone (default 100)
#   PROXIMITY_UM       proximity cone growth in microns; 0=K-hop BFS (default 0)
#   LONG_WIRE          long-wire break threshold in ps; 0=disabled (default 0)
#   MCTS_ITERS         MCTS iterations per cone; 0=balance+map only (default 0)
#   MCTS_MAX_DEPTH     MCTS tree depth (default 7)
#   MCTS_UCB           MCTS UCB exploration constant (default 1.414)
#   MAX_CONES          max cones applied per round; 0=unlimited (default 0)
#   WITH_REPAIR_TIMING 1=run repair_timing after cpa_remap (default 0)

proc getenv_or {varname default} {
  if { [info exists ::env($varname)] } { return $::env($varname) }
  return $default
}

set corner          [getenv_or CORNER             "slow"]
set bfs_hops        [getenv_or BFS_HOPS           1]
set max_inst        [getenv_or MAX_INST            200]
set prox_um         [getenv_or PROXIMITY_UM        0.0]
set long_wire       [getenv_or LONG_WIRE           0]
set mcts_iters      [getenv_or MCTS_ITERS          0]
set mcts_max_depth  [getenv_or MCTS_MAX_DEPTH      7]
set mcts_ucb        [getenv_or MCTS_UCB            1.414]
set max_cones       [getenv_or MAX_CONES           0]
set with_repair_timing [getenv_or WITH_REPAIR_TIMING 0]

# ---- load design ----
define_corners fast slow
foreach f {
  ../../asap7/asap7sc7p5t_AO_RVT_SS_nldm_211120.lib.gz
  ../../asap7/asap7sc7p5t_INVBUF_RVT_SS_nldm_220122.lib.gz
  ../../asap7/asap7sc7p5t_OA_RVT_SS_nldm_211120.lib.gz
  ../../asap7/asap7sc7p5t_SEQ_RVT_SS_nldm_220123.lib
  ../../asap7/asap7sc7p5t_SIMPLE_RVT_SS_nldm_211120.lib.gz
} { read_liberty -corner slow $f }
foreach f {
  ../../asap7/asap7sc7p5t_AO_RVT_FF_nldm_211120.lib.gz
  ../../asap7/asap7sc7p5t_INVBUF_RVT_FF_nldm_220122.lib.gz
  ../../asap7/asap7sc7p5t_OA_RVT_FF_nldm_211120.lib.gz
  ../../asap7/asap7sc7p5t_SEQ_RVT_FF_nldm_220123.lib
  ../../asap7/asap7sc7p5t_SIMPLE_RVT_FF_nldm_211120.lib.gz
} { read_liberty -corner fast $f }

read_lef ../../asap7/asap7_tech_1x_201209.lef
read_lef ../../asap7/asap7sc7p5t_28_R_1x_220121a.lef
read_def ../../benchmark_cts/new/uart/4_cts.def
read_sdc ../../benchmark_cts/new/uart/4_cts.sdc

set_wire_rc -signal \
    -resistance 1.3340e-1 \
    -capacitance 8.6000e-2

# ---- snapshot helper ----
proc snapshot_qor { label } {
  set wns_s    ""
  set tns_s    ""
  set viol_count ""
  catch { set wns_s      [sta::worst_slack_cmd "max"] }
  catch { set tns_s      [sta::total_negative_slack_cmd "max"] }
  catch { set viol_count [sta::endpoint_violation_count "max"] }
  if { $wns_s eq "" }      { set wns_s 0.0 }
  if { $tns_s eq "" }      { set tns_s 0.0 }
  if { $viol_count eq "" } { set viol_count 0 }
  if { $wns_s > 0.0 }      { set wns_s 0.0 }
  set wns_ps [expr {$wns_s * 1.0e12}]
  set tns_ps [expr {$tns_s * 1.0e12}]
  puts "===== $label ====="
  puts [format "  WNS  = %12.4f ps" $wns_ps]
  puts [format "  TNS  = %12.4f ps" $tns_ps]
  puts [format "  FEPs = %12d"      $viol_count]
  return [list $wns_ps $tns_ps $viol_count]
}

# ---- baseline ----
lassign [snapshot_qor "BASELINE"] wns_before tns_before viols_before

remove_fillers

puts ""
puts ">>> Config: corner=$corner bfs_hops=$bfs_hops max_inst=$max_inst proximity_um=$prox_um long_wire=${long_wire}ps mcts_iters=$mcts_iters mcts_max_depth=$mcts_max_depth mcts_ucb=$mcts_ucb max_cones=$max_cones"
puts ""

# ---- build resynth_cpa_remap command ----
set t0 [clock milliseconds]

set cmd "resynth_cpa_remap -corner $corner -bfs_hops $bfs_hops -max_instances $max_inst"
if { $prox_um != 0.0 } {
  append cmd " -proximity_um $prox_um"
}
if { $long_wire != 0 } {
  append cmd " -long_wire_ps $long_wire"
}
if { $mcts_iters > 0 } {
  append cmd " -mcts_iters $mcts_iters -mcts_max_depth $mcts_max_depth -mcts_ucb_constant $mcts_ucb"
}
if { $max_cones > 0 } {
  append cmd " -max_cones_per_round $max_cones"
}

puts ">>> CMD: $cmd"
puts ""
eval $cmd

set elapsed_cpa [expr {[clock milliseconds] - $t0}]

puts ""
lassign [snapshot_qor "AFTER resynth_cpa_remap"] wns_cpa tns_cpa viols_cpa

# ---- optional repair_timing ----
set wns_final  $wns_cpa
set tns_final  $tns_cpa
set viols_final $viols_cpa
set elapsed_repair 0

if { $with_repair_timing } {
  puts ""
  puts ">>> Running repair_timing ..."
  set t1 [clock milliseconds]
  repair_timing
  set elapsed_repair [expr {[clock milliseconds] - $t1}]
  puts ""
  lassign [snapshot_qor "AFTER repair_timing"] wns_final tns_final viols_final
}

# ---- final delta ----
set wns_delta   [expr {$wns_final - $wns_before}]
set tns_delta   [expr {$tns_final - $tns_before}]
set viols_delta [expr {$viols_final - $viols_before}]

set wns_pct [expr {$wns_before == 0.0 ? 0.0 : 100.0 * double($wns_delta) / abs($wns_before)}]
set tns_pct [expr {$tns_before == 0.0 ? 0.0 : 100.0 * double($tns_delta) / abs($tns_before)}]

puts ""
puts "===== QoR DELTA (total) ====="
puts [format "  WNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)" \
          $wns_before $wns_final $wns_delta $wns_pct]
puts [format "  TNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)" \
          $tns_before $tns_final $tns_delta $tns_pct]
puts [format "  FEPs: %12d     ->  %12d" $viols_before $viols_final]
puts [format "  TIME: cpa=%dms  repair=%dms" $elapsed_cpa $elapsed_repair]
puts ""
puts [format "RESULT corner=%s bfs=%s max_inst=%s prox_um=%.1f long_wire_ps=%s mcts=%s max_cones=%s with_repair=%s wns_before=%.4f wns_cpa=%.4f wns_final=%.4f tns_before=%.4f tns_cpa=%.4f tns_final=%.4f viols_before=%d viols_cpa=%d viols_final=%d cpa_ms=%d repair_ms=%d" \
      $corner $bfs_hops $max_inst $prox_um $long_wire $mcts_iters $max_cones $with_repair_timing \
      $wns_before $wns_cpa $wns_final $tns_before $tns_cpa $tns_final \
      $viols_before $viols_cpa $viols_final $elapsed_cpa $elapsed_repair]

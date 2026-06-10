# Two-pass resynth_mcts on sasc_top.
# Pass 1 restructures the original 4_cts.def at pct%.
# Pass 2 restructures the result of Pass 1 at the same pct%
# (new worst endpoints after Pass 1 changed timing).

set seed         [expr {[info exists ::env(SEED)]         ? $::env(SEED)         : 2345}]
set iters        [expr {[info exists ::env(ITERS)]        ? $::env(ITERS)        : 1000}]
set corner       [expr {[info exists ::env(CORNER)]       ? $::env(CORNER)       : "slow"}]
set pct          [expr {[info exists ::env(PERCENTAGE)]   ? $::env(PERCENTAGE)   : 25}]
set max_depth    [expr {[info exists ::env(MAX_DEPTH)]    ? $::env(MAX_DEPTH)    : 6}]
set ucb_constant [expr {[info exists ::env(UCB_CONSTANT)] ? $::env(UCB_CONSTANT) : 0.3}]
set debug_lvl    [expr {[info exists ::env(DEBUG_LEVEL)]  ? $::env(DEBUG_LEVEL)  : 1}]

# ---- libraries + design -----------------------------------------------------
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
read_def ../../benchmark_cts/Opencore/sasc_top/4_cts.def
read_sdc ../../benchmark_cts/Opencore/sasc_top/4_cts.sdc

set_wire_rc -signal \
    -resistance 1.3340e-1 \
    -capacitance 8.6000e-2

# ---- QoR snapshot helper ----------------------------------------------------
proc snapshot_qor { label } {
  set wns_s 0.0; set tns_s 0.0; set viol_count 0
  catch { set wns_s      [sta::worst_slack_cmd "max"] }
  catch { set tns_s      [sta::total_negative_slack_cmd "max"] }
  catch { set viol_count [sta::endpoint_violation_count "max"] }
  if { $wns_s > 0.0 } { set wns_s 0.0 }
  set wns_ps [expr {$wns_s * 1.0e12}]
  set tns_ps [expr {$tns_s * 1.0e12}]
  puts "===== $label ====="
  puts [format "  WNS  = %12.4f ps" $wns_ps]
  puts [format "  TNS  = %12.4f ps" $tns_ps]
  puts [format "  FEPs = %12d" $viol_count]
  return [list $wns_ps $tns_ps $viol_count]
}

set_debug_level RMP mcts    $debug_lvl
set_debug_level RMP sa_eval $debug_lvl

# ---- baseline ---------------------------------------------------------------
lassign [snapshot_qor "S0: BASELINE (4_cts.def)"] wns_s0 tns_s0 viols_s0

remove_fillers

# ---- pass 1 -----------------------------------------------------------------
puts ""
puts ">>> PASS 1: resynth_mcts -percentage $pct -iters $iters -seed $seed"
puts ""
set t0 [clock milliseconds]
resynth_mcts \
  -percentage   $pct \
  -corner       $corner \
  -seed         $seed \
  -iters        $iters \
  -max_depth    $max_depth \
  -ucb_constant $ucb_constant
set t_pass1 [expr {[clock milliseconds] - $t0}]

lassign [snapshot_qor "S1: AFTER PASS 1"] wns_s1 tns_s1 viols_s1
puts [format "  Pass1 delta vs baseline:  WNS %+.4f ps   TNS %+.4f ps   FEPs %+d" \
      [expr {$wns_s1 - $wns_s0}] [expr {$tns_s1 - $tns_s0}] [expr {$viols_s1 - $viols_s0}]]

# ---- pass 2 (on the already-restructured in-memory design) ------------------
puts ""
puts ">>> PASS 2: resynth_mcts -percentage $pct -iters $iters -seed $seed"
puts "    (timing re-evaluated — different worst endpoints after Pass 1)"
puts ""
set t0 [clock milliseconds]
resynth_mcts \
  -percentage   $pct \
  -corner       $corner \
  -seed         $seed \
  -iters        $iters \
  -max_depth    $max_depth \
  -ucb_constant $ucb_constant
set t_pass2 [expr {[clock milliseconds] - $t0}]

lassign [snapshot_qor "S2: AFTER PASS 2"] wns_s2 tns_s2 viols_s2

# ---- summary ----------------------------------------------------------------
puts ""
puts "===== FINAL SUMMARY (2-pass resynth_mcts pct=${pct}%) ====="
puts [format "  S0 baseline:  WNS=%12.4f ps   TNS=%14.4f ps   FEPs=%d" $wns_s0 $tns_s0 $viols_s0]
puts [format "  S1 pass1:     WNS=%12.4f ps   TNS=%14.4f ps   FEPs=%d   time=%dms" $wns_s1 $tns_s1 $viols_s1 $t_pass1]
puts [format "  S2 pass2:     WNS=%12.4f ps   TNS=%14.4f ps   FEPs=%d   time=%dms" $wns_s2 $tns_s2 $viols_s2 $t_pass2]
puts ""
puts [format "  Total WNS delta (S2-S0):  %+.4f ps   (%+.2f%%)" \
      [expr {$wns_s2 - $wns_s0}] \
      [expr {$wns_s0 == 0.0 ? 0.0 : 100.0 * ($wns_s2 - $wns_s0) / abs($wns_s0)}]]
puts [format "  Total TNS delta (S2-S0):  %+.4f ps   (%+.2f%%)" \
      [expr {$tns_s2 - $tns_s0}] \
      [expr {$tns_s0 == 0.0 ? 0.0 : 100.0 * ($tns_s2 - $tns_s0) / abs($tns_s0)}]]
puts [format "  Total FEP delta (S2-S0):  %+d" [expr {$viols_s2 - $viols_s0}]]

puts ""
puts [format "RESULT2PASS seed=%s pct=%s iters=%s \
wns_s0=%.4f tns_s0=%.4f viols_s0=%d \
wns_s1=%.4f tns_s1=%.4f viols_s1=%d \
wns_s2=%.4f tns_s2=%.4f viols_s2=%d \
wns_delta=%.4f tns_delta=%.4f viols_delta=%d \
time_pass1_ms=%d time_pass2_ms=%d" \
  $seed $pct $iters \
  $wns_s0 $tns_s0 $viols_s0 \
  $wns_s1 $tns_s1 $viols_s1 \
  $wns_s2 $tns_s2 $viols_s2 \
  [expr {$wns_s2 - $wns_s0}] [expr {$tns_s2 - $tns_s0}] [expr {$viols_s2 - $viols_s0}] \
  $t_pass1 $t_pass2]

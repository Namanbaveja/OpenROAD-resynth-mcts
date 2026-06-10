# Test script for resynth_mcts_cones on the GCD design (ASAP7, post-CTS).
#
# Run with:
#   openroad -no_init gcd_cones_mcts_test.tcl
#
# Overridable via env vars:
#   SEED=42 ITERS=30 CORNER=slow MAX_DEPTH=5 UCB=1.414 PROX=10.0 DEBUG=1

set seed      [expr {[info exists ::env(SEED)]      ? $::env(SEED)      : 42}]
set iters     [expr {[info exists ::env(ITERS)]     ? $::env(ITERS)     : 30}]
set corner    [expr {[info exists ::env(CORNER)]    ? $::env(CORNER)    : "slow"}]
set max_depth [expr {[info exists ::env(MAX_DEPTH)] ? $::env(MAX_DEPTH) : 5}]
set ucb       [expr {[info exists ::env(UCB)]       ? $::env(UCB)       : 1.414}]
set prox      [expr {[info exists ::env(PROX)]      ? $::env(PROX)      : 10.0}]
set debug_lvl [expr {[info exists ::env(DEBUG)]     ? $::env(DEBUG)     : 1}]

# ---- load design (same as gcd_routed_qor_mcts.tcl) --------------------------
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
read_def ../../benchmark_cts/new/gcd/4_cts.def
read_sdc ../../benchmark_cts/new/gcd/4_cts.sdc

# ---- estimated wire RC (no SPEF at post-CTS stage) --------------------------
set_wire_rc -signal -resistance 1.3340e-1 -capacitance 8.6000e-2

# ---- QoR helper (same as gcd_routed_qor_mcts.tcl) ---------------------------
proc snapshot_qor { label } {
  set wns_s      0.0
  set tns_s      0.0
  set viol_count 0
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

# ---- baseline ---------------------------------------------------------------
lassign [snapshot_qor "BASELINE (before resynth_mcts_cones)"] \
        wns_before tns_before viols_before

# ---- enable debug logging ---------------------------------------------------
# cone_mcts (level 1): cone group info per iteration, per-cone MCTS result
# cone_mcts (level 2): every MCTS iteration reward (very verbose)
# gia        (level 1): individual GIA op names during RunGiaPipeline
set_debug_level RMP cone_mcts $debug_lvl
set_debug_level RMP gia       $debug_lvl

# ---- remove fillers (required before logic restructuring) -------------------
puts ""
puts ">>> Removing fillers..."
remove_fillers

set cells_before [llength [get_cells *]]
puts [format ">>> Cells before: %d" $cells_before]

# ---- run resynth_mcts_cones -------------------------------------------------
puts ""
puts ">>> Running: resynth_mcts_cones \\"
puts "      -corner       $corner \\"
puts "      -seed         $seed \\"
puts "      -iters        $iters \\"
puts "      -max_depth    $max_depth \\"
puts "      -ucb_constant $ucb \\"
puts "      -proximity_um $prox"
puts ""

set t0 [clock milliseconds]
resynth_mcts_cones \
  -corner       $corner \
  -seed         $seed \
  -iters        $iters \
  -max_depth    $max_depth \
  -ucb_constant $ucb \
  -proximity_um $prox
set elapsed_ms [expr {[clock milliseconds] - $t0}]

set cells_after  [llength [get_cells *]]
set changed      [expr {$cells_before != $cells_after ? "YES" : "NO"}]
puts ""
puts [format ">>> Netlist changed: %s  (cells_before=%d  cells_after=%d)" \
          $changed $cells_before $cells_after]
puts [format ">>> Total elapsed: %d ms" $elapsed_ms]

# ---- verify placement -------------------------------------------------------
# Use catch so a placement error doesn't abort the QoR report.
puts ""
puts ">>> Checking placement..."
if { [catch {check_placement -verbose} err] } {
  puts ">>> WARNING: check_placement reported: $err"
}

# ---- after QoR --------------------------------------------------------------
puts ""
lassign [snapshot_qor "AFTER resynth_mcts_cones"] \
        wns_after tns_after viols_after

# ---- delta ------------------------------------------------------------------
set wns_delta   [expr {$wns_after   - $wns_before}]
set tns_delta   [expr {$tns_after   - $tns_before}]
set viols_delta [expr {$viols_after - $viols_before}]

set wns_pct  [expr {$wns_before  == 0.0 ? 0.0 : 100.0 * $wns_delta   / abs($wns_before)}]
set tns_pct  [expr {$tns_before  == 0.0 ? 0.0 : 100.0 * $tns_delta   / abs($tns_before)}]
set fep_pct  [expr {$viols_before == 0   ? 0.0 : 100.0 * $viols_delta / double($viols_before)}]

proc verdict { delta positive_is_good } {
  if { $positive_is_good } {
    return [expr {$delta > 0 ? "IMPROVED" : $delta < 0 ? "REGRESSED" : "UNCHANGED"}]
  } else {
    return [expr {$delta < 0 ? "IMPROVED" : $delta > 0 ? "REGRESSED" : "UNCHANGED"}]
  }
}

puts ""
puts "===== QoR DELTA ====="
puts [format "  WNS:  %12.4f ps -> %12.4f ps   delta=%+10.4f ps (%+6.2f%%)  %s" \
          $wns_before $wns_after $wns_delta $wns_pct [verdict $wns_delta 1]]
puts [format "  TNS:  %12.4f ps -> %12.4f ps   delta=%+10.4f ps (%+6.2f%%)  %s" \
          $tns_before $tns_after $tns_delta $tns_pct [verdict $tns_delta 1]]
puts [format "  FEPs: %12d    -> %12d      delta=%+10d    (%+6.2f%%)  %s" \
          $viols_before $viols_after $viols_delta $fep_pct [verdict $viols_delta 0]]
puts [format "  TIME: %d ms  changed=%s" $elapsed_ms $changed]
puts ""

# Machine-parseable result line
puts [format \
  "RESULT seed=%s iters=%s depth=%s ucb=%s prox=%s changed=%s \
wns_before=%.4f wns_after=%.4f tns_before=%.4f tns_after=%.4f \
viols_before=%d viols_after=%d wns_delta=%.4f tns_delta=%.4f \
viols_delta=%d elapsed_ms=%d" \
  $seed $iters $max_depth $ucb $prox $changed \
  $wns_before $wns_after $tns_before $tns_after \
  $viols_before $viols_after $wns_delta $tns_delta \
  $viols_delta $elapsed_ms]

# ---- args (overridable via env vars) -----------------------------------------
set seed         [expr {[info exists ::env(SEED)]         ? $::env(SEED)         : 42}]
set iters        [expr {[info exists ::env(ITERS)]        ? $::env(ITERS)        : 300}]
set corner       [expr {[info exists ::env(CORNER)]       ? $::env(CORNER)       : "slow"}]
set pct          [expr {[info exists ::env(PERCENTAGE)]   ? $::env(PERCENTAGE)   : 20}]
set max_depth    [expr {[info exists ::env(MAX_DEPTH)]    ? $::env(MAX_DEPTH)    : 6}]
set ucb_constant [expr {[info exists ::env(UCB_CONSTANT)] ? $::env(UCB_CONSTANT) : 0.3}]
set debug_lvl    [expr {[info exists ::env(DEBUG_LEVEL)]  ? $::env(DEBUG_LEVEL)  : 1}]
set eval_mode    [expr {[info exists ::env(EVAL_MODE)]    ? $::env(EVAL_MODE)    : "full"}]
set rounds       [expr {[info exists ::env(ROUNDS)]       ? $::env(ROUNDS)       : 1}]
set sdc_file     [expr {[info exists ::env(SDC_FILE)]     ? $::env(SDC_FILE)     : "../../benchmark_cts/new/uart/4_cts.sdc"}]

# ---- design (post-CTS: DEF + SDC, no SPEF — estimated wire RC) --------------
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
read_sdc $sdc_file

# ---- wire RC model (estimated parasitics — no SPEF at this stage) -----------
set_wire_rc -signal \
    -resistance 1.3340e-1 \
    -capacitance 8.6000e-2

# ---- QoR snapshot helper ----------------------------------------------------
proc snapshot_qor { label } {
  set wns_s      ""
  set tns_s      ""
  set viol_count ""

  catch { set wns_s      [sta::worst_slack_cmd "max"] }
  catch { set tns_s      [sta::total_negative_slack_cmd "max"] }
  catch { set viol_count [sta::endpoint_violation_count "max"] }

  if { $wns_s      eq "" } { set wns_s      0.0 }
  if { $tns_s      eq "" } { set tns_s      0.0 }
  if { $viol_count eq "" } { set viol_count 0   }

  if { $wns_s > 0.0 } { set wns_s 0.0 }

  set wns_ps [expr {$wns_s * 1.0e12}]
  set tns_ps [expr {$tns_s * 1.0e12}]

  puts "===== $label ====="
  puts [format "  WNS  = %12.4f ps   (%g s)" $wns_ps $wns_s]
  puts [format "  TNS  = %12.4f ps   (%g s)" $tns_ps $tns_s]
  puts [format "  FEPs = %12d        (Failing Endpoints)" $viol_count]

  return [list $wns_ps $tns_ps $viol_count]
}

# ---- baseline ---------------------------------------------------------------
lassign [snapshot_qor "BASELINE (post-CTS, before resynth_mcts)"] \
        wns_before tns_before viols_before

set_debug_level RMP mcts     $debug_lvl
set_debug_level RMP sa_eval  $debug_lvl

# ---- run (multi-round) -------------------------------------------------------
puts ""
puts ">>> Removing any existing fillers before MCTS..."
puts ""
remove_fillers

set total_elapsed_ms 0
set mcts_changed_any "NO"

# Track per-round QoR so we can detect early stagnation
set prev_wns   $wns_before
set prev_tns   $tns_before
set prev_viols $viols_before

set stagnant_rounds 0

for {set rnd 1} {$rnd <= $rounds} {incr rnd} {
  # Vary seed each round so MCTS explores different branches
  set round_seed [expr {$seed + $rnd - 1}]

  puts ""
  puts "===== ROUND $rnd / $rounds  (seed=$round_seed) ====="
  puts ">>> Running: resynth_mcts -percentage $pct -corner $corner -seed $round_seed -iters $iters -max_depth $max_depth -ucb_constant $ucb_constant"
  puts ""

  set cells_before_rnd [llength [get_cells *]]

  set t0 [clock milliseconds]
  resynth_mcts \
    -percentage   $pct \
    -corner       $corner \
    -seed         $round_seed \
    -iters        $iters \
    -max_depth    $max_depth \
    -ucb_constant $ucb_constant \
  -eval_mode    $eval_mode
  set rnd_elapsed [expr {[clock milliseconds] - $t0}]
  set total_elapsed_ms [expr {$total_elapsed_ms + $rnd_elapsed}]

  set cells_after_rnd [llength [get_cells *]]
  if { $cells_before_rnd != $cells_after_rnd } { set mcts_changed_any "YES" }

  # Per-round snapshot
  lassign [snapshot_qor "ROUND $rnd AFTER"] rnd_wns rnd_tns rnd_viols

  set rnd_wns_d   [expr {$rnd_wns   - $prev_wns}]
  set rnd_tns_d   [expr {$rnd_tns   - $prev_tns}]
  set rnd_viols_d [expr {$rnd_viols - $prev_viols}]
  puts [format "  ROUND $rnd DELTA: WNS %+.2f ps  TNS %+.2f ps  VEP %+d  time=%dms" \
        $rnd_wns_d $rnd_tns_d $rnd_viols_d $rnd_elapsed]

  # Early stop: 2 consecutive rounds with no TNS/VEP improvement
  if { $rnd_tns_d <= 0.0 && $rnd_viols_d >= 0 } {
    incr stagnant_rounds
    if { $stagnant_rounds >= 2 } {
      puts ">>> No improvement for 2 consecutive rounds — stopping early at round $rnd."
      break
    }
  } else {
    set stagnant_rounds 0
  }

  set prev_wns   $rnd_wns
  set prev_tns   $rnd_tns
  set prev_viols $rnd_viols
}

set wns_after   $prev_wns
set tns_after   $prev_tns
set viols_after $prev_viols

puts ""
puts ">>> Running check_placement -verbose"
puts ""
check_placement -verbose

# ---- conditional file export ------------------------------------------------
# Write final DEF, Verilog and SDC unconditionally
file mkdir ../../qor_mcts/uart
set file_prefix "../../qor_mcts/uart/uart_mcts_${seed}_pct${pct}_${iters}_r${rounds}"
puts ">>> Exporting design files with prefix: $file_prefix"
write_def     "${file_prefix}.def"
write_verilog "${file_prefix}.v"
write_sdc     "${file_prefix}.sdc"

# ---- final cumulative delta vs original baseline ----------------------------
set wns_delta   [expr {$wns_after   - $wns_before}]
set tns_delta   [expr {$tns_after   - $tns_before}]
set viols_delta [expr {$viols_after - $viols_before}]

set wns_pct   [expr {$wns_before   == 0.0 ? 0.0 : 100.0 * double($wns_delta)   / abs($wns_before)}]
set tns_pct   [expr {$tns_before   == 0.0 ? 0.0 : 100.0 * double($tns_delta)   / abs($tns_before)}]
set viols_pct [expr {$viols_before == 0    ? 0.0 : 100.0 * double($viols_delta) / double($viols_before)}]

set verdict_wns   [expr {$wns_delta   > 0 ? "IMPROVED"  : $wns_delta   < 0 ? "REGRESSED" : "UNCHANGED"}]
set verdict_tns   [expr {$tns_delta   > 0 ? "IMPROVED"  : $tns_delta   < 0 ? "REGRESSED" : "UNCHANGED"}]
set verdict_viols [expr {$viols_delta < 0 ? "IMPROVED"  : $viols_delta > 0 ? "REGRESSED" : "UNCHANGED"}]

puts ""
puts "===== QoR DELTA (post-CTS resynth_mcts, $rounds round(s)) ====="
puts [format "  WNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $wns_before $wns_after $wns_delta $wns_pct $verdict_wns]
puts [format "  TNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $tns_before $tns_after $tns_delta $tns_pct $verdict_tns]
puts [format "  FEPs: %12d     ->  %12d       delta = %+10d     (%+6.2f%%)  %s" \
          $viols_before $viols_after $viols_delta $viols_pct $verdict_viols]
puts [format "  TIME: %d ms  (total across %d round(s))" $total_elapsed_ms $rounds]
puts [format "  MCTS changed netlist: %s" $mcts_changed_any]
puts ""

# One machine-parseable RESULT line (shell script greps for "^RESULT ")
puts [format \
  "RESULT seed=%s pct=%s iters=%s rounds=%s mcts_changed=%s wns_before_ps=%.4f wns_after_ps=%.4f tns_before_ps=%.4f tns_after_ps=%.4f viols_before=%d viols_after=%d wns_delta_ps=%.4f tns_delta_ps=%.4f viols_delta=%d elapsed_ms=%d" \
  $seed $pct $iters $rounds $mcts_changed_any \
  $wns_before $wns_after $tns_before $tns_after \
  $viols_before $viols_after $wns_delta $tns_delta $viols_delta $total_elapsed_ms]

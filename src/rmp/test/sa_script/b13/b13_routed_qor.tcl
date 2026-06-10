# ---- args (overridable via env vars) -----------------------------------------
set seed       [expr {[info exists ::env(SEED)]       ? $::env(SEED)       : 500}]
set iters      [expr {[info exists ::env(ITERS)]      ? $::env(ITERS)      : 300}]
set corner     [expr {[info exists ::env(CORNER)]     ? $::env(CORNER)     : "slow"}]
set pct        [expr {[info exists ::env(PERCENTAGE)] ? $::env(PERCENTAGE) : 20}]
set debug_lvl  [expr {[info exists ::env(DEBUG_LEVEL)]? $::env(DEBUG_LEVEL): 1}]
set eval_mode    [expr {[info exists ::env(EVAL_MODE)]    ? $::env(EVAL_MODE)    : "full"}]
set final_topk   [expr {[info exists ::env(FINAL_TOPK)]   ? $::env(FINAL_TOPK)   : 5}]

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
read_def ../../benchmark_cts/more_design/b13/4_cts.def
read_sdc ../../benchmark_cts/more_design/b13/4_cts.sdc

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
lassign [snapshot_qor "BASELINE (post-CTS, before resynth_annealing)"] \
        wns_before tns_before viols_before

set_debug_level RMP sa_eval   $debug_lvl
set_debug_level RMP annealing $debug_lvl

# ---- run --------------------------------------------------------------------
puts ""
puts ">>> Removing any existing fillers before annealing..."
puts ""
remove_fillers

set cells_before [llength [get_cells *]]

puts ""
puts ">>> Running: resynth_sa -percentage $pct -corner $corner -seed $seed -iters $iters"
puts ""
set t0 [clock milliseconds]
resynth_sa -percentage $pct -corner $corner -seed $seed -iters $iters -eval_mode $eval_mode -final_topk $final_topk
set elapsed_ms [expr {[clock milliseconds] - $t0}]

set cells_after [llength [get_cells *]]
set sa_changed  [expr {$cells_before != $cells_after ? "YES" : "NO"}]
puts ">>> SA netlist change detected: sa_changed=$sa_changed  (cells_before=$cells_before  cells_after=$cells_after)"

puts ""
puts ">>> Running check_placement -verbose"
puts ""
check_placement -verbose

# ---- conditional file export ------------------------------------------------
# Write final DEF and Verilog unconditionally
file mkdir ${LOG_DIR}
set file_prefix "${LOG_DIR}/b13_sa_${seed}_pct${pct}_${iters}"
puts ">>> Exporting design files with prefix: $file_prefix"
write_def     "${file_prefix}.def"
write_verilog "${file_prefix}.v"
write_sdc     "${file_prefix}.sdc"

# ---- after ------------------------------------------------------------------
puts ""
lassign [snapshot_qor "AFTER resynth_annealing"] \
        wns_after tns_after viols_after

# ---- delta ------------------------------------------------------------------
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
puts "===== QoR DELTA (post-CTS resynth_annealing) ====="
puts [format "  WNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $wns_before $wns_after $wns_delta $wns_pct $verdict_wns]
puts [format "  TNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $tns_before $tns_after $tns_delta $tns_pct $verdict_tns]
puts [format "  FEPs: %12d     ->  %12d       delta = %+10d     (%+6.2f%%)  %s" \
          $viols_before $viols_after $viols_delta $viols_pct $verdict_viols]
puts [format "  TIME: %d ms" $elapsed_ms]
puts [format "  SA changed netlist: %s" $sa_changed]
puts ""

# One machine-parseable RESULT line (shell script greps for "^RESULT ")
puts [format \
  "RESULT seed=%s pct=%s iters=%s sa_changed=%s wns_before_ps=%.4f wns_after_ps=%.4f tns_before_ps=%.4f tns_after_ps=%.4f viols_before=%d viols_after=%d wns_delta_ps=%.4f tns_delta_ps=%.4f viols_delta=%d elapsed_ms=%d" \
  $seed $pct $iters $sa_changed \
  $wns_before $wns_after $tns_before $tns_after \
  $viols_before $viols_after $wns_delta $tns_delta $viols_delta $elapsed_ms]

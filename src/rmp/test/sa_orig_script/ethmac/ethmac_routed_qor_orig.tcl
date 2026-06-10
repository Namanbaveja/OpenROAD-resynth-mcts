
# ---- args (overridable via env vars) -----------------------------------------
set seed       [expr {[info exists ::env(SEED)]       ? $::env(SEED)       : 500}]
set iters      [expr {[info exists ::env(ITERS)]      ? $::env(ITERS)      : 300}]
set corner     [expr {[info exists ::env(CORNER)]     ? $::env(CORNER)     : "slow"}]
set pct        [expr {[info exists ::env(PERCENTAGE)] ? $::env(PERCENTAGE) : 20}]
set debug_lvl  [expr {[info exists ::env(DEBUG_LEVEL)]? $::env(DEBUG_LEVEL): 1}]

# ---- design (post-CTS pre-repair: DEF + SDC, no SPEF — estimated wire RC) ---
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
read_def ../../benchmark_cts/final_cts/ethmac/4_cts.def

# reading the verilog and linking it after the def will destroy all the instances
# in the ODB making it place all the cells from scratch — do not uncomment.
# read_verilog ./uart_final.v
# link_design uart

read_sdc ../../benchmark_cts/final_cts/ethmac/4_cts.sdc

# ---- wire RC model (estimated parasitics — no SPEF at this design stage) ----
# Must be set BEFORE the baseline snapshot so both before and after measurements
# use the same parasitic model.  This is also the model SA uses internally when
# evaluating remapped cones, making the comparison apples-to-apples throughout.
set_thread_count 1
set_wire_rc -signal \
    -resistance 1.3340e-1 \
    -capacitance 8.6000e-2

# ---- numeric capture using OpenSTA's underlying commands --------------------
proc snapshot_qor { label } {
  set wns_s    ""
  set tns_s    ""
  set viol_count ""

  catch { set wns_s    [sta::worst_slack_cmd "max"] }
  catch { set tns_s    [sta::total_negative_slack_cmd "max"] }
  catch { set viol_count [sta::endpoint_violation_count "max"] }

  if { $wns_s eq "" }     { set wns_s 0.0 }
  if { $tns_s eq "" }     { set tns_s 0.0 }
  if { $viol_count eq "" } { set viol_count 0 }

  # Clamp positive WNS (no violations) to 0
  if { $wns_s > 0.0 } { set wns_s 0.0 }

  set wns_ps [expr {$wns_s * 1.0e12}]
  set tns_ps [expr {$tns_s * 1.0e12}]

  puts "===== $label ====="
  puts [format "  WNS  = %12.4f ps   (%g s)" $wns_ps $wns_s]
  puts [format "  TNS  = %12.4f ps   (%g s)" $tns_ps $tns_s]
  puts [format "  FEPs = %12d        (Failing Endpoints)" $viol_count]

  return [list $wns_ps $tns_ps $viol_count]
}

# ---- baseline (estimated wire RC — same model used throughout) --------------
lassign [snapshot_qor "BASELINE (post-CTS pre-repair, before resynth_annealing)"] \
        wns_before tns_before viols_before

# Debug level: set to 0 to suppress per-iteration messages (speeds up log
# reading; the RESULT line is always printed regardless).
set_debug_level RMP sa_eval    $debug_lvl
set_debug_level RMP annealing  $debug_lvl

# ---- run --------------------------------------------------------------------
puts ""
puts ">>> Removing any existing fillers before annealing..."
puts ""
remove_fillers

# Capture instance count before SA to detect whether SA actually changed the
# netlist vs. returning no-op (instance count is stable after remove_fillers).
set cells_before [llength [get_cells *]]

puts ""
puts ">>> Running: resynth_annealing -percentage $pct -corner $corner -seed $seed -iters $iters"
puts ""
set t0 [clock milliseconds]
resynth_annealing -percentage $pct -corner $corner -seed $seed -iters $iters
set elapsed_ms [expr {[clock milliseconds] - $t0}]

set cells_after [llength [get_cells *]]
set sa_changed  [expr {$cells_before != $cells_after ? "YES" : "NO"}]
puts ">>> SA netlist change detected: sa_changed=$sa_changed  (cells_before=$cells_before  cells_after=$cells_after)"

puts ""
# ---- post-restructure: place + parasitic + repair ----
catch {detailed_placement}
# REMOVED (crashes/asymmetric on large designs): catch {estimate_parasitics -placement}
catch {repair_timing -setup}
catch {detailed_placement}

puts ">>> Running the check_placement -verbose"
puts ""
catch {check_placement -verbose}

# ---- conditional file export ------------------------------------------------
# Set EXPORT_FILES=1 to write DEF/v/sdc/spef; off by default to keep the sweep
# fast (avoids writing large files for every seed x pct x iters combination).
# Write final DEF and Verilog unconditionally
file mkdir ../../qor_sa_orig/ethmac
set file_prefix "../../qor_sa_orig/ethmac/ethmac_sa_${seed}_pct${pct}_${iters}"
puts ">>> Exporting design files with prefix: $file_prefix"
write_def     "${file_prefix}.def"
write_verilog "${file_prefix}.v"
write_sdc     "${file_prefix}.sdc"

# ---- after (estimated wire RC, same model as baseline) ----------------------
puts ""
lassign [snapshot_qor "AFTER resynth_annealing"] \
        wns_after tns_after viols_after

# ---- delta ------------------------------------------------------------------
set wns_delta   [expr {$wns_after - $wns_before}]
set tns_delta   [expr {$tns_after - $tns_before}]
set viols_delta [expr {$viols_after - $viols_before}]

set wns_pct   [expr {$wns_before == 0.0 ? 0.0 : 100.0 * double($wns_delta) / abs($wns_before)}]
set tns_pct   [expr {$tns_before == 0.0 ? 0.0 : 100.0 * double($tns_delta) / abs($tns_before)}]
set viols_pct [expr {$viols_before == 0  ? 0.0 : 100.0 * double($viols_delta) / double($viols_before)}]

set verdict_wns   [expr {$wns_delta > 0 ? "IMPROVED" : $wns_delta < 0 ? "REGRESSED" : "UNCHANGED"}]
set verdict_tns   [expr {$tns_delta > 0 ? "IMPROVED" : $tns_delta < 0 ? "REGRESSED" : "UNCHANGED"}]
set verdict_viols [expr {$viols_delta < 0 ? "IMPROVED" : $viols_delta > 0 ? "REGRESSED" : "UNCHANGED"}]

puts ""
puts "===== QoR DELTA (post-CTS pre-repair resynth_annealing) ====="
puts [format "  WNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $wns_before $wns_after $wns_delta $wns_pct $verdict_wns]
puts [format "  TNS:  %12.4f ps  ->  %12.4f ps    delta = %+10.4f ps  (%+6.2f%%)  %s" \
          $tns_before $tns_after $tns_delta $tns_pct $verdict_tns]
puts [format "  FEPs: %12d     ->  %12d       delta = %+10d     (%+6.2f%%)  %s" \
          $viols_before $viols_after $viols_delta $viols_pct $verdict_viols]
puts [format "  TIME: %d ms" $elapsed_ms]
puts [format "  SA changed netlist: %s" $sa_changed]
puts ""

# One-line machine-parseable summary (bash script greps for "^RESULT ")
puts [format \
  "RESULT seed=%s pct=%s iters=%s sa_changed=%s wns_before_ps=%.4f wns_after_ps=%.4f tns_before_ps=%.4f tns_after_ps=%.4f viols_before=%d viols_after=%d wns_delta_ps=%.4f tns_delta_ps=%.4f viols_delta=%d elapsed_ms=%d" \
  $seed $pct $iters $sa_changed \
  $wns_before $wns_after $tns_before $tns_after \
  $viols_before $viols_after $wns_delta $tns_delta $viols_delta $elapsed_ms]

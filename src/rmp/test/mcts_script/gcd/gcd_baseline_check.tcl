# Quick baseline check: does the design have placement issues BEFORE resynth?
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

puts "=== check_placement BEFORE remove_fillers ==="
if { [catch {check_placement -verbose} e] } { puts "FAIL: $e" } else { puts "PASS" }

remove_fillers
puts "=== check_placement AFTER remove_fillers ==="
if { [catch {check_placement -verbose} e] } { puts "FAIL: $e" } else { puts "PASS" }

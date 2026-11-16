#!/usr/bin/env bash
# Stochastic lighting benchmark runner (docs/stochastic_lighting.md, "Scaling" and "Validation"). Git Bash on Windows.
# Each session launches the deployed demo on ZorahLights (raster, stochastic lighting on, frozen lights, texture mip 4,
# clodLodErrorPixels 3) and drives it over the sampling control pipe.
#
#   scripts/stochastic_scaling.sh profile [options]   moving-camera pass timings, one session per (range, scale)
#   scripts/stochastic_scaling.sh bias [options]      reference, accumulation, reference again (still camera): bias
#   scripts/stochastic_scaling.sh noise [options]     accumulation, then live undenoised frames per variant: noise
#   scripts/stochastic_scaling.sh look [options]      live denoised frames per variant as PNGs (x / (1 + x), debug output 77's
#                                                     view), for looking at artefacts; add --vars ZORAH_FREEZE_LIGHTS=0 to move lights
#   scripts/stochastic_scaling.sh pair [options]      moving camera: each frame's denoised luminance against its own noisy
#                                                     input (DENOISE_DEBUG_PAIR), per variant: the denoiser's bias and lag
#   scripts/stochastic_scaling.sh trail [options]     moving lights under a held camera (--layout, default sweep: lights skimming
#                                                     the ground): --frames (48) consecutive frames per variant, the denoised
#                                                     output against the noisy input after the temporal pass, the spatial pass
#                                                     and DLSS; stuck history
#   scripts/stochastic_scaling.sh still [options]     frozen lights (--layout, default sweep): denoised mean against the converged
#                                                     reference's and the noisy input's (stationary brightness); with --variant,
#                                                     each variant's frames before the reference and again after it, in reverse
#                                                     order (--pso defaults to the denoiser's two pipelines)
#   scripts/stochastic_scaling.sh guide [options]     frozen lights (--layout): the guide (stochasticLightingNormalize on) for
#                                                     --frames frames with jitter and without, then the unshadowed reference
#                                                     written as the guide: its stability and its truth ratio (overhaul phase 2)
#   scripts/stochastic_scaling.sh paired [options]    frozen lights (--layout; default the bias scene), the paired ratio on: --frames
#                                                     frames of DENOISE_DEBUG_VIEW 7 and the guide: E[X] / G from the noisy x and
#                                                     from the long unshadowed history H_X, overall and where it deviates (phase 3)
#   scripts/stochastic_scaling.sh dlss [options]      held camera (ZORAH_CAMERA_T, default 0: the moving-camera test's start), DLSS
#                                                     on: the reference through the whole pipeline, then --frames consecutive frames
#                                                     of the undenoised input and of each denoiser variant, each captured before
#                                                     DLSS (stochastic lighting) and after it (the upscaled HDR image), against
#                                                     the reference: error per pixel, 8x8 and 32x32 blocks, persistent and flicker
#                                                     (does not fit a 60 s session: three graph rebuilds and reference convergence)
#   scripts/stochastic_scaling.sh dlssnoise [options] held camera, frozen lights, DLSS on, no reference (fits 60 s with two variants):
#                                                     --sets sets of --frames consecutive frames per variant, before and after
#                                                     DLSS; each variant's slow (between its sets) and fast (within a set) noise
#                                                     after DLSS at pixel / 8x8 / 32x32, display-weighted. Self-referenced: no
#                                                     bias (take it from still), no moving lights. --noisy adds the undenoised
#                                                     input, --floor the per-frame reference's floor (each a graph rebuild)
#
# Options:
#   --scales "1 4 16"       ZORAH_LIGHT_SCALE values, 16 lights each (profile; default "1 4 16 64"). 0 is the sun alone.
#   --scale n               bias and noise (default 1)
#   --ranges "18 6"         ZORAH_LIGHT_RANGE values (profile; default 18)
#   --range r               bias and noise (default 18)
#   --variant 'name={...}'  pso.recompile define overrides for the --pso pipelines (every mode but bias; repeatable;
#                           default 'base={}'), e.g. --variant 'base={}' --variant 'cheap={"STOCHASTIC_CHEAP_IMPORTANCE":"1"}'.
#                           A key "setting:NAME" sets that setting instead (settings.set), e.g. {"setting:stochasticLightingDownsample":1};
#                           a setting keeps its value until a later variant sets it, so name it in every variant
#   --pso ID[,ID...]        the pipelines the variants recompile, each with the same defines (default
#                           Stochastic.Select.PSO,Stochastic.Shade.PSO: the split estimator's selection and shading; add
#                           Stochastic.Trace.PSO for the ray probes; dlss: Stochastic.DenoiseTemporal.PSO,Stochastic.DenoiseSpatial.PSO)
#   --shader 'name=PATH'    variant name runs with the shader file PATH copied over the deployed --shader-target (repeatable),
#                           for A/B of code that defines can't switch; the other variants run the deployed file, which is
#                           restored on exit. PATH must keep the shader's set of Builtin resources (pso.recompile refuses others)
#   --shader-target REL     the deployed shader --shader replaces (default shaders/Stochastic/stochasticSelect.hlsl)
#   --repeat n              run the variant list n times in each session, A B A B (profile; default 1)
#   --samples n             the profile's maximum sample count (default: the config's, 300); fewer for slow scenes
#   --frames n              live frames per variant (noise, look, pair; default 6; dlss: consecutive frames a set, default 8)
#   --sets n                dlss: sets of --frames per variant (default 4), --set-gap s seconds apart (default 1)
#   --ref-frames n          dlss: consecutive frames of the converged reference (the truth before DLSS; default 8)
#   --per-frame-frames n    dlss: consecutive frames of the reference's per-frame estimate, before and after the variants
#                           (their DLSS output, averaged, is the truth after DLSS; default 32)
#   --ref-grid g            dlss: the per-frame reference traces a g x g grid of rays per light (default 4), so the sun's
#                           penumbrae are not noise DLSS mistreats
#   --no-noisy              dlss: skip the undenoised segment
#   --noisy                 dlssnoise: add the undenoised input first (a graph rebuild each way, ~20 s)
#   --floor                 dlssnoise: add the per-frame reference (STOCHASTIC_REFERENCE_PER_FRAME, --ref-grid) last, the
#                           floor of the same metric (a graph rebuild, ~15 s)
#   --history s             dlssnoise: seconds after a variant is applied before its first set (default 3: the denoiser's
#                           and DLSS's histories)
#   --denoise               noise: keep the denoiser on for the live frames (denoised error against the accumulation)
#   --converge n            frames the reference / accumulation must hold before a capture (bias, noise; default 1500: keep bias runs short)
#   --max-wait s            the longest wait for that, in seconds (default 60)
#   --settle s              seconds after the scene loads before the first measurement (default 20)
#   --tag name              run name prefix (default ss_<mode>); runs are <tag>_r<range>_s<scale>
#   --vars "A=1 B=2"        more environment variables for the demo (applied after the defaults, so they override)
#   --keep                  keep the captures (bias, noise); by default they are deleted after the analysis
#   --exit-after s          the demo quits on its own after s seconds (default 3600), a cap on a session's length
#   --layout name           trail, still: ZORAH_LIGHT_LAYOUT (sweep, off, single, foliage, swap, flight; ZorahLights.cs)
#   --stages 3|1            trail: 3 (default) the temporal, spatial and post-DLSS stages from DENOISE_DEBUG_VIEW 5 with the
#                           HDR image (~200 MB a frame); 1 the original DENOISE_DEBUG_PAIR capture (the spatial stage alone)
#   --debug-view 6          trail: capture DENOISE_DEBUG_VIEW 6 instead (set on both denoiser pipelines; stochastic_diffuse only):
#                           the trail numbers after the temporal and spatial passes, and how many stuck blocks were reached
#                           by history from another surface (cross-surface reprojection). Its composite is not valid
#   --debug-view 8          trail: DENOISE_DEBUG_VIEW 8 likewise: the trail numbers and the light-mixture confidence (overhaul
#                           phase 4): its mean, the pixels below 0.9 and 0.5, per frame. With --vars ZORAH_FREEZE_LIGHTS=1, its
#                           false-positive rate on still lights. With visibility gradients on, the combined confidence
#   --debug-view 9          trail: as 8, with the visibility gradients' confidence alone ("Visibility gradients and paired
#                           (2026-11-17)")
#   --trigger-after s       trail --layout off or swap: seconds after a capture starts that its event is triggered (default 1)
#   --fixed-dt s            the scenario (and renderer.Update) advance s seconds a frame (--script-fixed-dt), so lights move
#                           as far per captured frame as live at 1/s fps even while captures slow the frames down
#
# Environment: DEMO_DIR (default D:/BasicRendererDemo, the deployed demo: the exe, shaders/), CTL (StatisticalSamplingControl.exe,
# default SARP's ninja-renderer-host RelWithDebInfo build), CAP_DIR (captures, default D:/stoch_cap), OUT_DIR (run files, logs
# of the table, default <BasicRenderer>/out/stochastic_scaling), CONFIG (sampling config, default config/stochastic_scaling.json),
# PYTHON (default python).
#
# The demo's BasicRenderer.launch.json fills in every variable a launch leaves unset (it can turn camera motion on), so it is
# moved aside while this runs and put back on exit. Deploy first: the demo runs what is in DEMO_DIR, not the build tree.
# Profile timings move between sessions by ~10%: compare variants within one session (--variant), not across runs.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BR_ROOT=$(cd "$HERE/.." && pwd)
DEMO_DIR=${DEMO_DIR:-/d/BasicRendererDemo}
CTL=${CTL:-$(cd "$BR_ROOT/.." && pwd)/build/ninja-renderer-host/BasicRenderer/BasicRenderer/RelWithDebInfo/StatisticalSamplingControl.exe}
CAP_DIR=${CAP_DIR:-D:/stoch_cap}
OUT_DIR=${OUT_DIR:-$BR_ROOT/out/stochastic_scaling}
CONFIG=${CONFIG:-$BR_ROOT/config/stochastic_scaling.json}
PY=${PYTHON:-python}
ANALYSIS="$HERE/stochastic_scaling_analysis.py"
SCENARIO="$BR_ROOT/Scripting/Scenarios/ZorahLights.cs"
NVPERF_BIN="$BR_ROOT/ThirdParty/NvPerf/bin/x64"

die() { echo "stochastic_scaling: $*" >&2; exit 1; }
win() { cygpath -w "$1"; }

MODE=${1:-}
[ -n "$MODE" ] && shift
case "$MODE" in profile|bias|noise|look|pair|dlss|dlssnoise|trail|still|guide|paired) ;; *) sed -n '2,90p' "$0"; exit 1 ;; esac

SCALES="1 4 16 64"; SCALE=1; RANGES=18; RANGE=18; VARIANTS=(); PSO=Stochastic.Select.PSO,Stochastic.Shade.PSO; REPEAT=1; SAMPLES=""
EXIT_AFTER=3600; FRAMES=""; REF_FRAMES=8; PER_FRAME_FRAMES=32; REF_GRID=4; SETS=4; SET_GAP=1; NOISY=1; CONVERGE=1500; MAX_WAIT=60; SETTLE=20; TAG="ss_$MODE"; XVARS=""; KEEP=0; DENOISE_LIVE=0; PAIR=0
VIEW=0; LAYOUT=sweep; STAGES=3; TRIGGER_AFTER=1; FIXED_DT=""; DEBUG_VIEW=""; FLOOR=0; HISTORY=3
[ "$MODE" = dlssnoise ] && NOISY=0
PSO_SET=0; declare -A SHADERS=(); SHADER_TARGET=shaders/Stochastic/stochasticSelect.hlsl
while [ $# -gt 0 ]; do
	case "$1" in
		--scales) SCALES=$2; shift 2 ;;
		--scale) SCALE=$2; shift 2 ;;
		--ranges) RANGES=$2; shift 2 ;;
		--range) RANGE=$2; shift 2 ;;
		--variant) VARIANTS+=("$2"); shift 2 ;;
		--pso) PSO=$2; PSO_SET=1; shift 2 ;;
		--shader) SHADERS[${2%%=*}]=${2#*=}; shift 2 ;;
		--shader-target) SHADER_TARGET=$2; shift 2 ;;
		--repeat) REPEAT=$2; shift 2 ;;
		--samples) SAMPLES=$2; shift 2 ;;
		--frames) FRAMES=$2; shift 2 ;;
		--ref-frames) REF_FRAMES=$2; shift 2 ;;
		--per-frame-frames) PER_FRAME_FRAMES=$2; shift 2 ;;
		--ref-grid) REF_GRID=$2; shift 2 ;;
		--sets) SETS=$2; shift 2 ;;
		--set-gap) SET_GAP=$2; shift 2 ;;
		--no-noisy) NOISY=0; shift ;;
		--noisy) NOISY=1; shift ;;
		--floor) FLOOR=1; shift ;;
		--history) HISTORY=$2; shift 2 ;;
		--converge) CONVERGE=$2; shift 2 ;;
		--max-wait) MAX_WAIT=$2; shift 2 ;;
		--settle) SETTLE=$2; shift 2 ;;
		--tag) TAG=$2; shift 2 ;;
		--vars) XVARS=$2; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--exit-after) EXIT_AFTER=$2; shift 2 ;;
		--denoise) DENOISE_LIVE=1; shift ;;
		--layout) LAYOUT=$2; shift 2 ;;
		--stages) STAGES=$2; shift 2 ;;
		--trigger-after) TRIGGER_AFTER=$2; shift 2 ;;
		--fixed-dt) FIXED_DT=$2; shift 2 ;;
		--debug-view) DEBUG_VIEW=$2; shift 2 ;;
		*) die "unknown option '$1'" ;;
	esac
done
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=('base={}')
[ "$MODE" = trail ] && FRAMES=${FRAMES:-48}
if [ "$MODE" = dlss ] || [ "$MODE" = dlssnoise ] || [ "$MODE" = still ]; then
	{ [ "$MODE" = dlss ] || [ "$MODE" = dlssnoise ]; } && FRAMES=${FRAMES:-8}
	[ "$PSO_SET" = 1 ] || PSO=Stochastic.DenoiseTemporal.PSO,Stochastic.DenoiseSpatial.PSO
fi
[ "$MODE" = still ] && FRAMES=${FRAMES:-16}
[ "$MODE" = guide ] && FRAMES=${FRAMES:-8}
[ "$MODE" = paired ] && FRAMES=${FRAMES:-16}
FRAMES=${FRAMES:-6}
for v in "${VARIANTS[@]}"; do case "$v" in *=\{*\}) ;; *) die "a variant is name={json}, not '$v'" ;; esac; done

[ -x "$CTL" ] || die "no controller at $CTL (set CTL)"
[ -f "$DEMO_DIR/BasicRenderer.exe" ] || die "no demo at $DEMO_DIR (set DEMO_DIR, or deploy with scripts/deploy_demo.ps1)"
[ -f "$CONFIG" ] || die "no sampling config at $CONFIG"
command -v "$PY" >/dev/null || die "no $PY"
if tasklist 2>/dev/null | grep -qi '^BasicRenderer.exe'; then die "a BasicRenderer.exe is already running; close it first"; fi
mkdir -p "$OUT_DIR" "$DEMO_DIR/logs" "$CAP_DIR"

# The demo's launch file fills unset variables; keep it out of the way for the whole run.
LAUNCH_JSON="$DEMO_DIR/BasicRenderer.launch.json"
LAUNCH_ASIDE="$DEMO_DIR/BasicRenderer.launch.json.stochastic_scaling"
[ -f "$LAUNCH_ASIDE" ] && [ ! -f "$LAUNCH_JSON" ] && mv "$LAUNCH_ASIDE" "$LAUNCH_JSON" # a previous run that died
restore_launch() { [ -f "$LAUNCH_ASIDE" ] && mv "$LAUNCH_ASIDE" "$LAUNCH_JSON"; }
# --shader: the deployed file is kept here while a variant's file stands in for it.
SHADER_DEPLOYED="$DEMO_DIR/$SHADER_TARGET"
SHADER_BACKUP="$DEMO_DIR/$SHADER_TARGET.stochastic_scaling"
[ -f "$SHADER_BACKUP" ] && mv -f "$SHADER_BACKUP" "$SHADER_DEPLOYED" # a previous run that died
restore_shader() { [ -f "$SHADER_BACKUP" ] && mv -f "$SHADER_BACKUP" "$SHADER_DEPLOYED"; return 0; }
cleanup() {
	if [ -n "${RUN:-}" ] && tasklist 2>/dev/null | grep -qi '^BasicRenderer.exe'; then finish; fi
	restore_launch
	restore_shader
}
trap cleanup EXIT
trap 'exit 130' INT TERM
[ -f "$LAUNCH_JSON" ] && mv "$LAUNCH_JSON" "$LAUNCH_ASIDE"

# A copy of the sampling config whose outputs land in OUT_DIR (the checked-in one's are relative to the demo's folder).
RUN_CONFIG="$OUT_DIR/sampling_config.json"
SUMMARY="$OUT_DIR/last_run_summary.md"
"$PY" - "$CONFIG" "$RUN_CONFIG" "$(win "$OUT_DIR")" "$SAMPLES" <<'PYEOF' || die "could not write the sampling config"
import json, os, sys
src, dst, out, samples = sys.argv[1:5]
cfg = json.load(open(src, encoding='utf-8'))
cfg['output'] = {'database': os.path.join(out, 'stochastic_scaling.sqlite'), 'last_run_summary': os.path.join(out, 'last_run_summary.md')}
if samples:
    cfg['statistics']['maximum_samples'] = int(samples)
    cfg['statistics']['minimum_samples'] = min(cfg['statistics'].get('minimum_samples', int(samples)), int(samples))
json.dump(cfg, open(dst, 'w', encoding='utf-8'), indent=1)
PYEOF

ctl() { "$CTL" --pipe "p_$RUN" --json "$@"; }
running() { tasklist 2>/dev/null | grep -qi '^BasicRenderer.exe'; }
log_file() { echo "$DEMO_DIR/logs/$RUN.log"; }

# launch NAME "VARS" [demo args]: writes a .cmd (explorer drops the environment, so the wrapper sets it) and starts it.
launch() {
	RUN=$1; local vars=$2 args=${3:-}
	local cmdfile="$OUT_DIR/$RUN.cmd"
	rm -f "$(log_file)"
	{
		echo '@echo off'
		echo "cd /d $(win "$DEMO_DIR")"
		[ -d "$NVPERF_BIN" ] && echo "set PATH=$(win "$NVPERF_BIN");%PATH%"
		for v in SARP_RT_PATH_TRACING=0 SARP_STOCHASTIC_LIGHTING=1 SARP_TEXTURE_FEEDBACK_FORCE_MIP=4 ZORAH_LOD_ERROR_PIXELS=3 \
			ZORAH_FREEZE_LIGHTS=1 $vars $XVARS; do echo "set $v"; done
		echo "set BASICRENDERER_LOG_PATH=$(win "$(log_file)")"
		echo "BasicRenderer.exe --sampling-control-pipe p_$RUN --script $(win "$SCENARIO") --exit-after-seconds $EXIT_AFTER${FIXED_DT:+ --script-fixed-dt $FIXED_DT} $args"
	} > "$cmdfile"
	cmd //c start "" explorer.exe "$(win "$cmdfile")"
	echo "== $RUN: launched ($vars)"
	# The pipe answers before the scene is in; the scenario logs its light count once the lights are placed.
	local t=0
	until ctl profile.status '{}' >/dev/null 2>&1 && grep -q "point lights" "$(log_file)" 2>/dev/null; do
		sleep 2; t=$((t + 2))
		if [ $t -gt 30 ] && ! running; then die "$RUN: the demo exited during startup (see $(log_file))"; fi
		[ $t -gt 900 ] && die "$RUN: no scene after 900 s"
	done
	grep -o "[0-9]* orbiting and [0-9]* wandering point lights ([^)]*)" "$(log_file)" | head -1
}

finish() {
	ctl shutdown >/dev/null 2>&1
	local t=0
	while running; do
		sleep 1; t=$((t + 1))
		if [ $t -gt 60 ]; then taskkill //F //IM BasicRenderer.exe >/dev/null 2>&1; fi
	done
	RUN=""
}

set_setting() { ctl settings.set "{\"name\":\"$1\",\"value\":$2}" >/dev/null || die "$RUN: settings.set $1 failed"; }

CURRENT_DEFINES='{}'
CURRENT_SHADER=""
# use_shader NAME: puts variant NAME's --shader file in place of the deployed shader (or the deployed one back). Returns 0
# when the file changed, so the pipeline must be rebuilt even if the defines didn't.
use_shader() {
	local want=${SHADERS[$1]:-}
	[ "$want" = "$CURRENT_SHADER" ] && return 1
	if [ -n "$want" ]; then
		[ -f "$want" ] || die "no shader file '$want' for variant $1"
		[ -f "$SHADER_BACKUP" ] || cp "$SHADER_DEPLOYED" "$SHADER_BACKUP"
		cp "$want" "$SHADER_DEPLOYED"
	else
		restore_shader
	fi
	CURRENT_SHADER=$want
	return 0
}
# recompile DEFINES: rebuilds $PSO with these define overrides unless they are the current ones. The controller waits for
# the job; a failure leaves the old pipeline running and is only in the demo log, so it is fatal here.
recompile() {
	local defines=$1 force=${2:-0} pso
	[ "$force" = 0 ] && [ "$defines" = "$CURRENT_DEFINES" ] && return 0
	local reply
	for pso in ${PSO//,/ }; do
		local d=$defines
		# A variant's defines replace the pipeline's whole set: keep pair's or trail's diagnostic output on the spatial pass
		# (and view 6's on the temporal pass, which hands it over).
		if [ "$pso" = Stochastic.DenoiseSpatial.PSO ]; then
			[ "$PAIR" = 1 ] && d=$("$PY" -c 'import json, sys; d = json.loads(sys.argv[1]); d["DENOISE_DEBUG_PAIR"] = "1"; print(json.dumps(d, separators=(",", ":")))' "$d" | tr -d '\r')
		fi
		if [ "$VIEW" != 0 ] && { [ "$pso" = Stochastic.DenoiseSpatial.PSO ] || { [ "$pso" = Stochastic.DenoiseTemporal.PSO ] && { [ "$VIEW" = 6 ] || [ "$VIEW" = 8 ]; }; }; }; then
			d=$("$PY" -c 'import json, sys; d = json.loads(sys.argv[1]); d["DENOISE_DEBUG_VIEW"] = sys.argv[2]; print(json.dumps(d, separators=(",", ":")))' "$d" "$VIEW" | tr -d '\r')
		fi
		reply=$(ctl pso.recompile "{\"pipeline_id\":\"$pso\",\"defines\":$d}" 2>&1)
		if ! echo "$reply" | grep -q '"state":"published"'; then
			echo "$reply" | head -c 600 >&2; echo >&2
			grep -iE "failed to rebuild|structural" "$(log_file)" | tail -3 >&2
			die "$RUN: pso.recompile $pso $defines did not publish"
		fi
	done
	CURRENT_DEFINES=$defines
	sleep 4 # let the new pipeline's first frames through before measuring
}

# apply_variant NAME JSON: the variant's "setting:NAME" keys by settings.set, the rest as define overrides for $PSO (and
# its --shader file). A setting that changes rebuilds the graph, which silently drops every pso.recompile override, so the
# defines (and pair mode's DENOISE_DEBUG_PAIR) are applied again after one.
declare -A CURRENT_SETTINGS=()
apply_variant() {
	local name=$1 json=$2 split settings defines kv key value changed=0 force=0
	split=$("$PY" -c 'import json, sys
d = json.loads(sys.argv[1])
print(" ".join(k[8:] + "=" + json.dumps(v) for k, v in d.items() if k.startswith("setting:")))
print(json.dumps({k: v for k, v in d.items() if not k.startswith("setting:")}, separators=(",", ":")))' "$json") \
		|| die "variant $name: '$json' is not JSON"
	settings=$(echo "$split" | sed -n 1p | tr -d '\r')
	defines=$(echo "$split" | sed -n 2p | tr -d '\r')
	for kv in $settings; do
		key=${kv%%=*}; value=${kv#*=}
		if [ "${CURRENT_SETTINGS[$key]:-}" != "$value" ]; then
			set_setting "$key" "$value"; CURRENT_SETTINGS[$key]=$value
			# Per-frame settings rebuild nothing, so the pipelines keep their overrides; the denoiser restarts its history.
			case $key in
				stochasticLightingNormalize|stochasticLightingPaired|stochasticLightingMixtureConfidence|stochasticLightingVisibilityGradient|enableJitter) [ "$changed" = 0 ] && changed=2 ;;
				*) changed=1 ;;
			esac
		fi
	done
	[ "$changed" = 2 ] && sleep 2
	if [ "$changed" = 1 ]; then
		sleep 8 # the graph rebuild and its pipelines
		CURRENT_DEFINES='{}'
		[ "$PAIR" = 1 ] && pair_recompile
		[ "$VIEW" != 0 ] && view_recompile
	fi
	use_shader "$name" && force=1
	recompile "$defines" "$force"
}

# pair_recompile: the spatial pass writes each frame's denoised and noisy luminance (DENOISE_DEBUG_PAIR) into stochastic_diffuse.
pair_recompile() {
	local reply
	reply=$(ctl pso.recompile '{"pipeline_id":"Stochastic.DenoiseSpatial.PSO","defines":{"DENOISE_DEBUG_PAIR":"1"}}' 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: DENOISE_DEBUG_PAIR did not publish: $(echo "$reply" | head -c 300)"
	sleep 2
}

# capture TAG COUNT: COUNT frames of the stochastic diffuse and specular signals, <run>_<tag>_<d|s>_<i>.dds.
capture() {
	local tag=$1 count=${2:-1} i t
	for i in $(seq 1 "$count"); do
		for t in diffuse specular; do
			ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${tag}_${t:0:1}_$i.dds\",\"target\":\"stochastic_$t\"}" | grep -q '"state":"published"' \
				|| die "$RUN: capture.hdr $tag $t failed (see $(log_file))"
		done
	done
}

# converge WHAT: waits until the running average (reference or accumulation, just switched on) holds --converge frames.
converge() {
	local what=$1 from t=0 held=0
	from=$(wc -l < "$(log_file)")
	while :; do
		sleep 5; t=$((t + 5))
		held=$(tail -n +"$((from + 1))" "$(log_file)" | grep -o "accumulation: [0-9]* frames held" | tail -1 | grep -o "[0-9]*")
		held=${held:-0}
		[ "$held" -ge "$CONVERGE" ] && break
		if [ $t -ge "$MAX_WAIT" ]; then echo "   $what: only $held frames after $MAX_WAIT s (restarts?)"; break; fi
	done
	echo "   $what: $(tail -n +"$((from + 1))" "$(log_file)" | grep -o "[0-9]* frames held, [0-9]* restarts[^)]*)" | tail -1)"
}

delete_captures() { [ "$KEEP" = 1 ] || rm -f "$CAP_DIR/${RUN}"_*.dds; }

mode_profile() {
	local table="$OUT_DIR/$TAG.tsv" range scale name defines rep v from dropped
	: > "$table"
	for range in $RANGES; do
		for scale in $SCALES; do
			launch "${TAG}_r${range}_s${scale}" "ZORAH_CAMERA_MOTION=profile ZORAH_LIGHT_SCALE=$scale ZORAH_LIGHT_RANGE=$range" \
				"--sampling-config $(win "$RUN_CONFIG")"
			sleep "$SETTLE"
			CURRENT_DEFINES='{}'
			for rep in $(seq 1 "$REPEAT"); do
				for v in "${VARIANTS[@]}"; do
					name=${v%%=*}; defines=${v#*=}
					local base=$name
					[ "$REPEAT" -gt 1 ] && name="$name$rep"
					apply_variant "$base" "$defines"
					from=$(wc -l < "$(log_file)")
					rm -f "$SUMMARY"
					ctl profile.run "{\"label\":\"$RUN $name\"}" > "$OUT_DIR/profile_reply.json" 2>&1
					[ -f "$SUMMARY" ] || die "$RUN: profile '$name' wrote no summary: $(head -c 400 "$OUT_DIR/profile_reply.json")"
					cp "$SUMMARY" "$OUT_DIR/${RUN}_$name.md"
					dropped=$("$PY" "$ANALYSIS" dropped "$(win "$(log_file)")" "$from")
					"$PY" "$ANALYSIS" summary "$(win "$SUMMARY")" | while IFS=$'\t' read -r pass mean; do
						printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$range" "$scale" "$name" "$dropped" "$pass" "$mean" >> "$table"
					done
					echo "   $name: $(grep -o '"stopping_reason":"[^"]*"' "$OUT_DIR/profile_reply.json"); sample" \
						"$("$PY" "$ANALYSIS" sample "$(win "$SUMMARY")") ms; dropped $dropped"
				done
			done
			finish
		done
	done
	echo
	"$PY" "$ANALYSIS" table "$(win "$table")"
	echo "(rows: $table; times in ms, mean over the profile; Dropped: most (froxel, light) pairs the light grid dropped in a sampled frame)"
}

mode_bias() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	set_setting stochasticLightingReference true; converge reference; capture R1
	set_setting stochasticLightingReference false; set_setting stochasticLightingAccumulate true; converge accumulation; capture A
	set_setting stochasticLightingAccumulate false; set_setting stochasticLightingReference true; converge reference; capture R2
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" bias "$(win "$CAP_DIR")" "$RUN"
	delete_captures
}

mode_noise() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	set_setting stochasticLightingAccumulate true; converge accumulation; capture A
	set_setting stochasticLightingAccumulate false
	[ "$DENOISE_LIVE" = 1 ] || set_setting stochasticLightingDenoise false
	sleep 10
	CURRENT_DEFINES='{}'
	local tags=() v name
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		[ "$DENOISE_LIVE" = 1 ] && sleep 3 # the history after the switch
		capture "$name" "$FRAMES"
		tags+=("$name")
	done
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" noise "$(win "$CAP_DIR")" "$RUN" "${tags[@]}"
	delete_captures
}

mode_look() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	CURRENT_DEFINES='{}'
	local tags=() v name
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		sleep 2 # the denoiser's history after the switch
		capture "$name" "$FRAMES"
		tags+=("$name")
	done
	local run=$RUN
	finish
	RUN=$run
	mkdir -p "$OUT_DIR/look"
	"$PY" "$ANALYSIS" png "$(win "$CAP_DIR")" "$RUN" "$(win "$OUT_DIR/look")" "${tags[@]}"
	delete_captures
}

mode_pair() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_CAMERA_MOTION=always ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	PAIR=1
	pair_recompile
	CURRENT_DEFINES='{}'
	local tags=() v name
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		sleep 3 # the denoiser's history after the switch
		capture "$name" "$FRAMES"
		tags+=("$name")
	done
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" pair "$(win "$CAP_DIR")" "$RUN" "${tags[@]}"
	delete_captures
}

# capture_set TAG COUNT: COUNT consecutive frames, each with stochastic lighting before DLSS (diffuse, specular) and the
# upscaled HDR image after it, as luminance (R32F): <run>_<tag>_<target>_<i>.dds.
capture_set() {
	local tag=$1 count=$2 reply
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${tag}_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\",\"stochastic_specular\",\"hdr\"],\"frames\":$count,\"luminance\":true,\"timeout_seconds\":180}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr $tag failed: $(echo "$reply" | head -c 400) (see $(log_file))"
}

# capture_sets TAG: --sets sets of --frames consecutive frames, --set-gap seconds apart (longer than the denoiser's and DLSS's
# histories), as <tag>.<set>: noise that crawls over more frames than a set shows up between the sets.
capture_sets() {
	local set
	for set in $(seq 1 "$SETS"); do
		[ "$set" -gt 1 ] && sleep "$SET_GAP"
		capture_set "$1.$set" "$FRAMES"
	done
}

# reference_per_frame: the reference outputs each frame's estimate instead of its running average (and DLSS settles on it).
reference_per_frame() {
	local reply
	reply=$(ctl pso.recompile "{\"pipeline_id\":\"Stochastic.Reference.PSO\",\"defines\":{\"STOCHASTIC_REFERENCE_PER_FRAME\":\"1\",\"STOCHASTIC_REFERENCE_RAY_GRID\":\"$REF_GRID\"}}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: STOCHASTIC_REFERENCE_PER_FRAME did not publish: $(echo "$reply" | head -c 300)"
	sleep 4
}

mode_dlss() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_CAMERA_T=0 ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	grep -q "DLSS history reset submitted" "$(log_file)" || echo "   warning: no DLSS evaluation in the log yet (Streamline off?)"
	sleep "$SETTLE"
	# R1: the converged (accumulated) reference, the truth before DLSS. P1, P2: the reference's per-frame estimate
	# (STOCHASTIC_REFERENCE_PER_FRAME), which jitters with the rest of the image, so DLSS's output of it is the truth after DLSS.
	set_setting stochasticLightingReference true; converge reference; capture_set R1 "$REF_FRAMES"
	reference_per_frame; capture_set P1 "$PER_FRAME_FRAMES"
	set_setting stochasticLightingReference false
	local tags=() v name
	if [ "$NOISY" = 1 ]; then
		set_setting stochasticLightingDenoise false; sleep 8
		capture_sets noisy; tags+=(noisy)
		set_setting stochasticLightingDenoise true
	fi
	sleep 8
	CURRENT_DEFINES='{}'
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		sleep 3 # the denoiser's and DLSS's histories after the switch
		capture_sets "$name"
		tags+=("$name")
	done
	CURRENT_DEFINES='{}'
	set_setting stochasticLightingReference true; sleep 8; reference_per_frame; capture_set P2 "$PER_FRAME_FRAMES"
	local run=$RUN
	finish
	RUN=$run
	mkdir -p "$OUT_DIR/dlss"
	"$PY" "$ANALYSIS" dlss "$(win "$CAP_DIR")" "$RUN" "$(win "$OUT_DIR/dlss")" "${tags[@]}" | tee "$OUT_DIR/dlss/$RUN.txt"
	delete_captures
}

# dlssnoise: noise after DLSS without a reference (docs/stochastic_lighting.md "Running the benchmarks"). Held camera
# (ZORAH_CAMERA_T=0 unless --vars says otherwise), frozen lights. Per variant, --sets sets of --frames consecutive frames,
# --set-gap s apart, each frame before DLSS (stochastic lighting) and after it (the upscaled HDR image). The analysis splits each
# variant's frames into fast noise (within a set) and slow noise (between its sets), which need no truth: the old dlss mode's
# headline, sqrt(slow^2 + fast^2) after DLSS, was always computed this way. What every set of a variant shares (its bias, and
# noise slower than its sets' span) is not measured: take the bias from still. The display weights come from the mean of all
# variants' frames. --noisy adds the undenoised input first, --floor the per-frame reference last (each a graph rebuild).
mode_dlssnoise() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_CAMERA_T=0 ZORAH_FREEZE_LIGHTS=1 ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	local tags=() v name
	if [ "$NOISY" = 1 ]; then
		set_setting stochasticLightingDenoise false; sleep 8
		capture_sets noisy; tags+=(noisy)
		set_setting stochasticLightingDenoise true; sleep 8
	fi
	CURRENT_DEFINES='{}'
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		sleep "$HISTORY"
		running || { echo "   the session ended before $name"; break; }
		capture_sets "$name"
		tags+=("$name")
	done
	if [ "$FLOOR" = 1 ] && running; then
		set_setting stochasticLightingReference true; sleep 8; reference_per_frame; sleep 1
		capture_sets floor; tags+=(floor)
	fi
	grep -q "DLSS history reset submitted" "$(log_file)" || echo "   warning: no DLSS evaluation in the log (Streamline off?)"
	local run=$RUN
	finish
	RUN=$run
	mkdir -p "$OUT_DIR/dlss"
	"$PY" "$ANALYSIS" dlssnoise "$(win "$CAP_DIR")" "$RUN" "${tags[@]}" | tee "$OUT_DIR/dlss/$RUN.txt"
	delete_captures
}

# view_recompile: the spatial pass writes DENOISE_DEBUG_VIEW $VIEW: 5 for trail (the composite unchanged; the noisy luminance
# and the temporal pass's result in the targets' alphas), so one capture of both targets and the HDR image has the three
# stages; 1 for still; 6 (--debug-view 6, on the temporal pass too) the cross-surface share; 8 the mixture confidence.
view_recompile() {
	local reply pso
	for pso in Stochastic.DenoiseSpatial.PSO $({ [ "$VIEW" = 6 ] || [ "$VIEW" = 8 ] || [ "$VIEW" = 9 ]; } && echo Stochastic.DenoiseTemporal.PSO); do
		reply=$(ctl pso.recompile "{\"pipeline_id\":\"$pso\",\"defines\":{\"DENOISE_DEBUG_VIEW\":\"$VIEW\"}}" 2>&1)
		echo "$reply" | grep -q '"state":"published"' || die "$RUN: DENOISE_DEBUG_VIEW $VIEW did not publish on $pso: $(echo "$reply" | head -c 300)"
	done
	sleep 2
}

# trail: moving lights under a held camera (--layout, default sweep), --frames consecutive frames per variant: history the
# denoiser keeps after the light has gone. --stages 3 (default) captures DENOISE_DEBUG_VIEW 5 with the HDR image (RGBA32F,
# ~200 MB a frame): the trail after the temporal pass, after the spatial pass, and after DLSS, all from the same frames.
# --stages 1 is the original detector (DENOISE_DEBUG_PAIR, the spatial pass only, ~65 MB a frame).
# Layouts off and swap have an event (the lights cut; A swapped for B) that the scenario starts when the file
# ZORAH_EVENT_FILE appears: each variant's capture starts, and --trigger-after (1) seconds later the file is written, so the
# event falls a few frames into the capture. Off cuts at once (ZORAH_OFF_SECONDS=0); swap fades in ZORAH_SWAP_FADE=0.1 s.
# Captures are slow (5-12 frames a second, the readbacks), and the scenario's lights move by its clock: without --fixed-dt a
# light moves 5-12 times as far per captured frame as it does live at 60 fps.
mode_trail() {
	local layout_vars="ZORAH_LIGHT_LAYOUT=$LAYOUT ZORAH_FREEZE_LIGHTS=0" event=""
	if [ "$LAYOUT" = off ] || [ "$LAYOUT" = swap ]; then
		event="$OUT_DIR/${TAG}_r${RANGE}_s${SCALE}.event"
		rm -f "$event"
		layout_vars="$layout_vars ZORAH_EVENT_FILE=$(win "$event") ZORAH_OFF_SECONDS=0 ZORAH_SWAP_FADE=0.1"
	fi
	launch "${TAG}_r${RANGE}_s${SCALE}" "$layout_vars ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	local tags=() v name reply targets='["stochastic_diffuse"]' pid analysis=trail
	if [ -n "$DEBUG_VIEW" ]; then
		case "$DEBUG_VIEW" in 6) analysis=cross ;; 8|9) analysis=confidence ;; *) die "--debug-view takes 6, 8 or 9" ;; esac
		VIEW=$DEBUG_VIEW; view_recompile
	elif [ "$STAGES" = 1 ]; then PAIR=1; pair_recompile
	else VIEW=5; view_recompile; targets='["stochastic_diffuse","stochastic_specular","hdr"]'
	fi
	CURRENT_DEFINES='{}'
	sleep "$SETTLE"
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		# The event restarts with each variant: removing the file undoes it (lights on, A back) before the next one.
		[ -n "$event" ] && rm -f "$event"
		apply_variant "$name" "${v#*=}"
		sleep 3 # the denoiser's history after the switch
		if [ -n "$event" ]; then
			ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${name}_{target}_{frame}.dds\",\"targets\":$targets,\"frames\":$FRAMES,\"timeout_seconds\":180}" \
				> "$OUT_DIR/capture_reply.json" 2>&1 &
			pid=$!
			sleep "$TRIGGER_AFTER"
			echo "$name" > "$event"
			wait "$pid"
			reply=$(cat "$OUT_DIR/capture_reply.json")
		else
			reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${name}_{target}_{frame}.dds\",\"targets\":$targets,\"frames\":$FRAMES,\"timeout_seconds\":180}" 2>&1)
		fi
		echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr $name failed: $(echo "$reply" | head -c 400)"
		tags+=("$name")
	done
	[ -n "$event" ] && grep -o "event file seen.*\|lights off\|lights back on\|swapped.*" "$(log_file)" | head -4 | sed 's/^/   scenario: /'
	local run=$RUN
	finish
	RUN=$run
	[ -n "$event" ] && rm -f "$event"
	mkdir -p "$OUT_DIR/trail"
	"$PY" "$ANALYSIS" "$analysis" "$(win "$CAP_DIR")" "$RUN" "$(win "$OUT_DIR/trail")" "${tags[@]}" | tee "$OUT_DIR/trail/$RUN.txt"
	delete_captures
}

# still: frozen lights (--layout, default sweep, held camera): the denoiser's stationary brightness. --frames (16) frames of
# DENOISE_DEBUG_VIEW 1 (the denoised, noisy and temporal-only luminance of each frame), then the reference until it holds
# --converge frames (at most --max-wait s), and --frames frames of it as luminance. Reports the denoised, temporal-only and
# noisy means over the reference's, and the displayed ratios (x / (1 + x) of 8x8 block means).
mode_still() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_LAYOUT=$LAYOUT ZORAH_FREEZE_LIGHTS=1 ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	local reply
	if [ "${#VARIANTS[@]}" -gt 1 ]; then
		# Variants: D.<name> for each before the reference, E.<name> after it in reverse order, so the scene's drift falls
		# on every variant alike.
		local v name names=() i
		VIEW=1; view_recompile; CURRENT_DEFINES='{}'
		for v in "${VARIANTS[@]}"; do
			name=${v%%=*}; names+=("$name")
			apply_variant "$name" "${v#*=}" # its recompile waits 4 s, enough for the history
			reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_D.${name}_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":$FRAMES,\"timeout_seconds\":120}" 2>&1)
			echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr D.$name failed: $(echo "$reply" | head -c 400)"
		done
		set_setting stochasticLightingReference true; converge reference
		reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_R_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\",\"stochastic_specular\"],\"frames\":$FRAMES,\"luminance\":true,\"timeout_seconds\":120}" 2>&1)
		echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr R failed: $(echo "$reply" | head -c 400)"
		# The reference's graph rebuild dropped every override.
		set_setting stochasticLightingReference false; sleep 3
		view_recompile; CURRENT_DEFINES='{}'
		for ((i = ${#VARIANTS[@]} - 1; i >= 0; i--)); do
			v=${VARIANTS[$i]}; name=${v%%=*}
			running || { echo "   the session ended before E.$name"; break; }
			apply_variant "$name" "${v#*=}"
			reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_E.${name}_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":$FRAMES,\"timeout_seconds\":120}" 2>&1)
			# A session capped by --exit-after may end here: the analysis takes the sets that exist.
			echo "$reply" | grep -q '"state":"published"' || { echo "   capture.hdr E.$name failed (session over?)"; break; }
		done
		local run=$RUN
		finish
		RUN=$run
		"$PY" "$ANALYSIS" still "$(win "$CAP_DIR")" "$RUN" "${names[@]}" | tee "$OUT_DIR/$RUN.still.txt"
		delete_captures
		return
	fi
	reply=$(ctl pso.recompile '{"pipeline_id":"Stochastic.DenoiseSpatial.PSO","defines":{"DENOISE_DEBUG_VIEW":"1"}}' 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: DENOISE_DEBUG_VIEW 1 did not publish: $(echo "$reply" | head -c 300)"
	sleep 3
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_D_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":$FRAMES,\"timeout_seconds\":120}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr D failed: $(echo "$reply" | head -c 400)"
	# The reference rebuilds the graph, which drops the override.
	set_setting stochasticLightingReference true; converge reference
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_R_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\",\"stochastic_specular\"],\"frames\":$FRAMES,\"luminance\":true,\"timeout_seconds\":120}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr R failed: $(echo "$reply" | head -c 400)"
	# The denoised frames again (D2), so the scene's drift between the two captures shows (and averages out).
	set_setting stochasticLightingReference false; sleep 3
	reply=$(ctl pso.recompile '{"pipeline_id":"Stochastic.DenoiseSpatial.PSO","defines":{"DENOISE_DEBUG_VIEW":"1"}}' 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: DENOISE_DEBUG_VIEW 1 did not publish: $(echo "$reply" | head -c 300)"
	sleep 3
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_E_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":$FRAMES,\"timeout_seconds\":120}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr D2 failed: $(echo "$reply" | head -c 400)"
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" still "$(win "$CAP_DIR")" "$RUN" | tee "$OUT_DIR/$RUN.still.txt"
	delete_captures
}

# guide: the guide-normalized mode's guide (docs/stochastic_lighting.md "Guide-normalized accumulation"), frozen lights and camera:
# --frames (8) consecutive frames with the upscaler's jitter (G), the same without it (J), then the reference with
# STOCHASTIC_REFERENCE_UNSHADOWED 2 (no rays, written in the guide's domain; U).
mode_guide() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_LAYOUT=$LAYOUT ZORAH_FREEZE_LIGHTS=1 ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	local reply tag
	set_setting stochasticLightingNormalize true; sleep 2
	for tag in G J; do
		[ "$tag" = J ] && { set_setting enableJitter false; sleep 2; }
		reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${tag}_{target}_{frame}.dds\",\"targets\":[\"stochastic_guide\"],\"frames\":$FRAMES,\"timeout_seconds\":60}" 2>&1)
		echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr $tag failed: $(echo "$reply" | head -c 400)"
	done
	set_setting stochasticLightingReference true; sleep 8
	PSO=Stochastic.Reference.PSO; CURRENT_DEFINES='{}'; recompile '{"STOCHASTIC_REFERENCE_UNSHADOWED":"2"}'
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_U_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":2,\"timeout_seconds\":60}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr U failed: $(echo "$reply" | head -c 400)"
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" guide "$(win "$CAP_DIR")" "$RUN" | tee "$OUT_DIR/$RUN.guide.txt"
	delete_captures
}

# paired: the paired ratio's denominator (docs/stochastic_lighting.md "The paired ratio"), frozen lights and camera, normalize
# and paired on: --frames (16) consecutive frames of DENOISE_DEBUG_VIEW 7 (the unshadowed history H_X, before and after the
# spatial filter, and this frame's noisy x with the guide where it was taken) and the guide itself. --layout sweep etc. picks
# a layout; by default the bias scene (16 lights x --scale).
mode_paired() {
	local layout_vars="ZORAH_FREEZE_LIGHTS=1"
	[ "$LAYOUT" != sweep ] && layout_vars="$layout_vars ZORAH_LIGHT_LAYOUT=$LAYOUT"
	launch "${TAG}_r${RANGE}_s${SCALE}" "$layout_vars ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	local reply
	set_setting stochasticLightingNormalize true; set_setting stochasticLightingPaired true; sleep 2
	VIEW=7; view_recompile; sleep 3 # the history's 32 frames
	reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_P_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\",\"stochastic_specular\",\"stochastic_guide\"],\"frames\":$FRAMES,\"timeout_seconds\":120}" 2>&1)
	echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr P failed: $(echo "$reply" | head -c 400)"
	local run=$RUN
	finish
	RUN=$run
	"$PY" "$ANALYSIS" paired "$(win "$CAP_DIR")" "$RUN" "$(win "$OUT_DIR")" | tee "$OUT_DIR/$RUN.paired.txt"
	delete_captures
}

"mode_$MODE"

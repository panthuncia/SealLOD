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
#   scripts/stochastic_scaling.sh trail [options]     the sweep scene (lights skimming the ground under a held overhead camera):
#                                                     --frames (48) consecutive denoised/noisy frames per variant; stuck history
#   scripts/stochastic_scaling.sh dlss [options]      held camera (ZORAH_CAMERA_T, default 0: the moving-camera test's start), DLSS
#                                                     on: the reference through the whole pipeline, then --frames consecutive frames
#                                                     of the undenoised input and of each denoiser variant, each captured before
#                                                     DLSS (stochastic lighting) and after it (the upscaled HDR image), against
#                                                     the reference: error per pixel, 8x8 and 32x32 blocks, persistent and flicker
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
#   --denoise               noise: keep the denoiser on for the live frames (denoised error against the accumulation)
#   --converge n            frames the reference / accumulation must hold before a capture (bias, noise; default 1500: keep bias runs short)
#   --max-wait s            the longest wait for that, in seconds (default 60)
#   --settle s              seconds after the scene loads before the first measurement (default 20)
#   --tag name              run name prefix (default ss_<mode>); runs are <tag>_r<range>_s<scale>
#   --vars "A=1 B=2"        more environment variables for the demo (applied after the defaults, so they override)
#   --keep                  keep the captures (bias, noise); by default they are deleted after the analysis
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
case "$MODE" in profile|bias|noise|look|pair|dlss|trail) ;; *) sed -n '2,60p' "$0"; exit 1 ;; esac

SCALES="1 4 16 64"; SCALE=1; RANGES=18; RANGE=18; VARIANTS=(); PSO=Stochastic.Select.PSO,Stochastic.Shade.PSO; REPEAT=1; SAMPLES=""
FRAMES=""; REF_FRAMES=8; PER_FRAME_FRAMES=32; REF_GRID=4; SETS=4; SET_GAP=1; NOISY=1; CONVERGE=1500; MAX_WAIT=60; SETTLE=20; TAG="ss_$MODE"; XVARS=""; KEEP=0; DENOISE_LIVE=0; PAIR=0
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
		--converge) CONVERGE=$2; shift 2 ;;
		--max-wait) MAX_WAIT=$2; shift 2 ;;
		--settle) SETTLE=$2; shift 2 ;;
		--tag) TAG=$2; shift 2 ;;
		--vars) XVARS=$2; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--denoise) DENOISE_LIVE=1; shift ;;
		*) die "unknown option '$1'" ;;
	esac
done
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=('base={}')
[ "$MODE" = trail ] && FRAMES=${FRAMES:-48}
if [ "$MODE" = dlss ]; then
	FRAMES=${FRAMES:-8}
	[ "$PSO_SET" = 1 ] || PSO=Stochastic.DenoiseTemporal.PSO,Stochastic.DenoiseSpatial.PSO
fi
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
		echo "BasicRenderer.exe --sampling-control-pipe p_$RUN --script $(win "$SCENARIO") --exit-after-seconds 3600 $args"
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
		reply=$(ctl pso.recompile "{\"pipeline_id\":\"$pso\",\"defines\":$defines}" 2>&1)
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
			set_setting "$key" "$value"; CURRENT_SETTINGS[$key]=$value; changed=1
		fi
	done
	if [ "$changed" = 1 ]; then
		sleep 8 # the graph rebuild and its pipelines
		CURRENT_DEFINES='{}'
		[ "$PAIR" = 1 ] && pair_recompile
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

# trail: the sweep scene (ZORAH_LIGHT_LAYOUT=sweep, moving lights, held overhead camera), --frames consecutive frames of
# DENOISE_DEBUG_PAIR (denoised and noisy luminance) per variant: history the denoiser keeps after a light has passed.
mode_trail() {
	launch "${TAG}_r${RANGE}_s${SCALE}" "ZORAH_LIGHT_LAYOUT=sweep ZORAH_FREEZE_LIGHTS=0 ZORAH_LIGHT_SCALE=$SCALE ZORAH_LIGHT_RANGE=$RANGE"
	sleep "$SETTLE"
	PAIR=1
	pair_recompile
	CURRENT_DEFINES='{}'
	local tags=() v name reply
	for v in "${VARIANTS[@]}"; do
		name=${v%%=*}
		apply_variant "$name" "${v#*=}"
		sleep 3 # the denoiser's history after the switch
		reply=$(ctl capture.hdr "{\"path\":\"$CAP_DIR/${RUN}_${name}_{target}_{frame}.dds\",\"targets\":[\"stochastic_diffuse\"],\"frames\":$FRAMES,\"timeout_seconds\":180}" 2>&1)
		echo "$reply" | grep -q '"state":"published"' || die "$RUN: capture.hdr $name failed: $(echo "$reply" | head -c 400)"
		tags+=("$name")
	done
	local run=$RUN
	finish
	RUN=$run
	mkdir -p "$OUT_DIR/trail"
	"$PY" "$ANALYSIS" trail "$(win "$CAP_DIR")" "$RUN" "$(win "$OUT_DIR/trail")" "${tags[@]}" | tee "$OUT_DIR/trail/$RUN.txt"
	delete_captures
}

"mode_$MODE"

#!/usr/bin/env bash
#
# push.sh — KegWasher dual-target push helper
#
# Pushes updates to the two KegWasher targets:
#   * FIRMWARE: the ClearCore (SAME53) sketch, via the verified teknic flash.sh
#   * DISPLAY:  the 4 compiled ViSi-Genie files, onto the gen4-uLCD-43DT's uSD
#
# ============================ NEVER-GUESS POLICY ============================
# This script ONLY runs commands that an investigation probe VERIFIED against
# real evidence. For every step that a probe marked UNVERIFIED or GUI-only,
# this script PRINTS the exact instruction and PAUSES (read -p) instead of
# running a fabricated command. Each such pause cites WHY it is unverified.
# Do not "helpfully" replace a pause with a guessed command.
# ============================================================================
#
# Verified-fact provenance (file:line / command output) is cited inline at
# each step so the human can audit the claim.

set -euo pipefail

# ----------------------------------------------------------------------------
# Constants — every value below was confirmed by reading a real file or by a
# read-only command. Provenance cited in comments.
# ----------------------------------------------------------------------------

# Project (sketch) dir. `find` confirmed KegWasher.ino lives here; dir basename
# == ino basename, satisfying the Arduino convention flash.sh/arduino-cli need.
PROJECT_DIR="/home/buzz/dev/KegWasher"

# The verified flash wrapper. flash.sh:10 "Usage: flash.sh <sketch_dir> [port]";
# flash.sh:42 PORT default "/dev/ttyACM0"; flash.sh:44 FQBN ClearCore:sam:clearcore.
FLASH_SH="/home/buzz/dev/teknic-clearcore-cli/scripts/flash.sh"

# The four compiled display artifacts (all confirmed present in PROJECT_DIR).
# KegWasher.cfg lists ONLY the three media files (KEGW~UCD.*), NOT the .4XE —
# media copy to FAT uSD verbatim (already 8.3 names); the .4XE program is the
# one renamed per the Workshop4 Bank dropdown.
PROGRAM_4XE="${PROJECT_DIR}/KegWasher.4XE"
MEDIA_GCI="${PROJECT_DIR}/KEGW~UCD.gci"
MEDIA_DAT="${PROJECT_DIR}/KEGW~UCD.dat"
MEDIA_TXF="${PROJECT_DIR}/KEGW~UCD.txf"

# Staleness guard source: if KegWasher.4XE is OLDER than KegWasher.4DGenie, the
# .4DGenie was edited after the last compile and the .4XE on disk is stale —
# a recompile (Workshop4 GUI) is needed before pushing. (Recompile is GUI-only:
# no verified headless .4DGenie->.4XE codegen tool exists — doc-inventory gap.)
GENIE_PROJECT="${PROJECT_DIR}/KegWasher.4DGenie"

# Wine FileTransfer.exe — has a fully documented CLI (verified via UTF-16LE
# strings: /s send, /cCOMx port, /b baud, /q quiet, /v verify, /e exit). Present
# on disk (3585960 B). COM33 -> /dev/ttyUSB0 (the display CP2104) is verified in
# ~/.wine32/dosdevices/. NOTE: exact positional arg ordering of FileTransfer's
# CLI under Wine was NOT executed/verified -> treated as instruction+pause below.
WINEPREFIX_DIR="/home/buzz/.wine32"
FILETRANSFER_EXE="${WINEPREFIX_DIR}/drive_c/Program Files/4D Labs/4D Workshop 4 IDE/DEP/FileTransfer.exe"
DISPLAY_WINE_COM="COM33"   # dosdevices: com33 -> /dev/ttyUSB0 (CP2104 display)

# Teknic ClearCore USB VID. flash.sh:70 uses `lsusb | /2890:/` as the
# authoritative discriminator (8022=app mode, 0022=bootloader).
CLEARCORE_VID="2890"

# ----------------------------------------------------------------------------
# Pretty-printing helpers
# ----------------------------------------------------------------------------
banner() {
    printf '\n'
    printf '============================================================\n'
    printf '  %s\n' "$*"
    printf '============================================================\n'
}
info()  { printf '  [info] %s\n' "$*"; }
warn()  { printf '  [WARN] %s\n' "$*" >&2; }
err()   { printf '  [ERROR] %s\n' "$*" >&2; }

# A pause for an UNVERIFIED or GUI-only step. Prints the instruction, then waits.
pause_for_manual_step() {
    printf '\n'
    printf '  ----- MANUAL / UNVERIFIED STEP -------------------------\n'
    printf '%s\n' "$*"
    printf '  --------------------------------------------------------\n'
    read -r -p "  Press Enter when you have completed the above (or Ctrl-C to abort)... " _
}

confirm() {
    # $1 = prompt. Returns 0 on yes, 1 on no. Defaults to NO.
    local reply
    read -r -p "  $1 [y/N] " reply
    case "${reply}" in
        [yY]|[yY][eE][sS]) return 0 ;;
        *) return 1 ;;
    esac
}

# ----------------------------------------------------------------------------
# Usage
# ----------------------------------------------------------------------------
usage() {
    cat <<'EOF'
push.sh — KegWasher dual-target push helper

USAGE:
    push.sh [--firmware] [--display] [--all] [--help]

TARGETS:
    --firmware   Compile + flash the ClearCore sketch via the verified
                 teknic flash.sh. (Board MUST be plugged in; the script
                 refuses to flash if no Teknic VID 2890 device enumerates.)

    --display    Push the 4 compiled ViSi-Genie files to the gen4-uLCD-43DT.
                 If the uSD card is mounted in a PC reader, copies the 3 media
                 files verbatim and (after you confirm the rename) the program
                 file. If the card is NOT mounted (it's in the display), prints
                 instructions for the two verified alternatives:
                   (a) put the card in a PC reader and re-run, OR
                   (b) push over the serial cable via Workshop4 GUI File
                       Transfer / FileTransfer.exe.

    --all        Do --firmware then --display.

    --help       Show this help.

NOTES (NEVER-GUESS):
  * Recompiling KegWasher.4DGenie -> KegWasher.4XE + KEGW~UCD.* is GUI-only
    (no verified headless codegen tool). This script does NOT recompile; it
    only pushes whatever compiled files are already on disk, and warns if they
    look stale relative to the .4DGenie source.
  * The SD program filename is RunBank1.4xe (verified 2026-05-28 via the
    Workshop4 Copy Confirmation dialog + the on-display File Transfer listing).
    The script still lets you override it for projects with a different Bank.
EOF
}

# ----------------------------------------------------------------------------
# Preconditions
# ----------------------------------------------------------------------------
require_file() {
    # $1 = path, $2 = human description
    if [[ ! -e "$1" ]]; then
        err "Required ${2} not found: $1"
        exit 1
    fi
}

# ----------------------------------------------------------------------------
# Staleness guard
#   Returns 0 if compiled .4XE is at least as new as the .4DGenie source.
#   Returns 1 (stale) if .4XE is OLDER than .4DGenie (source edited since build).
# ----------------------------------------------------------------------------
display_files_fresh() {
    require_file "${PROGRAM_4XE}" "compiled program (KegWasher.4XE)"
    require_file "${GENIE_PROJECT}" "Genie project (KegWasher.4DGenie)"

    local xe_mtime genie_mtime
    xe_mtime="$(stat -c '%Y' "${PROGRAM_4XE}")"
    genie_mtime="$(stat -c '%Y' "${GENIE_PROJECT}")"

    info "KegWasher.4XE     mtime: $(stat -c '%y' "${PROGRAM_4XE}")"
    info "KegWasher.4DGenie mtime: $(stat -c '%y' "${GENIE_PROJECT}")"

    if (( xe_mtime < genie_mtime )); then
        return 1
    fi
    return 0
}

# ----------------------------------------------------------------------------
# FIRMWARE target
# ----------------------------------------------------------------------------
push_firmware() {
    banner "TARGET: FIRMWARE (ClearCore SAME53)"

    require_file "${FLASH_SH}" "flash wrapper (flash.sh)"
    require_file "${PROJECT_DIR}/KegWasher.ino" "sketch entry point (KegWasher.ino)"

    # --- Verified discriminator: refuse to flash unless the Teknic board is on
    #     the USB bus. flash.sh:70 greps `lsusb` for the 2890 VID; `lsusb | grep
    #     2890` returns nonzero when no ClearCore is present (verified: exit 1).
    info "Checking for Teknic ClearCore (USB VID ${CLEARCORE_VID})..."
    if ! lsusb 2>/dev/null | grep -q "${CLEARCORE_VID}:"; then
        err "No Teknic ClearCore (VID ${CLEARCORE_VID}) on the USB bus."
        err "The board is not connected. Plug the ClearCore into USB (USB-C on"
        err "the front of the board) and re-run with --firmware."
        return 1
    fi
    info "Found a Teknic VID ${CLEARCORE_VID} device on the bus:"
    lsusb 2>/dev/null | grep "${CLEARCORE_VID}:" | sed 's/^/    /'

    # --- Port disambiguation. We do NOT hardcode /dev/ttyACM0: the live ACM
    #     number is UNVERIFIED (no board was connected during the probe). We
    #     enumerate the ACM nodes and let flash.sh default only when there is
    #     exactly one. (flash.sh:42 default is /dev/ttyACM0.)
    local acm_nodes=()
    # shellcheck disable=SC2231
    for n in /dev/ttyACM*; do
        [[ -e "$n" ]] && acm_nodes+=("$n")
    done

    local port_arg=()
    if (( ${#acm_nodes[@]} == 0 )); then
        # The board reported on VID 2890 but no ACM node yet — it may be in
        # bootloader mode, or udev/ModemManager may be racing. This is not
        # something we can safely auto-resolve; defer to the user.
        warn "Teknic VID present but no /dev/ttyACM* node exists yet."
        warn "The Teknic by-id symlink string is UNVERIFIED (board was unplugged"
        warn "during investigation), so the script will not guess a node."
        pause_for_manual_step \
"Identify the ClearCore serial node, then note it:
    ls -l /dev/serial/by-id/ | grep -i teknic
    lsusb | grep ${CLEARCORE_VID}
If a stable /dev/clearcore symlink exists (udev rule installed), prefer it.
Also confirm ModemManager is masked (it can break the bossac handshake).
When a /dev/ttyACM* (or /dev/clearcore) node is present, press Enter."
        # Re-scan after the pause.
        acm_nodes=()
        for n in /dev/ttyACM*; do
            [[ -e "$n" ]] && acm_nodes+=("$n")
        done
        if [[ -e /dev/clearcore ]]; then
            info "Using stable symlink /dev/clearcore."
            port_arg=("/dev/clearcore")
        fi
    fi

    if (( ${#port_arg[@]} == 0 )); then
        if (( ${#acm_nodes[@]} == 1 )); then
            info "Exactly one ACM node present: ${acm_nodes[0]} — passing it explicitly."
            port_arg=("${acm_nodes[0]}")
        elif (( ${#acm_nodes[@]} > 1 )); then
            warn "More than one /dev/ttyACM* node present:"
            printf '    %s\n' "${acm_nodes[@]}"
            warn "Cannot auto-pick the ClearCore (ACM numbering is ambiguous and"
            warn "the Teknic by-id string is UNVERIFIED). Disambiguate manually."
            pause_for_manual_step \
"Find which ACM node is the ClearCore (Teknic VID ${CLEARCORE_VID}):
    ls -l /dev/serial/by-id/ | grep -i teknic
Then re-run the firmware push and pass it explicitly, e.g.:
    ${FLASH_SH} ${PROJECT_DIR} /dev/ttyACMx
Aborting the automated firmware flash now."
            return 1
        fi
    fi

    # --- The verified flash invocation. flash.sh runs preflight -> arduino-cli
    #     compile -> 1200-baud bootloader touch -> bossac -> PID-return verify,
    #     with no human interaction (flash.sh:146-202). Exit codes 0-5 are the
    #     header contract (flash.sh:30-35).
    banner "Running verified flash.sh"
    info "Command: ${FLASH_SH} ${PROJECT_DIR} ${port_arg[*]:-(default /dev/ttyACM0)}"

    set +e
    "${FLASH_SH}" "${PROJECT_DIR}" "${port_arg[@]}"
    local rc=$?
    set -e

    case "${rc}" in
        0) info "Firmware flash SUCCEEDED (flash.sh exit 0)." ;;
        1) err "flash.sh exit 1: preflight / compile failure." ;;
        2) err "flash.sh exit 2: device not detected." ;;
        3) err "flash.sh exit 3: bootloader entry failed." ;;
        4) err "flash.sh exit 4: bossac flash failed after retries." ;;
        5) err "flash.sh exit 5: device did not return to app mode after flash." ;;
        *) err "flash.sh exit ${rc}: unrecognized code." ;;
    esac
    return "${rc}"
}

# ----------------------------------------------------------------------------
# DISPLAY target
# ----------------------------------------------------------------------------

# Try to locate a mounted uSD card. The probe verified the card is NOT mounted
# right now and that the exact mount path/label (/run/media/buzz/4DGENIE) is
# from memory, NOT currently observed. So we discover dynamically rather than
# trusting a hardcoded path: scan /run/media/$USER/* and /media/$USER/* for a
# writable removable volume.
find_sd_mount() {
    local base candidate
    for base in "/run/media/${USER}" "/media/${USER}" "/run/media" "/media"; do
        [[ -d "${base}" ]] || continue
        for candidate in "${base}"/*; do
            [[ -d "${candidate}" ]] || continue
            # Must be writable to be a viable push target.
            [[ -w "${candidate}" ]] || continue
            printf '%s\n' "${candidate}"
        done
    done
}

push_display() {
    banner "TARGET: DISPLAY (gen4-uLCD-43DT ViSi-Genie files)"

    # Verify all four artifacts exist.
    require_file "${PROGRAM_4XE}" "compiled program (KegWasher.4XE)"
    require_file "${MEDIA_GCI}"  "media (KEGW~UCD.gci)"
    require_file "${MEDIA_DAT}"  "media (KEGW~UCD.dat)"
    require_file "${MEDIA_TXF}"  "media (KEGW~UCD.txf)"

    # --- Staleness guard (REQUIRED before any push). ---
    banner "Staleness check"
    if display_files_fresh; then
        info "KegWasher.4XE is newer than (or same age as) KegWasher.4DGenie — OK."
    else
        warn "STALE: KegWasher.4XE is OLDER than KegWasher.4DGenie."
        warn "The Genie source was edited after the last compile, so the .4XE on"
        warn "disk does NOT reflect the current source. Recompiling .4DGenie ->"
        warn ".4XE + KEGW~UCD.* is a Workshop4 GUI step (no verified headless"
        warn "codegen tool exists). You should recompile in Workshop4 first."
        if ! confirm "Push the STALE display files anyway?"; then
            err "Refusing to push stale display files. Recompile in Workshop4, then re-run."
            return 1
        fi
        warn "Proceeding with STALE files at user's explicit request."
    fi

    # --- Locate the uSD card. ---
    banner "Locating uSD card"
    local mounts mount_target=""
    mapfile -t mounts < <(find_sd_mount)

    if (( ${#mounts[@]} == 0 )); then
        warn "No mounted, writable removable volume found."
        warn "The probe verified the card is normally in the DISPLAY, not the PC."
        cat <<EOF

  Two VERIFIED alternatives to get the files onto the display's uSD:

  (a) SD-IN-PC-READER (simplest, fully scriptable here):
      Eject the uSD from the display, insert it into this PC's card reader,
      wait for it to auto-mount, then re-run:
          ${BASH_SOURCE[0]} --display

  (b) SERIAL-CABLE PUSH via the display's CP2104 (card stays in the display):
      This is GUI-or-CLI, but the FileTransfer.exe CLI argument ORDERING under
      Wine is UNVERIFIED, so this script will not run it blindly. Use whichever
      you trust:
        * Workshop4 GUI -> "File Transfer" tool (no verified menu path in docs;
          use the tool you already know from your Workshop4 session), OR
        * FileTransfer.exe CLI (template only — verify before relying):
            WINEPREFIX=${WINEPREFIX_DIR} wine \\
              "${FILETRANSFER_EXE}" \\
              <PCfile> <DisplayFile> /c${DISPLAY_WINE_COM} /s /b115200 /q /e
          (verified flags: /s send, /cCOMx port, /b baud, /q quiet, /e exit;
           ${DISPLAY_WINE_COM} -> /dev/ttyUSB0 is the display, per dosdevices.
           CAUTION: the same UART-contention caveat as the PmmC flash applies —
           keep the ClearCore CAT5/serial off this line during the transfer.)
EOF
        pause_for_manual_step \
"Either insert the uSD into this PC's reader (then re-run with --display),
or perform the serial-cable / GUI File Transfer yourself using the verified
template above. This script will NOT fabricate a FileTransfer.exe invocation."
        return 0
    fi

    if (( ${#mounts[@]} == 1 )); then
        mount_target="${mounts[0]}"
        info "Found one candidate uSD mount: ${mount_target}"
        if ! confirm "Use ${mount_target} as the uSD target?"; then
            err "User declined the mount target. Aborting display push."
            return 1
        fi
    else
        warn "Multiple writable removable volumes found:"
        local i=1
        for m in "${mounts[@]}"; do
            printf '    [%d] %s\n' "$i" "$m"
            ((i++))
        done
        warn "Cannot auto-pick (mount label/path is UNVERIFIED — not observed by probe)."
        pause_for_manual_step \
"Multiple candidate volumes are mounted. Re-run with only the uSD card
inserted, or copy the files manually to the correct volume. The four files are:
    ${PROGRAM_4XE}   (program -> rename per Workshop4 Bank dropdown; see below)
    ${MEDIA_GCI}
    ${MEDIA_DAT}
    ${MEDIA_TXF}
Aborting the automated copy to avoid writing to the wrong volume."
        return 1
    fi

    # --- Copy the THREE media files verbatim (verified: pure cp; 8.3 names
    #     already; KegWasher.cfg lists exactly these three). ---
    banner "Copying media files (verbatim) to ${mount_target}"
    for f in "${MEDIA_GCI}" "${MEDIA_DAT}" "${MEDIA_TXF}"; do
        info "cp $(basename "$f")  ->  ${mount_target}/"
        cp -v "$f" "${mount_target}/"
    done

    # --- The PROGRAM file rename target "RunBank1.4xe" was VERIFIED this session
    #     (2026-05-28): Workshop4's Copy Confirmation dialog stated
    #     "KegWasher.4XE(as RunBank1.4xe)" and the File-Transfer-via-cable file
    #     listing on the display showed RUNBANK1.4XE. 8.3-compliant. Still prompt
    #     so a project with a different Bank dropdown can override, but the
    #     default is trustworthy. ---
    banner "Copying the program file (.4XE) as RunBank1.4xe"
    info "Target name RunBank1.4xe verified 2026-05-28 (Copy Confirmation dialog"
    info "+ File Transfer listing). Override only if your Bank dropdown differs."

    local default_name="RunBank1.4xe"
    local target_name=""
    read -r -p "  SD program filename (8.3) [default ${default_name}, or 'skip']: " target_name
    target_name="${target_name:-${default_name}}"

    if [[ "${target_name}" == "skip" ]]; then
        warn "Skipping the .4XE copy at user request (media files were copied)."
    else
        info "cp $(basename "${PROGRAM_4XE}")  ->  ${mount_target}/${target_name}"
        cp -v "${PROGRAM_4XE}" "${mount_target}/${target_name}"
    fi

    # --- Flush and remind about the one-time bootstrap + power cycle. ---
    sync
    info "sync complete."

    cat <<'EOF'

  REMINDER (steady-state iteration model):
    After the files are on the uSD, eject it and reinsert into the display,
    then POWER-CYCLE the display. On boot, the Bank-0 "Update Bank(s) and Run"
    stub checks the uSD file dates and auto-copies/runs them.

    This "just swap SD + power-cycle" loop ONLY works if BOTH one-time bootstrap
    flashes have already been done (Bank 0 "Update Bank(s) and Run" + Bank 5
    inherent widgets) AND the project's Destination is actually uSD. Both are
    UNVERIFIED open questions — see the script's closing notes / probe reports.
EOF
}

# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
main() {
    local do_firmware=0 do_display=0

    if (( $# == 0 )); then
        usage
        exit 0
    fi

    while (( $# > 0 )); do
        case "$1" in
            --firmware) do_firmware=1 ;;
            --display)  do_display=1 ;;
            --all)      do_firmware=1; do_display=1 ;;
            --help|-h)  usage; exit 0 ;;
            *) err "Unknown argument: $1"; usage; exit 2 ;;
        esac
        shift
    done

    local overall_rc=0

    if (( do_firmware )); then
        if ! push_firmware; then
            overall_rc=1
            warn "Firmware push did not complete successfully."
        fi
    fi

    if (( do_display )); then
        if ! push_display; then
            overall_rc=1
            warn "Display push did not complete successfully."
        fi
    fi

    banner "DONE"
    if (( overall_rc == 0 )); then
        info "All requested targets completed (or paused for verified manual steps)."
    else
        warn "One or more targets reported a problem; review the log above."
    fi
    exit "${overall_rc}"
}

main "$@"

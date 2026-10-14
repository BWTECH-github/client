#!/usr/bin/env bash
# Sync-Ende-zu-Ende-Suite: treibt owncloudcmd gegen einen echten owncloud.online-Server
# und prüft das Ergebnis unabhängig vom Client (WebDAV über davtool.py, lokales
# Dateisystem, SHA-256 beider Seiten).
#
# Aufruf:
#   OCMD=/pfad/owncloudcmd OC_URL=http://127.0.0.1:8080 OC_USER=e2e OC_PASS=… \
#   E2E_OUT=/tmp/e2e bash test/e2e/sync/run-suite.sh [Fallnummer …]
#
# Jeder Fall arbeitet in einem eigenen lokalen Ordner und einem eigenen Serverordner
# /e2e-<nr>; ein Absturz in einem Fall beeinflusst die anderen nicht.
#
# Ergebnis: $E2E_OUT/results.md (Tabelle), $E2E_OUT/results.tsv,
#           $E2E_OUT/logs/<nr>-<lauf>.log (owncloudcmd-Protokolle mit HTTP-Logger).
# Exit: 0 alle PASS, 1 mindestens ein FAIL, 2 Suite-/Umgebungsfehler (ERROR).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
: "${OCMD:?OCMD (Pfad zu owncloudcmd) fehlt}" "${OC_URL:?}" "${OC_USER:?}" "${OC_PASS:?}"
export OC_URL OC_USER OC_PASS
OUT=${E2E_OUT:-$PWD/e2e-out}
WORK=$OUT/work
LOGS=$OUT/logs
RUN_TIMEOUT=${E2E_RUN_TIMEOUT:-600}
export QT_FORCE_STDERR_LOGGING=1
# REQUEST-/RESPONSE-Zeilen des HTTP-Loggers sind die Grundlage für Bulk-, Chunk- und
# Übertragungsprüfungen; ohne diese Regel loggt die Kategorie nur Warnungen.
export QT_LOGGING_RULES=${QT_LOGGING_RULES:-sync.httplogger.info=true}
# Feste Chunkgröße (2 MB) für die zeitkritischen Fälle 12 und 14: viele kleine Chunks
# geben ein verlässliches Zeitfenster für Abbruch bzw. Änderung während des Uploads.
SMALL_CHUNKS=(env OWNCLOUD_CHUNK_SIZE=2000000 OWNCLOUD_MIN_CHUNK_SIZE=2000000 OWNCLOUD_MAX_CHUNK_SIZE=2000000)
SMALL_CHUNK_BYTES=2000000

dav() { python3 -I "$HERE/davtool.py" "$@"; }
sha() { sha256sum "$1" | cut -c1-64; }
rnd() { mkdir -p "$(dirname "$1")" && head -c $((100 + RANDOM % 4000)) /dev/urandom >"$1"; }
rnd_mib() { mkdir -p "$(dirname "$1")" && dd if=/dev/urandom of="$1" bs=1M count="$2" status=none; }

# ---------------------------------------------------------------- Fallrahmen
ROWS=()
ID='' TITLE='' STATUS='' NOTES=() L='' R='' LOG='' RC=0 BG_PID='' STARTED=0 LAST_RUN=

case_begin() {
	ID=$1 TITLE=$2 STATUS=PASS NOTES=() STARTED=$SECONDS
	L=$WORK/$ID R=e2e-$ID
	echo "::group::Fall $ID – $TITLE"
	rm -rf "$L" "$WORK/seed-$ID" && mkdir -p "$L" "$WORK/seed-$ID" || { case_error "lokaler Ordner $L"; return 1; }
	dav delete "$R" && dav mkdirs "$R" || { case_error "Serverordner /$R nicht anlegbar"; return 1; }
}
fail() { [ "$STATUS" = ERROR ] || STATUS=FAIL; NOTES+=("$*"); echo "  FAIL: $*"; }
note() { NOTES+=("$*"); echo "  info: $*"; }
case_error() { STATUS=ERROR; NOTES+=("Suite: $*"); echo "  ERROR: $*"; }
case_end() {
	local text='' n
	for n in "${NOTES[@]}"; do text+="${text:+; }$n"; done
	ROWS+=("$ID"$'\t'"$TITLE"$'\t'"$STATUS"$'\t'"$((SECONDS - STARTED))"$'\t'"${text//$'\n'/ }")
	echo "  => $STATUS"
	echo "::endgroup::"
}
run_case() { # run_case <nr> <titel> <funktion>
	case_begin "$1" "$2" && "$3"
	case_end
}

# ---------------------------------------------------------------- owncloudcmd-Läufe
# ocmd <lauf> [env-präfix …]: ein Sync-Lauf für den aktuellen Fall; setzt RC und LOG
ocmd() {
	local run=$1
	shift
	LOG=$LOGS/$ID-$run.log LAST_RUN=$run
	"$@" timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$L" "$OC_URL" -u "$OC_USER" -p "$OC_PASS" --path "/$R" >"$LOG" 2>&1
	RC=$?
	echo "  $run: exit $RC"
}
# start_bg <lauf> [env-präfix …]: Lauf im Hintergrund; setzt BG_PID und LOG
start_bg() {
	local run=$1
	shift
	LOG=$LOGS/$ID-$run.log LAST_RUN=$run
	"$@" "$OCMD" "$L" "$OC_URL" -u "$OC_USER" -p "$OC_PASS" --path "/$R" >"$LOG" 2>&1 &
	BG_PID=$!
}
alive() { local s; s=$(ps -o stat= -p "$1" 2>/dev/null) && [[ $s != Z* ]]; }
# wait_bg: wartet auf den Hintergrundlauf (höchstens RUN_TIMEOUT); setzt RC
wait_bg() {
	local deadline=$((SECONDS + RUN_TIMEOUT))
	while alive "$BG_PID" && [ $SECONDS -lt $deadline ]; do sleep 1; done
	alive "$BG_PID" && kill -9 "$BG_PID" 2>/dev/null
	wait "$BG_PID"
	RC=$?
	echo "  Hintergrundlauf: exit $RC"
}
# check_run <lauf> [allow-fail]: Absturz, ASSERT, Zeitüberschreitung, Exit-Code
check_run() {
	local run=$1 allow_fail=${2:-} assert err
	assert=$(grep -m1 -aoE '(ASSERT|ENFORCE|ENSURE)(:| failure in) .*' "$LOG" | sed -E 's#in file [^ ]*/#in file #; s#(line [0-9]+).*#\1#' | cut -c1-200)
	err=$(grep -m1 -aoE '(Sync error|Could not reach the server|Failed to fetch capabilities|Usage|A user).*' "$LOG" | cut -c1-200)
	if [ "$RC" -gt 128 ] && [ "$RC" -ne 137 ]; then
		fail "$run: Absturz (Signal $((RC - 128)))${assert:+ – $assert}"
		return
	fi
	[ -n "$assert" ] && fail "$run: $assert"
	case $RC in
	0) ;;
	137) fail "$run: hängt – nach ${RUN_TIMEOUT}s abgebrochen" ;;
	1) [ -n "$allow_fail" ] || fail "$run: Sync meldet Fehler (Exit 1)${err:+: $err}" ;;
	*) fail "$run: Exit $RC${err:+: $err}" ;;
	esac
}

# ---------------------------------------------------------------- Protokollauswertung
reqs() { dav httplog "$1"; }
count_req() { reqs "$1" | grep -cE "^REQ $2 $3"; }        # count_req <log> <methode> <pfad-regex>
count_res() { reqs "$1" | grep -cE "^RES $2 $3 $4"; }     # count_res <log> <methode> <status-regex> <pfad-regex>
# Anfragen, die Daten bewegen oder den Server verändern
transfers() { reqs "$1" | grep -E '^REQ (GET|PUT|POST|MOVE|COPY|DELETE|MKCOL) /([^ ]*/)?remote\.php/(dav|webdav)/'; }
summarize() { awk '{print $2}' | sort | uniq -c | awk '{printf "%s%s×%s", (NR>1?", ":""), $1, $2} END{if(NR==0)printf "keine"}'; }
upload_paths() {
	echo "Bulk-POST $(count_req "$1" POST '/remote\.php/dav/bulk$'), PUT $(count_req "$1" PUT '/remote\.php/dav/files/'), Chunk-PUT $(count_req "$1" PUT '/remote\.php/dav/uploads/')"
}
chunks_done() { count_res "$1" PUT '20[14]' '/remote\.php/dav/uploads/'; }
# wait_chunks <anzahl> <sekunden>: wartet, bis so viele Chunks bestätigt sind, solange der Lauf lebt
wait_chunks() {
	local deadline=$((SECONDS + $2))
	while [ $SECONDS -lt $deadline ]; do
		alive "$BG_PID" || return 1
		[ "$(chunks_done "$LOG")" -ge "$1" ] && return 0
		sleep 0.2
	done
	return 1
}

# ---------------------------------------------------------------- Prüfungen
# compare <lokal> <serverpfad> [--skip-conflicts]: Namen, Ordner und SHA-256 beider Seiten gleich
compare() {
	local lm rm d n
	lm=$(dav localmanifest "$1" ${3:-}) || { fail "lokales Manifest nicht lesbar"; return 1; }
	rm=$(dav manifest "$2" ${3:-}) || { fail "Server-Manifest nicht lesbar"; return 1; }
	[ "$lm" = "$rm" ] && return 0
	d=$(diff <(printf '%s\n' "$lm") <(printf '%s\n' "$rm") | grep -E '^[<>]')
	n=$(grep -c . <<<"$d")
	fail "nach $LAST_RUN: lokal ≠ Server ($n Abweichungen; <lokal >Server: $(head -4 <<<"$d" | cut -f1 | tr '\n' ' '))"
	return 1
}
# seed <rel> …: kleine Zufallsdateien direkt auf dem Server anlegen (am Client vorbei)
seed() {
	local f tmp=$WORK/seed-$ID/x
	for f in "$@"; do
		rnd "$tmp" && dav put "$tmp" "$R/$f" || return 1
	done
}
server_files() { dav manifest "$1" | grep -c $'\t[0-9a-f]\\{64\\}$'; }

# ---------------------------------------------------------------- Fälle
t01() {
	rnd "$L/klein.txt"
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R"
	note "Weg: $(upload_paths "$LOG")"
}

t02() {
	local i t
	for i in $(seq -w 1 300); do rnd "$L/viele/f$i.bin"; done
	ocmd lauf1
	check_run lauf1
	[ "$(count_req "$LOG" POST '/remote\.php/dav/bulk$')" -ge 1 ] || fail "kein POST /dav/bulk – Bulk-Pfad nicht benutzt"
	note "lauf1: $(upload_paths "$LOG"); auf dem Server $(server_files "$R/viele")/300"
	compare "$L" "$R" || return
	# Bulk muss das Journal schreiben, sonst lädt der nächste Lauf alles erneut hoch
	ocmd lauf2
	check_run lauf2
	t=$(transfers "$LOG")
	[ -z "$t" ] || fail "lauf2 überträgt ohne Änderung erneut: $(summarize <<<"$t")"
}

t03() {
	local chunks moves
	rnd_mib "$L/gross-250MiB.bin" 250
	ocmd lauf1
	check_run lauf1
	chunks=$(chunks_done "$LOG")
	moves=$(count_req "$LOG" MOVE '/remote\.php/dav/uploads/.*/\.file$')
	[ "$chunks" -ge 2 ] || fail "nicht in mehreren Chunks hochgeladen ($(upload_paths "$LOG"))"
	[ "$moves" -ge 1 ] || fail "kein abschließendes MOVE …/.file"
	note "Chunks $chunks, MOVE $moves"
	compare "$L" "$R"
}

t04() {
	local seed=$WORK/seed-$ID
	rnd "$seed/klein.bin" && rnd_mib "$seed/gross-120MiB.bin" 120 &&
		dav put "$seed/klein.bin" "$R/klein.bin" && dav put "$seed/gross-120MiB.bin" "$R/gross-120MiB.bin" ||
		{ case_error "Vorlage nicht hochladbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R"
	note "GET $(count_req "$LOG" GET '/remote\.php/dav/files/')"
}

t05() {
	local id_a id_b id_d
	seed a.txt b.txt ordner/c.txt ordner/d.txt && dav mkdirs "$R/ziel" || { case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	id_a=$(dav stat "$R/a.txt" fileid) id_b=$(dav stat "$R/b.txt" fileid) id_d=$(dav stat "$R/ordner" fileid)
	mv "$L/a.txt" "$L/a-umbenannt.txt"
	mv "$L/b.txt" "$L/ziel/b.txt"
	mv "$L/ordner" "$L/ordner-umbenannt"
	ocmd lauf2
	check_run lauf2
	compare "$L" "$R"
	# Gleiche Datei-ID = echtes MOVE; Löschen + Neu-Upload verlöre Freigaben, Versionen, Kommentare
	[ -n "$id_a" ] && [ "$(dav stat "$R/a-umbenannt.txt" fileid)" = "$id_a" ] || fail "Umbenennen: Datei-ID nicht erhalten (Neu-Upload statt MOVE)"
	[ -n "$id_b" ] && [ "$(dav stat "$R/ziel/b.txt" fileid)" = "$id_b" ] || fail "Verschieben: Datei-ID nicht erhalten (Neu-Upload statt MOVE)"
	[ -n "$id_d" ] && [ "$(dav stat "$R/ordner-umbenannt" fileid)" = "$id_d" ] || fail "Ordner umbenennen: ID nicht erhalten"
	note "lauf2: $(transfers "$LOG" | summarize)"
}

t06() {
	local ino
	seed s1.txt sordner/s2.txt && rnd_mib "$WORK/seed-$ID/s3.bin" 2 && dav put "$WORK/seed-$ID/s3.bin" "$R/sordner/s3.bin" ||
		{ case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	ino=$(stat -c %i "$L/sordner/s3.bin")
	dav move "$R/s1.txt" "$R/s1-umbenannt.txt" && dav move "$R/sordner" "$R/sordner-umbenannt" ||
		{ case_error "Umbenennen auf dem Server gescheitert"; return; }
	ocmd lauf2
	check_run lauf2
	compare "$L" "$R"
	if [ -f "$L/sordner-umbenannt/s3.bin" ] && [ "$(stat -c %i "$L/sordner-umbenannt/s3.bin")" = "$ino" ]; then
		note "lokal umbenannt (kein Neu-Download)"
	else
		note "neu heruntergeladen statt umbenannt: $(transfers "$LOG" | summarize)"
	fi
}

t07() {
	seed del-lokal.txt del-server.txt bleibt.txt dordner/x.txt dordner/y.txt || { case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	rm "$L/del-lokal.txt"
	rm -r "$L/dordner"
	dav delete "$R/del-server.txt" || { case_error "Löschen auf dem Server gescheitert"; return; }
	ocmd lauf2
	check_run lauf2
	[ "$(dav stat "$R/del-lokal.txt" exists)" = false ] || fail "lokal gelöschte Datei noch auf dem Server"
	[ "$(dav stat "$R/dordner" exists)" = false ] || fail "lokal gelöschter Ordner noch auf dem Server"
	[ ! -e "$L/del-server.txt" ] || fail "auf dem Server gelöschte Datei lokal noch da"
	[ -f "$L/bleibt.txt" ] && [ "$(dav stat "$R/bleibt.txt" exists)" = true ] || fail "unbeteiligte Datei verloren"
	compare "$L" "$R"
}

t08() {
	local seed=$WORK/seed-$ID lsha ssha cf
	printf 'Version 1\n' >"$seed/v1" && dav put "$seed/v1" "$R/konflikt.txt" || { case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	sleep 1
	printf 'LOKAL geändert, länger als vorher %s\n' "$RANDOM" >"$L/konflikt.txt"
	lsha=$(sha "$L/konflikt.txt")
	printf 'SERVER geändert %s\n' "$RANDOM$RANDOM" >"$seed/v2"
	ssha=$(sha "$seed/v2")
	dav put "$seed/v2" "$R/konflikt.txt" || { case_error "Serveränderung gescheitert"; return; }
	ocmd lauf2
	check_run lauf2
	[ "$(dav sha "$R/konflikt.txt")" = "$ssha" ] || fail "Serverfassung überschrieben oder verloren"
	[ "$(sha "$L/konflikt.txt")" = "$ssha" ] || fail "lokal liegt nicht die Serverfassung"
	cf=$(find "$L" -maxdepth 1 -type f \( -name '*_conflict-*' -o -name '*(conflicted copy*' \) | head -1)
	if [ -z "$cf" ]; then
		fail "keine Konfliktkopie – lokale Änderung verloren?"
	elif [ "$(sha "$cf")" != "$lsha" ]; then
		fail "Konfliktkopie enthält nicht die lokale Fassung"
	else
		note "Konfliktkopie: ${cf##*/}"
	fi
	dav manifest "$R" | grep -qE '_conflict-|\(conflicted copy' && note "Konfliktkopie liegt auch auf dem Server"
	compare "$L" "$R" --skip-conflicts
}

# Fälle 9 und 10 prüfen zuerst die Wege ohne Bulk (Download, Ordner, NG-Chunking) und
# erst danach kleine neue Dateien (Bulk), damit ein Bulk-Fehler den Rest nicht verdeckt.
t09() {
	local n t
	seed 'Server Ä Ö Ü ß.txt' 'Server 🎉 Emoji.txt' 'Server Ordner ä/Inhalt ö #1.txt' 'Server Sonder #%&+;=@[]{}!$(),~.txt' ||
		{ case_error "Vorlage nicht anlegbar"; return; }
	rnd_mib "$L/Groß 🎉 Chunk #%.bin" 2 # > 1 MiB: NG-Chunking, Sonderzeichen im MOVE-Ziel
	mkdir -p "$L/Leerer Ordner äöü 🌍 #%"
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	for n in 'Größe äöü ÄÖÜ ß.txt' 'mit  zwei Leerzeichen.txt' 'Emoji 🚀😀.txt' 'Sonder #%&+;=@[]{}!$(),~.txt' 'Ordner äöü 🌍/Datei ß €.txt'; do
		rnd "$L/$n"
	done
	ocmd lauf2-bulk
	check_run lauf2-bulk
	compare "$L" "$R" || return
	# Namen im Journal und auf dem Server müssen übereinstimmen, sonst überträgt der nächste Lauf erneut
	ocmd lauf3
	check_run lauf3
	t=$(transfers "$LOG")
	[ -z "$t" ] || fail "lauf3 überträgt ohne Änderung: $(summarize <<<"$t")"
}

t10() {
	local d=$L i
	seed "$(printf 's%d/' $(seq 1 12))server-tief.txt" || { case_error "Vorlage nicht anlegbar"; return; }
	for i in $(seq 1 15); do d=$d/ebene$i; done
	mkdir -p "$d/leer"
	rnd_mib "$d/tief-chunk.bin" 2
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	d=$L
	for i in $(seq 1 15); do
		d=$d/ebene$i
		[ $((i % 5)) -eq 0 ] && rnd "$d/datei-ebene$i.txt"
	done
	rnd "$d/tief.txt"
	ocmd lauf2-bulk
	check_run lauf2-bulk
	compare "$L" "$R"
}

t11() {
	local t
	seed s1.txt s2.txt sub/s3.txt || { case_error "Vorlage nicht anlegbar"; return; }
	rnd_mib "$L/chunk-2MiB.bin" 2
	: >"$L/leer.txt"
	mkdir -p "$L/lokaler-ordner"
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	ocmd lauf2
	check_run lauf2
	t=$(transfers "$LOG")
	[ -z "$t" ] || fail "lauf2 überträgt ohne Änderung: $(summarize <<<"$t")"
}

t12() {
	local f=$L/abbruch-200MiB.bin before total done1 again
	rnd_mib "$f" 200
	before=$(sha "$f")
	total=$(((200 * 1048576 + SMALL_CHUNK_BYTES - 1) / SMALL_CHUNK_BYTES))
	start_bg lauf1 "${SMALL_CHUNKS[@]}"
	if ! wait_chunks 5 180; then
		alive "$BG_PID" && kill -9 "$BG_PID"
		wait "$BG_PID"
		case_error "kein Abbruchfenster: Lauf endete vor 5 bestätigten Chunks ($(chunks_done "$LOG") Chunks)"
		return
	fi
	kill -9 "$BG_PID"
	wait "$BG_PID"
	RC=$?
	[ "$RC" -eq 137 ] || { case_error "lauf1 endete vor dem Abbruch (exit $RC)"; return; }
	done1=$(chunks_done "$LOG")
	note "lauf1 nach $done1/$total Chunks hart beendet (kill -9)"
	RC=0
	check_run lauf1 # nur ASSERTs vor dem Abbruch
	[ "$(sha "$f")" = "$before" ] || fail "lokale Datei durch den Abbruch verändert"
	[ "$(dav stat "$R/abbruch-200MiB.bin" exists)" = false ] || fail "unvollständige Datei nach dem Abbruch auf dem Server sichtbar"
	ocmd lauf2 "${SMALL_CHUNKS[@]}"
	check_run lauf2
	again=$(chunks_done "$LOG")
	[ "$(dav sha "$R/abbruch-200MiB.bin")" = "$before" ] || fail "nach erneutem Lauf falscher oder fehlender Serverinhalt"
	if [ "$again" -lt "$total" ]; then
		note "fortgesetzt: lauf2 lud $again/$total Chunks"
	else
		fail "nicht fortgesetzt: lauf2 lud alle $again/$total Chunks neu"
	fi
	compare "$L" "$R"
}

t13() {
	local seed=$WORK/seed-$ID
	: >"$L/leer-lokal.txt"
	: >"$seed/leer" && dav put "$seed/leer" "$R/leer-server.txt" || { case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	[ "$(dav stat "$R/leer-lokal.txt" size)" = 0 ] || fail "leere Datei fehlt auf dem Server"
	[ -f "$L/leer-server.txt" ] && [ ! -s "$L/leer-server.txt" ] || fail "leere Serverdatei fehlt lokal oder ist nicht leer"
	compare "$L" "$R"
}

t14() {
	local f=$L/aenderung.bin v1 v2 s k runs=0
	rnd_mib "$f" 150
	v1=$(sha "$f")
	start_bg lauf1 "${SMALL_CHUNKS[@]}"
	if ! wait_chunks 3 180; then
		alive "$BG_PID" && kill -9 "$BG_PID"
		wait "$BG_PID"
		case_error "kein Änderungsfenster: Lauf endete vor 3 bestätigten Chunks"
		return
	fi
	# Während des Uploads an Ort und Stelle neu schreiben: anderer Inhalt, andere Größe
	dd if=/dev/urandom of="$f" bs=1M count=100 status=none
	v2=$(sha "$f")
	wait_bg
	check_run lauf1 allow-fail
	s=$(dav sha "$R/aenderung.bin")
	case $s in
	MISSING) note "lauf1: Änderung erkannt, nichts übernommen" ;;
	"$v2") note "lauf1: neue Fassung übernommen" ;;
	"$v1") fail "lauf1: alte Fassung trotz Änderung als fertig übernommen" ;;
	*) fail "lauf1: Mischinhalt auf dem Server (weder alte noch neue Fassung)" ;;
	esac
	for k in 2 3 4; do
		[ "$(dav sha "$R/aenderung.bin")" = "$v2" ] && break
		[ "$k" -gt 2 ] && sleep 30 # eine eventuelle Sperrfrist des Clients (mind. 25 s) abwarten
		ocmd "lauf$k" "${SMALL_CHUNKS[@]}"
		check_run "lauf$k"
		runs=$((runs + 1))
	done
	[ "$(sha "$f")" = "$v2" ] || fail "lokale Datei verändert"
	compare "$L" "$R" && note "neue Fassung nach $runs weiteren Läufen auf dem Server"
}

# ---------------------------------------------------------------- Ablauf
CASES=(
	"01|Upload kleine Datei|t01"
	"02|Upload 300 kleine Dateien (Bulk)|t02"
	"03|Upload 250 MiB (NG-Chunking)|t03"
	"04|Download klein + groß|t04"
	"05|Umbenennen/Verschieben lokal → Server|t05"
	"06|Umbenennen auf dem Server → lokal|t06"
	"07|Löschen lokal → Server und Server → lokal|t07"
	"08|Konflikt: beidseitig geändert|t08"
	"09|Umlaute, Leerzeichen, Sonderzeichen, Emoji|t09"
	"10|Tief verschachtelte Ordner|t10"
	"11|Zweiter Lauf ohne Änderung überträgt nichts|t11"
	"12|Abbruch mitten im Upload, erneuter Lauf|t12"
	"13|Leere Datei|t13"
	"14|Datei während des Syncs geändert|t14"
)

rm -rf "$WORK" "$LOGS"
mkdir -p "$WORK" "$LOGS"
[ -x "$OCMD" ] || { echo "owncloudcmd nicht ausführbar: $OCMD" >&2; exit 2; }
[ "$(dav stat '' exists)" = true ] || { echo "Server $OC_URL nicht erreichbar oder Anmeldung abgelehnt" >&2; exit 2; }

for entry in "${CASES[@]}"; do
	IFS='|' read -r nr title fn <<<"$entry"
	if [ $# -gt 0 ] && [[ " $* " != *" $nr "* ]]; then continue; fi
	run_case "$nr" "$title" "$fn"
done

pass=0 failed=0 errors=0
{
	echo "| Nr | Fall | Ergebnis | Dauer | Befund |"
	echo "|---|---|---|---|---|"
	for row in "${ROWS[@]}"; do
		IFS=$'\t' read -r nr title st secs text <<<"$row"
		case $st in PASS) pass=$((pass + 1)) ;; FAIL) failed=$((failed + 1)) ;; *) errors=$((errors + 1)) ;; esac
		echo "| $nr | $title | **$st** | ${secs}s | ${text//|/\\|} |"
	done
	echo
	echo "PASS $pass, FAIL $failed, ERROR $errors"
} >"$OUT/results.md"
printf '%s\n' "${ROWS[@]}" >"$OUT/results.tsv"
cat "$OUT/results.md"

[ "$errors" -eq 0 ] || exit 2
[ "$failed" -eq 0 ] || exit 1
exit 0

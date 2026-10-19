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
# OAuth2 (E2E_AUTH=oauth): Fälle A1–A9 statt 01–14. A1 meldet sich wie der Desktop-Client
# im Browser an (test/e2e/oauth/login.py) und legt die Token-Datei an; damit laufen die
# Sync-Fälle 01–05, 07 und 08 über owncloudcmd --oauth-token-file, danach Erneuern und
# Widerruf. OC_USER/OC_PASS dienen dann nur noch der Browser-Anmeldung und davtool.
# A8 (Token läuft mitten im Sync ab) und A9 (doppelter Refresh) stellen den Testserver
# per occ um (OC_ROOT, nur im Runner) und brauchen oauth2 ab 0.10.0, sonst SKIP.
#   E2E_OAUTH2_VERSION   Version der oauth2-App, wenn bekannt (Erwartung für 127.0.0.1)
#   E2E_OAUTH_REVOKE=1   A6: Freigabe in den persönlichen Einstellungen widerrufen
#                        (trifft alle Desktop-Anmeldungen des Kontos – nur auf Testservern)
#   E2E_MASK_ACCOUNT=1   Kennung des Kontos im Protokoll maskieren (fremde Server)
# E2E_REMOTE_BASE: alle Serverordner unter diesem Ordner (für fremde Server; aufräumen
# muss der Aufrufer). E2E_SECRETS: Datei, in der die Suite alle gesehenen Tokens sammelt,
# damit der Aufrufer sie aus den Protokollen schwärzen kann.
#
# Ergebnis: $E2E_OUT/results.md (Tabelle), $E2E_OUT/results.tsv,
#           $E2E_OUT/logs/<nr>-<lauf>.log (owncloudcmd-Protokolle mit HTTP-Logger).
# Exit: 0 alle PASS, 1 mindestens ein FAIL, 2 Suite-/Umgebungsfehler (ERROR).
# SKIP: der getestete owncloudcmd kennt die Option des Falls noch nicht (Stände vor 11.0.0-beta3);
# zählt weder als Fehler noch als Erfolg.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
: "${OCMD:?OCMD (Pfad zu owncloudcmd) fehlt}" "${OC_URL:?}" "${OC_USER:?}" "${OC_PASS:?}"
export OC_URL OC_USER OC_PASS
OUT=${E2E_OUT:-$PWD/e2e-out}
WORK=$OUT/work
LOGS=$OUT/logs
RUN_TIMEOUT=${E2E_RUN_TIMEOUT:-600}
AUTH=${E2E_AUTH:-basic}
case $AUTH in basic | oauth) ;; *) echo "E2E_AUTH muss basic oder oauth sein" >&2 && exit 2 ;; esac
BASE=${E2E_REMOTE_BASE:+${E2E_REMOTE_BASE%/}/}
SECRETS=${E2E_SECRETS:-$WORK/secrets.lst}
# Token-Datei der laufenden OAuth2-Fälle (A1 legt sie an, A3/A4 erneuern sie)
TOKEN_FILE=
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
# OCMD_ARGS: weitere owncloudcmd-Optionen des laufenden Falls; OCMD_STDIN: Datei als Standardeingabe
OCMD_ARGS=() OCMD_STDIN=

case_begin() {
	ID=$1 TITLE=$2 STATUS=PASS NOTES=() STARTED=$SECONDS OCMD_ARGS=() OCMD_STDIN=
	L=$WORK/$ID R=${BASE}e2e-$ID
	echo "::group::Fall $ID – $TITLE"
	rm -rf "$L" "$WORK/seed-$ID" && mkdir -p "$L" "$WORK/seed-$ID" || { case_error "lokaler Ordner $L"; return 1; }
	dav delete "$R" && dav mkdirs "$R" || { case_error "Serverordner /$R nicht anlegbar"; return 1; }
}
fail() { [ "$STATUS" = ERROR ] || STATUS=FAIL; NOTES+=("$*"); echo "  FAIL: $*"; }
note() { NOTES+=("$*"); echo "  info: $*"; }
case_error() { STATUS=ERROR; NOTES+=("Suite: $*"); echo "  ERROR: $*"; }
# has_option <option>: kennt der getestete owncloudcmd die Option? (Optionen aus 6.0.3 erst ab 11.0.0-beta3)
has_option() { "$OCMD" --help 2>/dev/null | grep -q -- "$1"; }
skip_unsupported() { STATUS=SKIP; NOTES+=("owncloudcmd kennt $1 nicht (älterer Stand)"); echo "  SKIP: $1"; }
# annotate <titel> <text>: Fehlerhinweis an der Lauf-Übersicht (nur unter GitHub Actions);
# Werte nach den Regeln der Workflow-Befehle kodiert, damit % : , den Befehl nicht zerlegen
annotate() {
	[ -n "${GITHUB_ACTIONS:-}" ] || return 0
	local t=${1//'%'/%25} m=${2//'%'/%25}
	t=${t//:/%3A} t=${t//,/%2C}
	echo "::error title=$t::$m"
}
case_end() {
	local text='' n
	for n in "${NOTES[@]}"; do text+="${text:+; }$n"; done
	text=${text//$'\n'/ }
	ROWS+=("$ID"$'\t'"$TITLE"$'\t'"$STATUS"$'\t'"$((SECONDS - STARTED))"$'\t'"$text")
	case $STATUS in
	FAIL) annotate "Fall $ID – $TITLE" "$text" ;;
	ERROR) annotate "Fall $ID – $TITLE (nicht bewertbar)" "$text" ;;
	esac
	echo "  => $STATUS"
	echo "::endgroup::"
}
run_case() { # run_case <nr> <titel> <funktion>
	case_begin "$1" "$2" && "$3"
	case_end
}

# ---------------------------------------------------------------- owncloudcmd-Läufe
# Anmeldung der Läufe: Benutzer und Kennwort oder die aktuelle Token-Datei
auth_args() {
	if [ "$AUTH" = oauth ]; then
		AUTH_ARGS=(--oauth-token-file "$TOKEN_FILE")
	else
		AUTH_ARGS=(-u "$OC_USER" -p "$OC_PASS")
	fi
}
# ocmd <lauf> [env-präfix …]: ein Sync-Lauf für den aktuellen Fall; setzt RC und LOG.
# Dazu OCMD_ARGS und als Standardeingabe OCMD_STDIN (sonst /dev/null: eine Rückfrage endet sofort).
ocmd() {
	local run=$1
	shift
	LOG=$LOGS/$ID-$run.log LAST_RUN=$run
	auth_args
	"$@" timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$L" "$OC_URL" "${AUTH_ARGS[@]}" --path "/$R" "${OCMD_ARGS[@]}" <"${OCMD_STDIN:-/dev/null}" >"$LOG" 2>&1
	RC=$?
	[ "$AUTH" != oauth ] || remember_tokens "$TOKEN_FILE"
	echo "  $run: exit $RC"
}
# start_bg <lauf> [env-präfix …]: Lauf im Hintergrund; setzt BG_PID und LOG
start_bg() {
	local run=$1
	shift
	LOG=$LOGS/$ID-$run.log LAST_RUN=$run
	auth_args
	"$@" "$OCMD" "$L" "$OC_URL" "${AUTH_ARGS[@]}" --path "/$R" >"$LOG" 2>&1 &
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
	[ "$AUTH" != oauth ] || remember_tokens "$TOKEN_FILE"
	echo "  Hintergrundlauf: exit $RC"
}
# check_run <lauf> [allow-fail]: Absturz, ASSERT, Zeitüberschreitung, Exit-Code
check_run() {
	local run=$1 allow_fail=${2:-} assert err
	assert=$(grep -m1 -aoE '(ASSERT|ENFORCE|ENSURE)(:| failure in) .*' "$LOG" | sed -E 's#in file [^ ]*/#in file #; s#(line [0-9]+).*#\1#' | cut -c1-200)
	err=$(grep -m1 -aoE '(Sync error|Could not reach the server|Failed to fetch capabilities|Authentication failed|Cannot use the OAuth2|The OAuth2 token file|Usage|A user).*' "$LOG" | cut -c1-200)
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
	note "lauf1 nach $done1/$total bestätigten Chunks hart beendet (kill -9)"
	RC=0
	check_run lauf1 # nur ASSERTs vor dem Abbruch
	[ "$(sha "$f")" = "$before" ] || fail "lokale Datei durch den Abbruch verändert"
	[ "$(dav stat "$R/abbruch-200MiB.bin" exists)" = false ] || fail "unvollständige Datei nach dem Abbruch auf dem Server sichtbar"
	ocmd lauf2 "${SMALL_CHUNKS[@]}"
	check_run lauf2
	again=$(chunks_done "$LOG")
	[ "$(dav sha "$R/abbruch-200MiB.bin")" = "$before" ] || fail "nach erneutem Lauf falscher oder fehlender Serverinhalt"
	if [ "$again" -lt "$total" ]; then
		# Parallele Chunks können noch angekommen sein, ohne dass lauf1 die Antwort protokolliert hat
		note "fortgesetzt: lauf2 lud $again/$total Chunks, $((total - again)) vom Server übernommen"
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
	local f=$L/aenderung.bin v1 v2 s k runs=0 total late=''
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
	# Stand nach der Änderung: Waren schon alle Chunks bestätigt, lag die Änderung nicht mehr
	# im Upload – dann ist eine übernommene alte Fassung kein Client-Fehler.
	total=$(((150 * 1048576 + SMALL_CHUNK_BYTES - 1) / SMALL_CHUNK_BYTES))
	alive "$BG_PID" && [ "$(chunks_done "$LOG")" -lt "$total" ] || late=1
	v2=$(sha "$f")
	wait_bg
	check_run lauf1 allow-fail
	s=$(dav sha "$R/aenderung.bin")
	case $s in
	MISSING) note "lauf1: Änderung erkannt, nichts übernommen" ;;
	"$v2") note "lauf1: neue Fassung übernommen" ;;
	"$v1")
		if [ -n "$late" ]; then
			# Fenster verpasst: nicht bewertbar; ein schon gemeldeter Client-Fehler bleibt FAIL
			if [ "$STATUS" = FAIL ]; then
				note "Änderung erst nach dem letzten Chunk fertig – Änderungsfall nicht bewertbar"
			else
				case_error "Änderung erst nach dem letzten Chunk fertig – kein Änderungsfenster"
			fi
			return
		fi
		fail "lauf1: alte Fassung trotz Änderung als fertig übernommen"
		;;
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

# ---------------------------------------------------------------- OAuth2-Fälle
TOKEN_PATH_RE='/([^ ]*/)?index\.php/apps/oauth2/api/v1/token$'
LOGIN=(python3 -I "$HERE/../oauth/login.py" --server "$OC_URL")

# remember_tokens <token-datei>: Tokens maskieren (GitHub Actions) und für das Schwärzen merken;
# kurze Platzhalter der Fälle ("abgelaufen", "unbekannt") sind keine Tokens
remember_tokens() {
	local value
	[ -f "${1:-}" ] || return 0
	while IFS= read -r value; do
		[ ${#value} -ge 20 ] || continue
		[ -z "${GITHUB_ACTIONS:-}" ] || echo "::add-mask::$value"
		printf '%s\n' "$value" >>"$SECRETS"
	done < <(jq -r '.access_token // empty, .refresh_token // empty' "$1" 2>/dev/null)
}
# token_edit <datei> <jq-filter>: Token-Datei verändern; die Rechte (0600) bleiben
token_edit() {
	local content
	content=$(jq "$2" "$1") && printf '%s\n' "$content" >"$1"
}
# token_copy <quelle> <ziel>: Kopie nur für den Eigentümer lesbar
token_copy() { cp "$1" "$2" && chmod 600 "$2"; }
token_posts() { count_req "$LOG" POST "$TOKEN_PATH_RE"; }
unauthorized() { count_res "$LOG" '[A-Z]+' 401 '.*'; }
version_lt() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]; }

# oauth_login <redirect-host> <token-datei>: Browser-Anmeldung wie der Desktop-Client; setzt RC und LOG
oauth_login() {
	LOG=$LOGS/$ID-anmeldung.log LAST_RUN=anmeldung
	"${LOGIN[@]}" --redirect-host "$1" --out "$2" >"$LOG" 2>&1
	RC=$?
	# Kennung eines fremden Kontos nicht offen in das Protokoll eines öffentlichen Laufs
	if [ "${E2E_MASK_ACCOUNT:-}" = 1 ] && [ -n "${GITHUB_ACTIONS:-}" ] && [ -f "$2" ]; then
		local uid
		uid=$(jq -r '.user_id // empty' "$2" 2>/dev/null)
		[ -z "$uid" ] || echo "::add-mask::$uid"
	fi
	sed 's/^/    /' "$LOG"
	echo "  anmeldung: exit $RC"
}

# login_error: Fehlerzeile von login.py (Fehler, Ablehnung oder fehlende oauth2-App)
login_error() { grep -m1 -E '^(Fehler|abgelehnt|ohne oauth2):' "$LOG" | cut -c1-220; }

# expect_signin_failure <lauf> <token-datei> <sha vorher>: sauberer Abbruch mit Exit 2
expect_signin_failure() {
	if [ "$RC" -gt 128 ]; then
		fail "$1: Absturz (Signal $((RC - 128)))"
		return
	fi
	[ "$RC" -eq 2 ] || fail "$1: Exit $RC statt 2 (neu anmelden)"
	grep -q 'Authentication failed:.*sign in again' "$LOG" || fail "$1: keine klare Meldung mit „sign in again“"
	[ "$(sha "$2")" = "$3" ] || fail "$1: Token-Datei verändert"
	[ -f "$L/bleibt.txt" ] || fail "$1: lokale Datei gelöscht"
	[ "$(dav stat "$R/bleibt.txt" exists)" = false ] || fail "$1: trotz abgelehnter Anmeldung hochgeladen"
	note "$1: $(grep -m1 -ao 'Authentication failed:.*' "$LOG" | cut -c1-170)"
}

a1() {
	local tf=$WORK/oauth/token.json
	mkdir -p "$WORK/oauth"
	oauth_login localhost "$tf"
	case $RC in
	0) ;;
	5)
		# keine Schwäche des Clients: auch der Desktop-Client kann sich auf diesem Server nicht anmelden
		case_error "Server ohne oauth2-App – Anmeldung des Desktop-Clients dort unmöglich: $(login_error)"
		return
		;;
	*)
		fail "Anmeldung über http://localhost:<port> gescheitert (Exit $RC): $(login_error)"
		return
		;;
	esac
	remember_tokens "$tf"
	jq -e '.server_url and .access_token and .refresh_token and .expires_at' "$tf" >/dev/null || fail "Token-Datei unvollständig"
	[ "$(stat -c %a "$tf")" = 600 ] || fail "Token-Datei nicht nur für den Eigentümer lesbar"
	TOKEN_FILE=$tf
	# WebDAV-Pfade von davtool über die Kennung, falls der Anmeldename abweicht (E-Mail-Login)
	OC_DAV_USER=$(jq -r '.user_id // empty' "$tf")
	export OC_DAV_USER
	note "$(grep -m1 '^Token:' "$LOG" | sed 's/, Benutzer .*//')"
}

a2() {
	local tf=$WORK/oauth/token-127.json v=${E2E_OAUTH2_VERSION:-}
	mkdir -p "$WORK/oauth"
	oauth_login 127.0.0.1 "$tf"
	case $RC in
	0)
		remember_tokens "$tf"
		note "angenommen: redirect_uri http://127.0.0.1:<port> (Loopback nach RFC 8252)"
		if [ -n "$v" ] && version_lt "$v" 0.9.0; then note "unerwartet: oauth2 $v kannte bisher nur http://localhost:*"; fi
		;;
	4)
		if [ -z "$v" ]; then
			note "Server lehnt 127.0.0.1 ab (oauth2 vor 0.9.0?); der Client meldet sich über localhost an"
		elif version_lt "$v" 0.9.0; then
			note "abgelehnt wie erwartet: oauth2 $v kennt nur http://localhost:*"
		else
			fail "oauth2 $v lehnt redirect_uri http://127.0.0.1:<port> ab – ab 0.9.0 erwartet angenommen (RFC 8252)"
		fi
		;;
	5) case_error "Server ohne oauth2-App: $(login_error)" ;;
	*) fail "Anmeldung über 127.0.0.1: Exit $RC – $(login_error)" ;;
	esac
}

a3() {
	local tf=$WORK/oauth/token-a3.json old posts unauth
	# A5: dieses Refresh-Token verbraucht A3 beim Erneuern
	token_copy "$TOKEN_FILE" "$WORK/oauth/token-vor-a3.json" && token_copy "$TOKEN_FILE" "$tf" || {
		case_error "Token-Datei nicht kopierbar"
		return
	}
	token_edit "$tf" '.access_token = "abgelaufen" | .expires_at = (now | floor) - 60'
	old=$(jq -r .refresh_token "$tf")
	rnd "$L/nach-ablauf.txt"
	TOKEN_FILE=$tf
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R"
	posts=$(token_posts) unauth=$(unauthorized)
	[ "$posts" -eq 1 ] || fail "$posts Anfragen an den Token-Endpunkt statt einer"
	[ "$unauth" -eq 0 ] || fail "$unauth Antworten 401 – das abgelaufene Token hätte vor dem Lauf erneuert werden müssen"
	[ "$(jq -r .refresh_token "$tf")" != "$old" ] || fail "neue Tokens nicht in die Token-Datei geschrieben"
	[ "$(jq -r '.expires_at // 0' "$tf")" -gt "$(date +%s)" ] || fail "expires_at nicht erneuert"
	jq -e '.server_url' "$tf" >/dev/null || fail "server_url beim Zurückschreiben verloren"
	[ "$(stat -c %a "$tf")" = 600 ] || fail "Token-Datei nach dem Zurückschreiben nicht mehr 0600"
	note "Token-Endpunkt $posts×, 401: $unauth"
}

a4() {
	local tf=$WORK/oauth/token-a4.json old posts unauth
	token_copy "$TOKEN_FILE" "$tf" || { case_error "Token-Datei nicht kopierbar"; return; }
	# laut Datei gültig, dem Server unbekannt – wie ein Token, das er vorzeitig verworfen hat
	token_edit "$tf" '.access_token = "unbekannt" | .expires_at = (now | floor) + 3600'
	old=$(jq -r .refresh_token "$tf")
	rnd "$L/nach-401.txt"
	TOKEN_FILE=$tf
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R"
	posts=$(token_posts) unauth=$(unauthorized)
	[ "$unauth" -ge 1 ] || fail "keine Antwort 401 – der Server nahm das unbekannte Token an"
	[ "$posts" -eq 1 ] || fail "$posts Anfragen an den Token-Endpunkt statt einer"
	[ "$(jq -r .refresh_token "$tf")" != "$old" ] || fail "neue Tokens nicht in die Token-Datei geschrieben"
	note "401: $unauth, Token-Endpunkt $posts×, danach wiederholt"
}

a5() {
	local tf=$WORK/oauth/token-a5.json keep=$TOKEN_FILE before
	token_copy "$WORK/oauth/token-vor-a3.json" "$tf" || { case_error "Token-Datei von vor A3 fehlt"; return; }
	token_edit "$tf" '.access_token = "unbekannt" | .expires_at = (now | floor) + 3600'
	before=$(sha "$tf")
	rnd "$L/bleibt.txt"
	TOKEN_FILE=$tf
	ocmd lauf1
	TOKEN_FILE=$keep
	expect_signin_failure lauf1 "$tf" "$before"
}

a6() {
	local tf=$WORK/oauth/token-a6.json before
	token_copy "$TOKEN_FILE" "$tf" || { case_error "Token-Datei nicht kopierbar"; return; }
	LOG=$LOGS/$ID-widerruf.log
	if ! "${LOGIN[@]}" --revoke >"$LOG" 2>&1; then
		case_error "Widerruf in den persönlichen Einstellungen gescheitert: $(login_error)"
		return
	fi
	sed 's/^/    /' "$LOG"
	before=$(sha "$tf")
	rnd "$L/bleibt.txt"
	TOKEN_FILE=$tf
	ocmd lauf1
	expect_signin_failure lauf1 "$tf" "$before"
}

a7() {
	local hits
	[ -s "$SECRETS" ] || { case_error "keine Tokens gesammelt"; return; }
	hits=$(grep -rlF -f "$SECRETS" "$OUT" --exclude-dir=work 2>/dev/null)
	[ -z "$hits" ] || fail "Tokens im Klartext in: $(xargs -n1 basename <<<"$hits" | head -4 | tr '\n' ' ')"
	note "$(sort -u "$SECRETS" | wc -l) Tokens gegen $(find "$OUT" -path "$WORK" -prune -o -type f -print | wc -l) Dateien geprüft"
}

# ---------------------------------------------------------------- Token-Ablauf mitten im Sync, doppelter Refresh
# Beide Fälle stellen den Testserver im Runner um (occ, OC_ROOT setzt der Workflow) und brauchen dafür die oauth2-App
# ab 0.10.0: Lebensdauer per access_token_lifetime, Gnadenfrist beim Austausch. Fremde Server bleiben unberührt.
TOKEN_LIFETIME=10
server_occ() { (cd "$OC_ROOT" && php occ --no-interaction "$@"); }
oauth2_at_least() { [ -n "${E2E_OAUTH2_VERSION:-}" ] && ! version_lt "$E2E_OAUTH2_VERSION" "$1"; }
skip_because() { STATUS=SKIP; NOTES+=("$*"); echo "  SKIP: $*"; }
# token_posts_in <log>: Anfragen an den Token-Endpunkt in einem Protokoll
token_posts_in() { count_req "$1" POST "$TOKEN_PATH_RE"; }

# A8: Das Access-Token lebt 10 s, der Sync dauert ein Vielfaches davon und überträgt dabei viele Dateien parallel (ohne
# Bulk: einzelne PUT und GET). Erwartet: keine neue Anmeldung, alles übertragen, Erneuerungen während des Laufs.
a8() {
	local tf=$WORK/oauth/token-a8.json i posts unauth started dur old
	if [ -z "${OC_ROOT:-}" ]; then
		skip_because "nur auf dem Testserver im Runner (Lebensdauer per occ)"
		return
	fi
	if ! oauth2_at_least 0.10.0; then
		skip_because "oauth2 ${E2E_OAUTH2_VERSION:-unbekannt} kennt access_token_lifetime nicht (ab 0.10.0)"
		return
	fi
	server_occ config:app:set oauth2 access_token_lifetime --value "$TOKEN_LIFETIME" >/dev/null || { case_error "occ config:app:set gescheitert"; return; }
	token_copy "$TOKEN_FILE" "$tf" || { case_error "Token-Datei nicht kopierbar"; return; }
	# abgelaufen: Der Lauf erneuert zuerst und bekommt ein Token mit 10 s Lebensdauer
	token_edit "$tf" '.access_token = "abgelaufen" | .expires_at = (now | floor) - 60'
	old=$(jq -r .refresh_token "$tf")
	for i in $(seq 1 100); do rnd "$L/hoch/datei-$i.bin"; done
	for i in $(seq 1 30); do seed "runter/datei-$i.bin" || { case_error "Serverdateien nicht anlegbar"; break; }; done
	TOKEN_FILE=$tf
	started=$SECONDS
	ocmd lauf1 env OWNCLOUD_BULK_UPLOAD=0
	dur=$((SECONDS - started))
	server_occ config:app:delete oauth2 access_token_lifetime >/dev/null
	check_run lauf1
	grep -q 'Authentication failed' "$LOG" && fail "lauf1: neue Anmeldung verlangt: $(grep -m1 -ao 'Authentication failed:.*' "$LOG" | cut -c1-160)"
	compare "$L" "$R"
	[ "$(server_files "$R")" -eq 130 ] || fail "auf dem Server $(server_files "$R") statt 130 Dateien"
	posts=$(token_posts) unauth=$(unauthorized)
	# die erste Erneuerung kommt vor dem Lauf, jede weitere mitten im Sync
	if [ "$posts" -lt 2 ]; then
		if [ "$dur" -le "$TOKEN_LIFETIME" ]; then
			case_error "Lauf dauerte nur ${dur}s, nicht länger als die Lebensdauer – Ablauf mitten im Sync nicht geprüft"
		else
			fail "keine Erneuerung während des Laufs (${dur}s, Lebensdauer ${TOKEN_LIFETIME}s, Token-Endpunkt ${posts}×)"
		fi
	fi
	[ "$(jq -r .refresh_token "$tf")" != "$old" ] || fail "neue Tokens nicht in die Token-Datei geschrieben"
	[ "$(jq -r '.expires_in // 0' "$tf")" -eq "$TOKEN_LIFETIME" ] || fail "expires_in in der Token-Datei ist nicht ${TOKEN_LIFETIME}"
	note "Lauf ${dur}s, Lebensdauer ${TOKEN_LIFETIME}s, Token-Endpunkt ${posts}×, 401: ${unauth}, $(upload_paths "$LOG")"
}

# A9: Doppelter Refresh. (1) Zwei Läufe gleichzeitig mit derselben Token-Datei: Die Sperre der Datei lässt genau einen
# erneuern, der andere übernimmt dessen Tokens. (2) Zwei Läufe gleichzeitig mit zwei Kopien desselben Refresh-Tokens
# (oauth2 ab 0.10.0): Der Server tauscht einmal aus und liefert der Wiederholung dasselbe Paar statt invalid_grant.
a9() {
	local tf=$WORK/oauth/token-a9.json tfa=$WORK/oauth/token-a9a.json tfb=$WORK/oauth/token-a9b.json
	local la=$WORK/$ID-a lb=$WORK/$ID-b loga=$LOGS/$ID-lauf1a.log logb=$LOGS/$ID-lauf1b.log pa pb rca rcb posts
	seed "geteilt.txt" || { case_error "Serverdatei nicht anlegbar"; return; }

	# (1) dieselbe Datei
	token_copy "$TOKEN_FILE" "$tf" || { case_error "Token-Datei nicht kopierbar"; return; }
	token_edit "$tf" '.access_token = "abgelaufen" | .expires_at = (now | floor) - 60'
	rm -rf "$la" "$lb" && mkdir -p "$la" "$lb"
	timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$la" "$OC_URL" --oauth-token-file "$tf" --path "/$R" </dev/null >"$loga" 2>&1 &
	pa=$!
	timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$lb" "$OC_URL" --oauth-token-file "$tf" --path "/$R" </dev/null >"$logb" 2>&1 &
	pb=$!
	wait "$pa"; rca=$?
	wait "$pb"; rcb=$?
	remember_tokens "$tf"
	echo "  gleiche Datei: exit $rca / $rcb"
	[ "$rca" -eq 0 ] || fail "gleiche Datei, Lauf a: Exit $rca$(grep -m1 -ao 'Authentication failed:.*' "$loga" | cut -c1-120 | sed 's/^/ – /')"
	[ "$rcb" -eq 0 ] || fail "gleiche Datei, Lauf b: Exit $rcb$(grep -m1 -ao 'Authentication failed:.*' "$logb" | cut -c1-120 | sed 's/^/ – /')"
	posts=$(($(token_posts_in "$loga") + $(token_posts_in "$logb")))
	[ "$posts" -eq 1 ] || fail "gleiche Datei: $posts Anfragen an den Token-Endpunkt statt einer"
	[ -f "$la/geteilt.txt" ] && [ -f "$lb/geteilt.txt" ] || fail "gleiche Datei: nicht beide Läufe haben synchronisiert"
	note "gleiche Datei: beide Exit 0, Token-Endpunkt ${posts}×"
	TOKEN_FILE=$tf

	# (2) zwei Kopien desselben Refresh-Tokens
	if ! oauth2_at_least 0.10.0; then
		note "zwei Kopien übersprungen: oauth2 ${E2E_OAUTH2_VERSION:-unbekannt} tauscht ohne Gnadenfrist (ab 0.10.0)"
		return
	fi
	token_copy "$TOKEN_FILE" "$tfa" && token_copy "$TOKEN_FILE" "$tfb" || { case_error "Token-Datei nicht kopierbar"; return; }
	token_edit "$tfa" '.access_token = "abgelaufen" | .expires_at = (now | floor) - 60'
	token_edit "$tfb" '.access_token = "abgelaufen" | .expires_at = (now | floor) - 60'
	rm -rf "$la" "$lb" && mkdir -p "$la" "$lb"
	loga=$LOGS/$ID-lauf2a.log logb=$LOGS/$ID-lauf2b.log
	timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$la" "$OC_URL" --oauth-token-file "$tfa" --path "/$R" </dev/null >"$loga" 2>&1 &
	pa=$!
	timeout -s KILL "$RUN_TIMEOUT" "$OCMD" "$lb" "$OC_URL" --oauth-token-file "$tfb" --path "/$R" </dev/null >"$logb" 2>&1 &
	pb=$!
	wait "$pa"; rca=$?
	wait "$pb"; rcb=$?
	remember_tokens "$tfa"
	remember_tokens "$tfb"
	echo "  zwei Kopien: exit $rca / $rcb"
	[ "$rca" -eq 0 ] || fail "zwei Kopien, Lauf a: Exit $rca$(grep -m1 -ao 'Authentication failed:.*' "$loga" | cut -c1-120 | sed 's/^/ – /')"
	[ "$rcb" -eq 0 ] || fail "zwei Kopien, Lauf b: Exit $rcb$(grep -m1 -ao 'Authentication failed:.*' "$logb" | cut -c1-120 | sed 's/^/ – /')"
	[ "$(jq -r .refresh_token "$tfa")" = "$(jq -r .refresh_token "$tfb")" ] || fail "zwei Kopien: verschiedene neue Refresh-Tokens (Gabelung)"
	[ "$(jq -r .refresh_token "$tfa")" != "$(jq -r .refresh_token "$TOKEN_FILE")" ] || fail "zwei Kopien: Refresh-Token nicht erneuert"
	note "zwei Kopien: beide Exit 0, dasselbe neue Paar (Token-Endpunkt $(($(token_posts_in "$loga") + $(token_posts_in "$logb")))×)"
	TOKEN_FILE=$tfa
}

# ---------------------------------------------------------------- Optionen aus 6.0.3 und Löschschutz
# Lokaler Ordner leer (wie ein nicht eingehängtes Laufwerk): ohne Rückfrage nichts löschen, Exit 3;
# mit „y“ auf der Standardeingabe löschen
t15() {
	has_option --non-interactive || { skip_unsupported --non-interactive; return; }
	seed a.txt b.txt ordner/c.txt || { case_error "Vorlage nicht anlegbar"; return; }
	ocmd lauf1
	check_run lauf1
	compare "$L" "$R" || return
	find "$L" -mindepth 1 -maxdepth 1 ! -name '.sync_*' -exec rm -rf {} +
	OCMD_ARGS=(--non-interactive)
	ocmd lauf2
	[ "$RC" = 3 ] || fail "lauf2: Exit $RC statt 3 – Abbruch ohne Löschen erwartet"
	[ "$(server_files "$R")" = 3 ] || fail "lauf2 hat auf dem Server gelöscht"
	grep -q "Nothing was changed" "$LOG" || fail "lauf2: Hinweis „Nothing was changed“ fehlt"
	OCMD_ARGS=() OCMD_STDIN=$WORK/stdin-$ID
	printf 'y\n' >"$OCMD_STDIN"
	ocmd lauf3
	check_run lauf3
	grep -q "Delete these files" "$LOG" || fail "lauf3: keine Rückfrage"
	[ "$(server_files "$R")" = 0 ] || fail "lauf3: nach der Bestätigung nicht gelöscht"
	compare "$L" "$R"
}

# --exclude: zusätzliche Ausschlussliste
t16() {
	has_option --exclude || { skip_unsupported --exclude; return; }
	rnd "$L/behalten.txt"
	rnd "$L/entwurf.skipme"
	rnd "$L/sub/auch.skipme"
	printf '*.skipme\n' >"$WORK/exclude-$ID.lst"
	OCMD_ARGS=(--exclude "$WORK/exclude-$ID.lst")
	ocmd lauf1
	check_run lauf1
	[ "$(dav stat "$R/behalten.txt" exists)" = true ] || fail "behalten.txt fehlt auf dem Server"
	[ "$(dav stat "$R/entwurf.skipme" exists)" = false ] || fail "ausgeschlossene Datei hochgeladen"
	[ "$(dav stat "$R/sub/auch.skipme" exists)" = false ] || fail "ausgeschlossene Datei im Unterordner hochgeladen"
}

# --unsyncedfolders: abgewählte Serverordner nicht herunterladen, nach dem Wiederwählen schon
t17() {
	has_option --unsyncedfolders || { skip_unsupported --unsyncedfolders; return; }
	seed bleibt/x.txt weg/y.txt weg/tief/z.txt || { case_error "Vorlage nicht anlegbar"; return; }
	printf '# nicht synchronisieren\nweg\n' >"$WORK/unsynced-$ID.lst"
	OCMD_ARGS=(--unsyncedfolders "$WORK/unsynced-$ID.lst")
	ocmd lauf1
	check_run lauf1
	[ -f "$L/bleibt/x.txt" ] || fail "bleibt/x.txt fehlt lokal"
	[ ! -e "$L/weg" ] || fail "abgewählter Ordner heruntergeladen"
	[ "$(dav stat "$R/weg/tief/z.txt" exists)" = true ] || fail "abgewählter Ordner auf dem Server verändert"
	: >"$WORK/unsynced-$ID.lst"
	ocmd lauf2
	check_run lauf2
	[ -f "$L/weg/tief/z.txt" ] || fail "wieder gewählter Ordner nicht heruntergeladen"
	compare "$L" "$R"
}

# Versteckte Dateien wie in 6.0.3 nur mit --sync-hidden-files
t18() {
	has_option --sync-hidden-files || { skip_unsupported --sync-hidden-files; return; }
	rnd "$L/sichtbar.txt"
	rnd "$L/.versteckt"
	ocmd lauf1
	check_run lauf1
	[ "$(dav stat "$R/sichtbar.txt" exists)" = true ] || fail "sichtbare Datei fehlt auf dem Server"
	[ "$(dav stat "$R/.versteckt" exists)" = false ] || fail "versteckte Datei ohne --sync-hidden-files hochgeladen"
	OCMD_ARGS=(--sync-hidden-files)
	ocmd lauf2
	check_run lauf2
	[ "$(dav stat "$R/.versteckt" exists)" = true ] || fail "versteckte Datei mit --sync-hidden-files nicht hochgeladen"
}

# --uplimit/--downlimit: je 3 MiB bei 512 KB/s brauchen gut 6 s; beide Richtungen laufen gleichzeitig, ungedrosselt
# dauert es auf dem Testserver unter einer Sekunde. Prüfgrenze 4 s (Startguthaben höchstens 2 s).
t19() {
	has_option --uplimit || { skip_unsupported --uplimit; return; }
	local start secs
	rnd_mib "$WORK/seed-$ID/runter.bin" 3 && dav put "$WORK/seed-$ID/runter.bin" "$R/runter.bin" || { case_error "Vorlage nicht anlegbar"; return; }
	rnd_mib "$L/hoch.bin" 3
	OCMD_ARGS=(--uplimit 512 --downlimit 512)
	start=$SECONDS
	ocmd lauf1
	secs=$((SECONDS - start))
	check_run lauf1
	compare "$L" "$R"
	[ "$secs" -ge 4 ] || fail "je 3 MiB hoch und herunter nach ${secs}s – die Grenzen greifen nicht"
	note "je 3 MiB hoch und herunter mit 512 KB/s in ${secs}s"
}

# --version, --silent (nur Fehler), --logdebug (Debug-Zeilen)
t20() {
	has_option --logdebug || { skip_unsupported --logdebug; return; }
	local version
	version=$("$OCMD" --version 2>&1) || fail "--version: Exit $?"
	[[ $version == owncloudcmd\ * ]] || fail "--version: unerwartete Ausgabe ${version:0:80}"
	rnd "$L/a.txt"
	OCMD_ARGS=(--silent)
	ocmd lauf1
	check_run lauf1
	! grep -q "Sync finished successfully" "$LOG" || fail "--silent gibt Meldungen aus"
	rnd "$L/b.txt"
	OCMD_ARGS=(--logdebug)
	ocmd lauf2
	check_run lauf2
	grep -q "SQL prepare" "$LOG" || fail "--logdebug ohne Debug-Zeilen"
	compare "$L" "$R"
	note "${version:0:60}"
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
	"15|Löschschutz: leerer lokaler Ordner löscht nur nach Rückfrage|t15"
	"16|--exclude: zusätzliche Ausschlussliste|t16"
	"17|--unsyncedfolders: Serverordner abwählen|t17"
	"18|Versteckte Dateien nur mit --sync-hidden-files|t18"
	"19|--uplimit/--downlimit drosseln|t19"
	"20|--version, --silent, --logdebug|t20"
)

if [ "$AUTH" = oauth ]; then
	# Sync-Fälle über OAuth2: Upload klein/Bulk/Chunking, Download, Umbenennen, Löschen, Konflikt
	SYNC_CASES=("${CASES[@]}")
	CASES=(
		"A1|OAuth2-Anmeldung wie der Desktop-Client (redirect_uri localhost)|a1"
		"A2|OAuth2-Anmeldung über 127.0.0.1 (RFC 8252)|a2"
	)
	for entry in "${SYNC_CASES[@]}"; do
		case ${entry%%|*} in 01 | 02 | 03 | 04 | 05 | 07 | 08) CASES+=("$entry") ;; esac
	done
	CASES+=(
		"A3|Access-Token abgelaufen: vor dem Lauf erneuert|a3"
		"A4|Access-Token abgelehnt (401): erneuert und wiederholt|a4"
		"A8|Access-Token läuft während des Syncs mehrfach ab (parallele Übertragungen)|a8"
		"A9|Doppelter Refresh: gleiche Token-Datei und zwei Kopien gleichzeitig|a9"
		"A5|Refresh-Token verbraucht: Abbruch mit Exit 2|a5"
	)
	[ "${E2E_OAUTH_REVOKE:-}" != 1 ] || CASES+=("A6|Freigabe in den Einstellungen widerrufen: Abbruch mit Exit 2|a6")
	CASES+=("A7|Keine Tokens in Protokollen und Ergebnissen|a7")
fi

# skip_case <nr> <titel> <grund>: Fall ohne Lauf als ERROR verbuchen
skip_case() {
	ID=$1 TITLE=$2 STATUS=ERROR NOTES=("Suite: $3") STARTED=$SECONDS
	echo "::group::Fall $ID – $TITLE"
	case_end
}

rm -rf "$WORK" "$LOGS"
mkdir -p "$WORK" "$LOGS"
[ -x "$OCMD" ] || { echo "owncloudcmd nicht ausführbar: $OCMD" >&2; exit 2; }
[ "$(dav stat '' exists)" = true ] || { echo "Server $OC_URL nicht erreichbar oder Anmeldung abgelehnt" >&2; exit 2; }
if [ "$AUTH" = oauth ] && ! command -v jq >/dev/null; then
	echo "jq fehlt" >&2
	exit 2
fi

for entry in "${CASES[@]}"; do
	IFS='|' read -r nr title fn <<<"$entry"
	# ohne Token-Datei läuft über OAuth2 nichts: A1 auch dann, wenn nur andere Fälle gewählt sind
	if [ $# -gt 0 ] && [[ " $* " != *" $nr "* ]] && ! [[ $AUTH == oauth && $nr == A1 ]]; then continue; fi
	if [ "$AUTH" = oauth ] && [ -z "$TOKEN_FILE" ] && [[ $nr != A1 && $nr != A2 && $nr != A7 ]]; then
		skip_case "$nr" "$title" "keine Token-Datei – A1 gescheitert"
		continue
	fi
	run_case "$nr" "$title" "$fn"
done

pass=0 failed=0 errors=0 skipped=0
{
	echo "| Nr | Fall | Ergebnis | Dauer | Befund |"
	echo "|---|---|---|---|---|"
	for row in "${ROWS[@]}"; do
		IFS=$'\t' read -r nr title st secs text <<<"$row"
		case $st in PASS) pass=$((pass + 1)) ;; FAIL) failed=$((failed + 1)) ;; SKIP) skipped=$((skipped + 1)) ;; *) errors=$((errors + 1)) ;; esac
		echo "| $nr | $title | **$st** | ${secs}s | ${text//|/\\|} |"
	done
	echo
	echo "PASS $pass, FAIL $failed, ERROR $errors, SKIP $skipped"
	echo
	echo "_FAIL: Fehlverhalten des Clients, Grund in der Spalte Befund. ERROR: Suite oder Umgebung, Fall nicht bewertbar. SKIP: Option im getesteten Stand noch nicht vorhanden._"
} >"$OUT/results.md"
printf '%s\n' "${ROWS[@]}" >"$OUT/results.tsv"
cat "$OUT/results.md"

[ "$errors" -eq 0 ] || exit 2
[ "$failed" -eq 0 ] || exit 1
exit 0

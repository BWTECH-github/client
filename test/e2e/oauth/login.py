#!/usr/bin/env python3
"""OAuth2-Anmeldung des Desktop-Clients per Skript nachstellen (oauth2-App von owncloud.online).

Der Ablauf folgt src/gui/creds/oauth.cpp (OAuth::startAuthentication, authorisationLink,
getTokens):

  1. .well-known/openid-configuration abfragen; ohne brauchbare Antwort gelten die Pfade
     der oauth2-App (/index.php/apps/oauth2/authorize und .../api/v1/token)
  2. Autorisierungs-URL mit denselben Parametern wie der Client: response_type=code,
     client_id, redirect_uri=http://<host>:<port>, PKCE (S256), scope, prompt, state
  3. Anmeldeseite mit requesttoken, Benutzer und Kennwort absenden (Cookies wie im Browser)
  4. Zustimmungsseite absenden, die Weiterleitung mit dem Code abfangen statt ihr zu folgen
  5. state prüfen, Code am Token-Endpunkt tauschen (client_secret_basic, code_verifier)
  6. Access-Token an ocs/v2.php/cloud/user prüfen, Token-Datei für owncloudcmd schreiben

Aufruf (Zugang nur über die Umgebung, nie auf der Befehlszeile):
  OC_USER=… OC_PASS=… python3 -I login.py --server URL --out token.json \
      [--redirect-host localhost|127.0.0.1]
  OC_USER=… OC_PASS=… python3 -I login.py --server URL --revoke
      Freigabe des Desktop Clients in den persönlichen Einstellungen widerrufen, wie ein
      Nutzer es tut (die oauth2-App löscht dabei alle Tokens dieses Clients für das Konto)

Exit: 0 angemeldet, 4 der Server lehnt die Anfrage ab (z. B. redirect_uri), 1 sonstiger Fehler.
Code und Tokens gibt das Skript nie aus; maskieren muss der Aufrufer (run-suite.sh).
"""

import argparse
import base64
import hashlib
import html.parser
import http.cookiejar
import json
import os
import re
import secrets
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

# OAuth2-Client des Desktop-Clients (src/libsync/theme.cpp: oauthClientId/oauthClientSecret,
# openIdConnectScopes, openIdConnectPrompt); die oauth2-App legt ihn als "Desktop Client" an
CLIENT_ID = 'xdXOt13JKxym1B1QcEncf2XDkLAexMBFwiT9j6EfhhHFJhs2KM9jbjTmf8JBXE69'
CLIENT_SECRET = 'UBntmLjC2yYCeHwsyj73Uwo9TAaecAetRwMw0xYcvNL9yRdLSUi0hUAHfvCHFeFh'
SCOPES = 'openid offline_access email profile'
# Theme "select_account consent"; der Client schickt die Werte in fester Reihenfolge (toString)
PROMPT = 'consent select_account'
AUTHORIZE_PATH = '/index.php/apps/oauth2/authorize'
TOKEN_PATH = '/index.php/apps/oauth2/api/v1/token'
TIMEOUT = 60
USER_AGENT = 'Mozilla/5.0 (owncloud.online E2E OAuth2 login)'

EXIT_FAILED = 1
EXIT_REFUSED = 4


class Refused(Exception):
    """Der Server lehnt die Autorisierung ab (Fehlerseite oder error= in der Weiterleitung)."""


class Failed(Exception):
    pass


def b64url(data, padding=True):
    text = base64.urlsafe_b64encode(data).decode()
    return text if padding else text.rstrip('=')


class Forms(html.parser.HTMLParser):
    """Formulare einer Seite mit ihren Eingabefeldern."""

    def __init__(self):
        super().__init__()
        self.forms = []

    def handle_starttag(self, tag, attrs):
        a = {k: (v or '') for k, v in attrs}
        if tag == 'form':
            self.forms.append({'action': a.get('action', ''), 'method': a.get('method', 'get').lower(), 'fields': {}})
        elif tag == 'input' and self.forms and a.get('name'):
            if a.get('type', 'text').lower() in ('checkbox', 'radio') and 'checked' not in a:
                return
            self.forms[-1]['fields'][a['name']] = a.get('value', '')


DESKTOP_REVOKE = re.compile(
    r'<td>\s*Desktop Client\s*</td>\s*<td>\s*<form[^>]*\baction="([^"]*/revoke)"[^>]*>\s*'
    r'<input[^>]*\bname="requesttoken"[^>]*\bvalue="([^"]*)"', re.S)


def post_form_with_token(body):
    parser = Forms()
    parser.feed(body.decode('utf-8', 'replace'))
    for form in parser.forms:
        if form['method'] == 'post' and 'requesttoken' in form['fields']:
            return form
    return None


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class Browser:
    """HTTP mit Cookies; Weiterleitungen werden einzeln behandelt."""

    def __init__(self):
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()), NoRedirect())

    def request(self, url, data=None, headers=None):
        body = urllib.parse.urlencode(data).encode() if isinstance(data, dict) else data
        # wie ein Browser: ohne Accept text/html antwortet der Kern mit 401 statt zur Anmeldung weiterzuleiten
        headers = headers or {'Accept': 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8'}
        headers.setdefault('User-Agent', USER_AGENT)
        req = urllib.request.Request(url, data=body, headers=headers, method='POST' if body is not None else 'GET')
        try:
            with self.opener.open(req, timeout=TIMEOUT) as resp:
                return resp.status, resp.headers, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read()

    def follow(self, url, data=None, stop=None, hops=10):
        """Folgt Weiterleitungen des Servers; hält vor einer an, für die stop(ziel) wahr ist."""
        for _ in range(hops):
            status, headers, body = self.request(url, data)
            if status not in (301, 302, 303, 307, 308):
                return status, url, body, None
            target = urllib.parse.urljoin(url, headers.get('Location', ''))
            if stop and stop(target):
                return status, url, body, target
            url, data = target, None
        raise Failed(f'zu viele Weiterleitungen ab {url}')


def discover(browser, server):
    """Wie OAuth::fetchWellKnown: Endpunkte aus .well-known, sonst die der oauth2-App."""
    fallback = (server + AUTHORIZE_PATH, server + TOKEN_PATH, 'oauth2-App (kein .well-known)')
    status, _, body = browser.request(server + '/.well-known/openid-configuration')
    if status != 200:
        return fallback
    try:
        data = json.loads(body)
    except ValueError:
        return fallback
    auth, token = data.get('authorization_endpoint'), data.get('token_endpoint')
    if not (isinstance(auth, str) and isinstance(token, str) and auth.startswith('http') and token.startswith('http')):
        return fallback
    return auth, token, '.well-known/openid-configuration'


def free_port(host):
    # Der Client lauscht auf einem freien Port (Theme::oauthPorts = 0)
    with socket.socket() as s:
        s.bind(('127.0.0.1' if host == 'localhost' else host, 0))
        return s.getsockname()[1]


def submit_login(browser, page_url, body, user, password):
    """Sendet die Anmeldeseite des Kerns ab; False, wenn body keine Anmeldeseite ist."""
    form = post_form_with_token(body)
    if form is None or 'password' not in form['fields']:
        return False
    fields = dict(form['fields'])
    fields.update({'user': user, 'password': password, 'timezone': 'UTC', 'timezone-offset': '0'})
    status, _, _ = browser.request(urllib.parse.urljoin(page_url, form['action']), fields)
    if status not in (302, 303):
        raise Failed(f'Anmeldung: unerwartete Antwort HTTP {status}')
    return True


def login(server, user, password, redirect_host):
    browser = Browser()
    authorize_endpoint, token_endpoint, source = discover(browser, server)
    print(f'Endpunkte: {source}')

    redirect_uri = f'http://{redirect_host}:{free_port(redirect_host)}'
    verifier = b64url(secrets.token_bytes(96))  # wie generateRandomString(24): 96 Byte -> 128 Zeichen
    state = b64url(secrets.token_bytes(32))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest(), padding=False)
    query = urllib.parse.urlencode({
        'response_type': 'code',
        'client_id': CLIENT_ID,
        'redirect_uri': redirect_uri,
        'code_challenge': challenge,
        'code_challenge_method': 'S256',
        'scope': SCOPES,
        'prompt': PROMPT,
        'state': state,
    }, quote_via=urllib.parse.quote)
    authorize_url = f'{authorize_endpoint}?{query}'
    to_client = urllib.parse.urlsplit(redirect_uri).netloc

    def reaches_client(target):
        return urllib.parse.urlsplit(target).netloc == to_client

    # Anmeldeseite: die oauth2-App verlangt eine Sitzung und leitet zur Anmeldung weiter;
    # danach mit der Sitzung erneut zur Autorisierung, wie der Browser nach der Anmeldung
    status, page_url, body, target = browser.follow(authorize_url, stop=reaches_client)
    if target is None and submit_login(browser, page_url, body, user, password):
        print('Anmeldeseite: abgesendet')
        status, page_url, body, target = browser.follow(authorize_url, stop=reaches_client)
    if target is None:
        form = post_form_with_token(body)
        if form is not None and 'password' in form['fields']:
            raise Failed('Anmeldung mit Benutzer und Kennwort abgelehnt (wieder die Anmeldeseite)')
        if form is None:
            if b'class="error"' in body:
                raise Refused(f'Fehlerseite der oauth2-App (HTTP {status}) – Anfrage abgelehnt, z. B. redirect_uri {redirect_uri}')
            raise Failed(f'keine Zustimmungsseite unter {page_url} (HTTP {status})')
        # Zustimmung: das Formular schickt sich an dieselbe URL samt Parametern
        status, page_url, body, target = browser.follow(
            urllib.parse.urljoin(page_url, form['action']) if form['action'] else page_url,
            data={'requesttoken': form['fields']['requesttoken']}, stop=reaches_client)
        print('Zustimmungsseite: abgesendet')
    else:
        print('Zustimmung: nicht nötig (vertrauenswürdiger Client)')
    if target is None:
        raise Failed(f'keine Weiterleitung zum Client nach der Zustimmung (HTTP {status} unter {page_url})')

    params = urllib.parse.parse_qs(urllib.parse.urlsplit(target).query)
    if 'error' in params:
        raise Refused(f'Weiterleitung mit error={params["error"][0]}')
    code = params.get('code', [''])[0]
    if not code:
        raise Failed('Weiterleitung zum Client ohne code')
    if params.get('state', [''])[0] != state:
        raise Failed('state der Weiterleitung passt nicht')
    print(f'Code erhalten über {redirect_uri}')

    # Code tauschen wie OAuth::getTokens / postTokenRequest (client_secret_basic)
    basic = base64.b64encode(f'{CLIENT_ID}:{CLIENT_SECRET}'.encode()).decode()
    status, _, body = browser.request(token_endpoint, {
        'grant_type': 'authorization_code',
        'code': code,
        'redirect_uri': redirect_uri,
        'code_verifier': verifier,
        'scope': SCOPES,
    }, headers={'Authorization': f'Basic {basic}', 'Content-Type': 'application/x-www-form-urlencoded; charset=UTF-8'})
    try:
        tokens = json.loads(body)
    except ValueError:
        raise Failed(f'Token-Endpunkt: keine JSON-Antwort (HTTP {status})')
    if status != 200 or 'error' in tokens:
        raise Failed(f'Token-Endpunkt: HTTP {status} {tokens.get("error", "")} {tokens.get("error_description", "")}'.strip())
    for field in ('access_token', 'refresh_token'):
        if not tokens.get(field):
            raise Failed(f'Token-Endpunkt: {field} fehlt')
    if str(tokens.get('token_type', '')).lower() != 'bearer':
        raise Failed(f'Token-Endpunkt: token_type {tokens.get("token_type")!r} statt Bearer')

    # Das Token muss die Benutzerkennung liefern, wie ConnectionValidator/UserInfo im Client
    status, _, body = browser.request(server + '/ocs/v2.php/cloud/user?format=json', headers={
        'Authorization': f'Bearer {tokens["access_token"]}', 'OCS-APIRequest': 'true'})
    try:
        user_id = json.loads(body)['ocs']['data']['id']
    except (ValueError, KeyError, TypeError):
        raise Failed(f'ocs/v2.php/cloud/user mit dem neuen Token: HTTP {status}')
    print(f'Token: Bearer, gültig {tokens.get("expires_in", "?")} s, Benutzer {user_id}')

    content = {
        'server_url': server,
        'user_id': tokens.get('user_id') or user_id,
        'access_token': tokens['access_token'],
        'refresh_token': tokens['refresh_token'],
        'token_type': 'Bearer',
    }
    if isinstance(tokens.get('expires_in'), int):
        content['expires_in'] = tokens['expires_in']
        content['expires_at'] = int(time.time()) + tokens['expires_in']
    if token_endpoint != server + TOKEN_PATH:
        content['token_endpoint'] = token_endpoint
    return content


def revoke(server, user, password):
    """Wie ein Nutzer: anmelden, persönliche Einstellungen > Sicherheit, Desktop Client widerrufen."""
    browser = Browser()
    status, page_url, body, _ = browser.follow(server + '/index.php/login')
    if not submit_login(browser, page_url, body, user, password):
        raise Failed(f'keine Anmeldeseite unter {page_url} (HTTP {status})')
    settings = server + '/index.php/settings/personal?sectionid=security'
    status, page_url, body, _ = browser.follow(settings)
    match = DESKTOP_REVOKE.search(body.decode('utf-8', 'replace'))
    if not match:
        form = post_form_with_token(body)
        if form is not None and 'password' in form['fields']:
            raise Failed('Anmeldung mit Benutzer und Kennwort abgelehnt')
        raise Failed(f'„Desktop Client“ steht nicht unter den freigegebenen Anwendungen (HTTP {status})')
    action, token = html.unescape(match.group(1)), html.unescape(match.group(2))
    status, _, _ = browser.request(urllib.parse.urljoin(page_url, action), {'requesttoken': token})
    if status not in (302, 303):
        raise Failed(f'Widerruf: HTTP {status}')
    status, page_url, body, _ = browser.follow(settings)
    if DESKTOP_REVOKE.search(body.decode('utf-8', 'replace')):
        raise Failed('Freigabe nach dem Widerruf noch vorhanden')
    print('Freigabe des Desktop Clients widerrufen')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--server', required=True, help='Server-URL, z. B. http://127.0.0.1:8080')
    parser.add_argument('--out', help='Token-Datei für owncloudcmd --oauth-token-file')
    parser.add_argument('--revoke', action='store_true',
                        help='Freigabe des Desktop Clients in den persönlichen Einstellungen widerrufen')
    parser.add_argument('--redirect-host', default='localhost', choices=('localhost', '127.0.0.1'),
                        help='Host der redirect_uri (der Client nutzt localhost)')
    args = parser.parse_args()
    if not args.revoke and not args.out:
        parser.error('--out fehlt')
    user, password = os.environ.get('OC_USER'), os.environ.get('OC_PASS')
    if not user or not password:
        print('OC_USER und OC_PASS fehlen', file=sys.stderr)
        return EXIT_FAILED

    try:
        if args.revoke:
            revoke(args.server.rstrip('/'), user, password)
            return 0
        content = login(args.server.rstrip('/'), user, password, args.redirect_host)
    except Refused as e:
        print(f'abgelehnt: {e}', file=sys.stderr)
        return EXIT_REFUSED
    except (Failed, OSError) as e:
        print(f'Fehler: {e}', file=sys.stderr)
        return EXIT_FAILED

    # nur für den Eigentümer lesbar, wie owncloudcmd die Datei auch selbst schreibt
    fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        json.dump(content, f, indent=2)
    print(f'Token-Datei: {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

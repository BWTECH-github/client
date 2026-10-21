#!/usr/bin/env python3
"""Browser-Anmeldung für den Kontoassistenten des Desktop-Clients nachstellen (Install-Probe, Szenario assistent).

Der Client öffnet im Assistenten den Anmelde-Link der oauth2-App (OAuth::authorisationLink: redirect_uri
http://localhost:<Port>, state, PKCE) und wartet auf diesem Port auf die Weiterleitung des Browsers. Dieses
Skript übernimmt die Rolle des Browsers mit genau diesem Link:

  1. Link aufrufen, Anmeldeseite mit Benutzer und Kennwort absenden (Cookies wie im Browser)
  2. Zustimmungsseite absenden, falls die oauth2-App sie zeigt
  3. die Weiterleitung zum Client abfangen, state prüfen und sie beim Client abliefern (GET an
     http://localhost:<Port>/?code=…&state=…) – den Code tauscht der Client selbst

Formulare, Weiterleitungen und Fehlerbilder kommen aus test/e2e/oauth/login.py (Zweig sync-e2e); das
Skript lädt es aus --login-py und ändert es nicht.

Aufruf (Zugang nur über die Umgebung, nie auf der Befehlszeile):
  OC_USER=… OC_PASS=… python -I assistent-anmeldung.py --login-py …/login.py --url-file link.txt

Exit: 0 Weiterleitung beim Client abgeliefert, 4 der Server lehnt die Anfrage ab, 5 keine oauth2-App,
1 sonstiger Fehler. Code, Tokens und Zugangsdaten gibt das Skript nie aus.
"""

import argparse
import importlib.util
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

LOOPBACK_HOSTS = ('localhost', '127.0.0.1', '::1')


def load_login(path):
    spec = importlib.util.spec_from_file_location('e2e_oauth_login', path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f'{path} nicht ladbar')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def deliver(login, target):
    """Liefert die Weiterleitung beim Client ab wie ein Browser, ohne dessen Antwort (303 zur Meldungsseite) zu folgen."""
    opener = urllib.request.build_opener(login.NoRedirect())
    request = urllib.request.Request(target, headers={'User-Agent': login.USER_AGENT, 'Accept': 'text/html'})
    try:
        with opener.open(request, timeout=login.TIMEOUT) as response:
            response.read()
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def complete(login, authorize_url, user, password):
    parts = urllib.parse.urlsplit(authorize_url)
    if parts.scheme != 'https':
        raise login.Failed('der Anmelde-Link verwendet kein https')
    query = urllib.parse.parse_qs(parts.query)
    redirect_uri = query.get('redirect_uri', [''])[0]
    state = query.get('state', [''])[0]
    redirect = urllib.parse.urlsplit(redirect_uri)
    if redirect.scheme != 'http' or redirect.hostname not in LOOPBACK_HOSTS or not redirect.port:
        raise login.Failed('redirect_uri des Anmelde-Links zeigt nicht auf den Client (http://localhost:<Port>)')
    if not state:
        raise login.Failed('der Anmelde-Link enthält kein state')
    print(f'Anmelde-Link: {parts.netloc}{parts.path}, Rückruf an Port {redirect.port}')

    browser = login.Browser()

    def reaches_client(target):
        return urllib.parse.urlsplit(target).netloc == redirect.netloc

    # wie login.login(): die oauth2-App verlangt eine Sitzung und leitet zur Anmeldung weiter
    status, page_url, body, target = browser.follow(authorize_url, stop=reaches_client)
    print(f'Aufruf: {browser.trail()}')
    if target is None and login.submit_login(browser, page_url, body, user, password):
        print('Anmeldeseite: abgesendet')
        status, page_url, body, target = browser.follow(authorize_url, stop=reaches_client)
        print(f'Aufruf nach der Anmeldung: {browser.trail()}')
    if target is None:
        form = login.post_form_with_token(body)
        if form is not None and 'password' in form['fields']:
            raise login.Failed('Anmeldung mit Benutzer und Kennwort abgelehnt (wieder die Anmeldeseite)')
        if form is None:
            if b'class="error"' in body:
                raise login.Refused(f'Fehlerseite der oauth2-App (HTTP {status})')
            trail = browser.trail()
            if not login.oauth2_answers(browser, f'{parts.scheme}://{parts.netloc}{login.TOKEN_PATH}'):
                raise login.NoOAuth2(f'keine oauth2-App auf dem Server ({trail})')
            raise login.Failed(f'keine Zustimmungsseite ({trail})')
        status, page_url, body, target = browser.follow(
            urllib.parse.urljoin(page_url, form['action']) if form['action'] else page_url,
            data={'requesttoken': form['fields']['requesttoken']}, stop=reaches_client)
        print(f'Zustimmungsseite: abgesendet ({browser.trail()})')
    else:
        print('Zustimmung: nicht nötig (vertrauenswürdiger Client)')
    if target is None:
        raise login.Failed(f'keine Weiterleitung zum Client nach der Zustimmung ({browser.trail()})')

    params = urllib.parse.parse_qs(urllib.parse.urlsplit(target).query)
    if 'error' in params:
        raise login.Refused(f'Weiterleitung mit error={params["error"][0]}')
    if not params.get('code', [''])[0]:
        raise login.Failed('Weiterleitung zum Client ohne code')
    if params.get('state', [''])[0] != state:
        raise login.Failed('state der Weiterleitung passt nicht zum Anmelde-Link')
    print('Weiterleitung zum Client erhalten (code und state vorhanden)')

    status = deliver(login, target)
    print(f'Rückruf beim Client abgeliefert: HTTP {status}')
    if status not in (200, 303):
        raise login.Failed(f'der Client beantwortete den Rückruf mit HTTP {status}')
    report_capabilities(browser, f'{parts.scheme}://{parts.netloc}')


def report_capabilities(browser, server):
    """Welche Abfragen der Client nach dem Verbinden stellt, hängt an den Capabilities des Kontos: Benachrichtigungen
    nur mit notifications/ocs-endpoints (Capabilities::notificationsAvailable). Mit der Sitzung des Browsers gelesen."""
    status, _, body = browser.request(server + '/ocs/v2.php/cloud/capabilities?format=json',
                                      headers={'OCS-APIRequest': 'true', 'Accept': 'application/json'})
    try:
        capabilities = json.loads(body)['ocs']['data']['capabilities']
    except (ValueError, KeyError, TypeError):
        print(f'Capabilities: nicht lesbar (HTTP {status})')
        return
    notifications = isinstance(capabilities.get('notifications'), dict) and 'ocs-endpoints' in capabilities['notifications']
    print('Capabilities: notifications mit ocs-endpoints: {}; activity: {}'.format(
        'ja' if notifications else 'nein', 'ja' if 'activity' in capabilities else 'nein'))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--login-py', required=True, help='test/e2e/oauth/login.py aus dem Zweig sync-e2e')
    parser.add_argument('--url-file', required=True, help='Datei mit dem Anmelde-Link aus dem Assistenten')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8', line_buffering=True)
    sys.stderr.reconfigure(encoding='utf-8')

    user, password = os.environ.get('OC_USER'), os.environ.get('OC_PASS')
    if not user or not password:
        print('OC_USER und OC_PASS fehlen', file=sys.stderr)
        return 1
    try:
        login = load_login(args.login_py)
    except (OSError, RuntimeError, SyntaxError) as error:
        print(f'login.py nicht ladbar: {error}', file=sys.stderr)
        return 1
    with open(args.url_file, encoding='utf-8') as handle:
        authorize_url = handle.read().strip()

    try:
        complete(login, authorize_url, user, password)
    except login.Refused as error:
        print(f'abgelehnt: {error}', file=sys.stderr)
        return login.EXIT_REFUSED
    except login.NoOAuth2 as error:
        print(f'ohne oauth2: {error}', file=sys.stderr)
        return login.EXIT_NO_OAUTH2
    except (login.Failed, OSError) as error:
        print(f'Fehler: {error}', file=sys.stderr)
        return login.EXIT_FAILED
    return 0


if __name__ == '__main__':
    sys.exit(main())

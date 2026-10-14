#!/usr/bin/env python3
"""WebDAV- und Protokollwerkzeug für die Sync-E2E-Suite (nur Python-Standardbibliothek).

Die Suite prüft damit den Serverzustand unabhängig vom Client.

Zugang über die Umgebung: OC_URL (z. B. http://127.0.0.1:8080), OC_USER, OC_PASS.
Serverpfade sind relativ zur Wurzel des Benutzers (remote.php/dav/files/<user>/),
im Klartext (UTF-8); kodiert wird hier.

Unterbefehle:
  mkdirs <pfad>                 Ordner samt Eltern anlegen
  put <lokale-datei> <pfad>     Datei hochladen (Eltern werden angelegt)
  delete <pfad>                 löschen (fehlt er schon, gilt das als erledigt)
  move <von> <nach>             auf dem Server verschieben/umbenennen
  stat <pfad> [feld]            JSON {exists, type, size, fileid, etag} oder nur ein Feld
  sha <pfad>                    SHA-256 des Serverinhalts oder MISSING
  manifest <pfad> [--skip-conflicts]
                                Baum rekursiv: "rel<TAB>sha256" bzw. "rel/<TAB>DIR"
  localmanifest <ordner> [--skip-conflicts]
                                dasselbe für einen lokalen Ordner (ohne Client-Journal)
  httplog <owncloudcmd-log>     "REQ METHODE PFAD" / "RES METHODE STATUS PFAD"
"""
import base64
import hashlib
import http.client
import json
import os
import re
import sys
import urllib.parse
import xml.etree.ElementTree as ET

BLOCK = 1 << 20
DAV = '{DAV:}'
OC = '{http://owncloud.org/ns}'
# Journal, Protokoll und temporäre Downloads des Clients sind kein Nutzinhalt
CLIENT_FILES_RE = re.compile(
    r'^(\.sync_.*\.db.*|\._sync_.*\.db.*|\.csync_journal\.db.*|\.owncloudsync\.log.*|\..*\.~[0-9a-f]+)$')
# Muster wie Utility::isConflictFile() im Client
CONFLICT_RE = re.compile(r'_conflict-|\(conflicted copy')
PROPFIND_BODY = (b'<?xml version="1.0"?><d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns">'
                 b'<d:prop><d:resourcetype/><d:getcontentlength/><d:getetag/><oc:fileid/></d:prop>'
                 b'</d:propfind>')


class DavError(Exception):
    pass


class Server:
    def __init__(self):
        missing = [n for n in ('OC_URL', 'OC_USER', 'OC_PASS') if not os.environ.get(n)]
        if missing:
            raise DavError('Umgebungsvariablen fehlen: ' + ', '.join(missing))
        self.base = urllib.parse.urlsplit(os.environ['OC_URL'].rstrip('/'))
        user = os.environ['OC_USER']
        token = base64.b64encode(f'{user}:{os.environ["OC_PASS"]}'.encode()).decode()
        self.auth = 'Basic ' + token
        self.root = f'{self.base.path}/remote.php/dav/files/{urllib.parse.quote(user)}'

    def href(self, rel):
        rel = rel.strip('/')
        return self.root + ('/' + urllib.parse.quote(rel) if rel else '')

    def rel_of(self, href):
        path = urllib.parse.unquote(href).rstrip('/')
        root = urllib.parse.unquote(self.root)
        if path != root and not path.startswith(root + '/'):
            raise DavError(f'unerwartete href {href}')
        return path[len(root):].strip('/')

    def call(self, method, rel, body=None, headers=None, sink=None):
        """Eine Anfrage; liefert (status, rumpf). sink bekommt bei 200 den Rumpf blockweise."""
        cls = http.client.HTTPSConnection if self.base.scheme == 'https' else http.client.HTTPConnection
        conn = cls(self.base.hostname, self.base.port, timeout=900)
        try:
            conn.request(method, self.href(rel), body=body,
                         headers={'Authorization': self.auth, **(headers or {})})
            resp = conn.getresponse()
            if sink is not None and resp.status == 200:
                while True:
                    block = resp.read(BLOCK)
                    if not block:
                        return resp.status, b''
                    sink(block)
            return resp.status, resp.read()
        finally:
            conn.close()

    def mkdirs(self, rel):
        parts = [p for p in rel.strip('/').split('/') if p]
        for i in range(1, len(parts) + 1):
            sub = '/'.join(parts[:i])
            status, _ = self.call('MKCOL', sub)
            if status not in (201, 405):  # 405: existiert bereits
                raise DavError(f'MKCOL {sub}: HTTP {status}')

    def put(self, local, rel):
        parent = rel.strip('/').rpartition('/')[0]
        if parent:
            self.mkdirs(parent)
        # Content-Length ausdrücklich setzen: sonst sendet http.client chunked, das lehnt der Server ab
        with open(local, 'rb') as fh:
            status, data = self.call('PUT', rel, body=fh,
                                     headers={'Content-Length': str(os.fstat(fh.fileno()).st_size)})
        if status not in (201, 204):
            raise DavError(f'PUT {rel}: HTTP {status} {data[:200]!r}')

    def delete(self, rel):
        status, _ = self.call('DELETE', rel)
        if status not in (204, 404):
            raise DavError(f'DELETE {rel}: HTTP {status}')

    def move(self, src, dst):
        dest = f'{self.base.scheme}://{self.base.netloc}{self.href(dst)}'
        status, data = self.call('MOVE', src, headers={'Destination': dest, 'Overwrite': 'F'})
        if status not in (201, 204):
            raise DavError(f'MOVE {src} -> {dst}: HTTP {status} {data[:200]!r}')

    def propfind(self, rel, depth):
        """Liste von (rel, props) oder None, wenn der Pfad fehlt."""
        status, data = self.call('PROPFIND', rel, body=PROPFIND_BODY,
                                 headers={'Depth': str(depth), 'Content-Type': 'application/xml'})
        if status == 404:
            return None
        if status != 207:
            raise DavError(f'PROPFIND {rel}: HTTP {status}')
        # Manche Kernstände hängen bei fehlenden Knoten ein Fehlerdokument an die
        # gestreamte Multistatus-Antwort an – alles danach abschneiden.
        end = data.find(b'</d:multistatus>')
        if end >= 0:
            data = data[:end + len(b'</d:multistatus>')]
        try:
            doc = ET.fromstring(data)
        except ET.ParseError as exc:
            raise DavError(f'PROPFIND {rel}: kein gültiges XML ({exc})') from exc
        entries = []
        for resp in doc.findall(f'{DAV}response'):
            props = {}
            for propstat in resp.findall(f'{DAV}propstat'):
                if ' 200 ' not in (propstat.findtext(f'{DAV}status') or ''):
                    continue
                prop = propstat.find(f'{DAV}prop')
                if prop is None:
                    continue
                rtype = prop.find(f'{DAV}resourcetype')
                if rtype is not None:
                    props['dir'] = rtype.find(f'{DAV}collection') is not None
                for tag, key in ((f'{DAV}getcontentlength', 'size'), (f'{DAV}getetag', 'etag'),
                                 (f'{OC}fileid', 'fileid')):
                    text = prop.findtext(tag)
                    if text is not None:
                        props[key] = text
            entries.append((self.rel_of(resp.findtext(f'{DAV}href') or ''), props))
        return entries

    def stat(self, rel):
        want = rel.strip('/')
        for path, props in self.propfind(rel, 0) or []:
            if path == want:
                return {'exists': True, 'type': 'dir' if props.get('dir') else 'file',
                        'size': int(props.get('size') or 0), 'fileid': props.get('fileid'),
                        'etag': props.get('etag')}
        return {'exists': False}

    def sha(self, rel):
        digest = hashlib.sha256()
        status, _ = self.call('GET', rel, sink=digest.update)
        if status == 404:
            return 'MISSING'
        if status != 200:
            raise DavError(f'GET {rel}: HTTP {status}')
        return digest.hexdigest()

    def manifest(self, rel, skip_conflicts):
        base = rel.strip('/')
        lines, pending = [], [base]
        while pending:
            current = pending.pop()
            entries = self.propfind(current, 1)
            if entries is None:
                raise DavError(f'{current or "/"} fehlt auf dem Server')
            for path, props in entries:
                if path == current:
                    continue
                sub = path[len(base):].lstrip('/')
                if props.get('dir'):
                    lines.append(f'{sub}/\tDIR')
                    pending.append(path)
                elif not (skip_conflicts and CONFLICT_RE.search(path.rpartition('/')[2])):
                    lines.append(f'{sub}\t{self.sha(path)}')
        return sorted(lines)


def sha_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as fh:
        for block in iter(lambda: fh.read(BLOCK), b''):
            digest.update(block)
    return digest.hexdigest()


def local_manifest(root, skip_conflicts):
    if not os.path.isdir(root):
        raise DavError(f'{root} ist kein Ordner')
    lines = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, '/')
        prefix = '' if rel_dir == '.' else rel_dir + '/'
        lines.extend(f'{prefix}{d}/\tDIR' for d in dirnames)
        for name in filenames:
            full = os.path.join(dirpath, name)
            if CLIENT_FILES_RE.match(name) or os.path.islink(full):
                continue
            if skip_conflicts and CONFLICT_RE.search(name):
                continue
            lines.append(f'{prefix}{name}\t{sha_file(full)}')
    return sorted(lines)


def http_log(path):
    """Wertet die REQUEST-/RESPONSE-Zeilen des Client-HTTP-Loggers (sync.httplogger) aus."""
    line_re = re.compile(r'\b(REQUEST|RESPONSE) \S+ (\{.*\})\s*$')
    out = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            match = line_re.search(line)
            if not match:
                continue
            try:
                doc = json.loads(match.group(2))
            except ValueError:
                continue
            kind = 'request' if match.group(1) == 'REQUEST' else 'response'
            info = (doc.get(kind) or {}).get('info') or {}
            url_path = urllib.parse.unquote(urllib.parse.urlsplit(str(info.get('url', ''))).path)
            if kind == 'request':
                out.append(f'REQ {info.get("method")} {url_path}')
            else:
                out.append(f'RES {info.get("method")} {(info.get("reply") or {}).get("status")} {url_path}')
    return out


def main(argv):
    if not argv:
        raise DavError('Unterbefehl fehlt (siehe Kopf von davtool.py)')
    cmd, args = argv[0], argv[1:]
    skip = '--skip-conflicts' in args
    args = [a for a in args if a != '--skip-conflicts']
    if cmd == 'localmanifest':
        print('\n'.join(local_manifest(args[0], skip)))
        return
    if cmd == 'httplog':
        print('\n'.join(http_log(args[0])))
        return
    srv = Server()
    if cmd == 'mkdirs':
        srv.mkdirs(args[0])
    elif cmd == 'put':
        srv.put(args[0], args[1])
    elif cmd == 'delete':
        srv.delete(args[0])
    elif cmd == 'move':
        srv.move(args[0], args[1])
    elif cmd == 'stat':
        info = srv.stat(args[0])
        if len(args) > 1:
            value = info.get(args[1])
            print('' if value is None else (str(value).lower() if isinstance(value, bool) else value))
        else:
            print(json.dumps(info))
    elif cmd == 'sha':
        print(srv.sha(args[0]))
    elif cmd == 'manifest':
        print('\n'.join(srv.manifest(args[0], skip)))
    else:
        raise DavError(f'unbekannter Unterbefehl {cmd}')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    try:
        main(sys.argv[1:])
    except (DavError, OSError, IndexError, http.client.HTTPException) as exc:
        print(f'davtool: {exc}', file=sys.stderr)
        sys.exit(2)

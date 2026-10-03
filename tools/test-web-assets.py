"""Validate offline dashboard assets, markup and release metadata without a board."""
import gzip
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import runpy
from urllib.parse import urlsplit, unquote

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'data'


class Markup(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.links = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if 'id' in attrs:
            assert attrs['id'] not in self.ids, f'Duplicate HTML id: {attrs["id"]}'
            self.ids.add(attrs['id'])
        if tag in ('script', 'link', 'img'):
            value = attrs.get('src') or attrs.get('href')
            if value:
                self.links.append(value)


def require_local_asset(value, parent=DATA):
    parts = urlsplit(value)
    assert not parts.scheme and not parts.netloc, f'Offline page has remote asset: {value}'
    path = ((DATA / unquote(parts.path).lstrip('/')) if parts.path.startswith('/') else (parent / unquote(parts.path))).resolve()
    assert path.is_relative_to(DATA.resolve()) and path.is_file(), f'Missing asset: {value}'


def main():
    html = (DATA / 'index.html').read_text(encoding='utf-8')
    markup = Markup()
    markup.feed(html)
    for value in markup.links:
        require_local_asset(value)
    css = (DATA / 'css/style.css').read_text(encoding='utf-8')
    for value in re.findall(r'url\([\x22\x27]?([^\x22\x27)]+)', css):
        require_local_asset(value, DATA / 'css')
    manifest = json.loads((DATA / 'manifest.json').read_text(encoding='utf-8'))
    for icon in manifest['icons']:
        require_local_asset(icon['src'])
    assets = runpy.run_path(str(ROOT / 'tools/build-web-assets.py'))['ASSETS']
    for name in assets:
        source = DATA / name
        assert gzip.decompress(source.with_name(source.name + '.gz').read_bytes()) == source.read_bytes(), f'Stale gzip: {name}'
    header = (ROOT / 'src/core/telemetry.h').read_text(encoding='utf-8')
    version = re.search(r'#define HYGROW_FIRMWARE_VERSION "([^\x22]+)"', header).group(1)
    assert set(re.findall(r'\?v=([^\x22]+)', html)) == {version}, 'Dashboard cache version differs from firmware'
    handoff = (ROOT / 'docs/APP_LOCAL_TELEMETRY_HANDOFF.md').read_text(encoding='utf-8')
    assert f'Firmware: **{version}**' in handoff, 'Handoff version differs from firmware'
    assert set(re.findall(r'"firmwareVersion": "([^\x22]+)"', handoff)) == {version}
    print(f'Offline asset links, unique HTML IDs, manifest/fonts, {len(assets)} gzip copies, and firmware {version} metadata passed.')


if __name__ == '__main__':
    main()

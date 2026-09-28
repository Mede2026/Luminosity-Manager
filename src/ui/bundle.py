# Assemble index.html + style.css + app.js en une seule page (integree dans le .exe).
import base64, pathlib, re, sys

here = pathlib.Path(__file__).parent
html = (here / 'index.html').read_text(encoding='utf-8')
css = (here / 'style.css').read_text(encoding='utf-8')
js = (here / 'app.js').read_text(encoding='utf-8')
# Polices : integrees directement dans la page (data URI), pour rester un seul fichier
css = re.sub(r'url\("fonts/([\w.-]+)"\)',
             lambda m: 'url("data:font/woff2;base64,' + base64.b64encode((here / 'fonts' / m.group(1)).read_bytes()).decode() + '")', css)
html = html.replace('<link rel="stylesheet" href="style.css">', '<style>\n' + css + '</style>')
html = html.replace('<script src="app.js"></script>', '<script>\n' + js + '</script>')
assert 'href="style.css"' not in html and 'src="app.js"' not in html
html = re.sub(r'\n\s*\n', '\n', html)
pathlib.Path(sys.argv[1]).write_text(html, encoding='utf-8')

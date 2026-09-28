# Assemble index.html + style.css + app.js en une seule page (integree dans le .exe).
import pathlib, re, sys

here = pathlib.Path(__file__).parent
html = (here / 'index.html').read_text(encoding='utf-8')
css = (here / 'style.css').read_text(encoding='utf-8')
js = (here / 'app.js').read_text(encoding='utf-8')
html = html.replace('<link rel="stylesheet" href="style.css">', '<style>\n' + css + '</style>')
html = html.replace('<script src="app.js"></script>', '<script>\n' + js + '</script>')
assert 'href="style.css"' not in html and 'src="app.js"' not in html
html = re.sub(r'\n\s*\n', '\n', html)
pathlib.Path(sys.argv[1]).write_text(html, encoding='utf-8')

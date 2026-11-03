#!/usr/bin/env python3
"""Integration tests for browsing-context resource loading in pr_shell."""

import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest
import zlib

from tools.suite.wpt import wpt


SHELL = wpt.SHELL


def box(kind, payload):
    return struct.pack(">I", 8 + len(payload)) + kind + payload


def mp4(seconds, width, height):
    """Returns a movie of `seconds` whose only track is a video of `width` by `height`, as its boxes declare."""
    header = bytes(4) + bytes(8) + struct.pack(">II", 1000, int(seconds * 1000)) + bytes(80)
    track = bytes(4) + bytes(72) + struct.pack(">II", width << 16, height << 16)
    handler = bytes(8) + b"vide" + bytes(12)
    trak = box(b"trak", box(b"tkhd", track) + box(b"mdia", box(b"hdlr", handler)))
    return box(b"ftyp", b"isom" + bytes(4)) + box(b"moov", box(b"mvhd", header) + trak)


def png(width, height):
    """Returns a PNG of `width` by `height` opaque gray pixels."""
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    rows = b"".join(b"\x00" + b"\x80\x80\x80" * width for _ in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) +
        chunk(b"IEND", b""))


def url(path):
    """Returns the file: URL that the shell loads the page at `path` at, its path made absolute as the shell does."""
    return Path(os.path.abspath(path)).as_uri()


class HtmlHostTests(unittest.TestCase):
    def run_page(self, root, source, *options):
        page = root / "pages" / "test.html"
        page.parent.mkdir(exist_ok=True)
        page.write_text(source)
        return subprocess.run([SHELL, *options, "--html", str(page)], capture_output=True, encoding="utf-8", timeout=10)

    def test_a_local_page_loads_at_its_file_url_and_resolves_encoded_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixtures = root / "fixtures"
            fixtures.mkdir()
            (fixtures / "a #%.js").write_text("print('external', document.URL, document.baseURI);")
            result = self.run_page(root,
                "<base href='../fixtures/'><base href='../ignored/'><script "
                "src='./a%20%23%25.js?cache=1#fragment'></script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), [
                "external %s %s/" % (url(root / "pages" / "test.html"), url(fixtures))])

    def test_a_resource_that_does_not_load_is_a_network_error_and_the_run_goes_on(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory), "<script src='missing.js?cache=1#fragment'></script>"
                                                    "<script src='https://elsewhere.test/script.js'></script>"
                                                    "<script>print('parsing goes on');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "parsing goes on\n")

    def test_a_page_of_the_wpt_checkout_loads_from_wpts_server_over_the_network(self):
        with tempfile.TemporaryDirectory(dir=wpt.WPT) as directory:
            root = Path(directory)
            base = "/" + os.path.relpath(directory, wpt.WPT).replace(os.sep, "/")
            (root / "fixtures").mkdir()
            (root / "fixtures" / "values.sub.js").write_text("print('{{GET[name]}}', '{{host}}');")
            page = root / "pages" / "test.html"
            page.parent.mkdir()
            page.write_text("<script>print(location.hostname);</script>"
                            "<script src='" + base + "/fixtures/values.sub.js?name=value'></script>")
            result = subprocess.run([SHELL, *wpt.network_options(), "--html", wpt.url_of(str(page))],
                                    capture_output=True, encoding="utf-8", timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["web-platform.test", "value web-platform.test"])

    def test_inline_syntax_error_keeps_html_line_and_parsing_continues(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                                   "<!doctype html>\n<script>\nlet = ;\n</script>\n"
                                                   "<script>print('continued');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "continued\n")
            self.assertIn("SyntaxError", result.stderr)
            self.assertIn(url(Path(directory) / "pages" / "test.html") + ":3:", result.stderr)

    def test_an_error_listener_that_throws_is_reported_and_not_dispatched_again(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                                   "<!doctype html><script>let calls = 0;\n"
                                   "addEventListener('error', () => { ++calls; throw new Error('listener'); });\n"
                                   "setTimeout(() => print('calls', calls));\n"
                                   "throw new Error('script');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "calls 1\n")
            self.assertIn("Error: listener", result.stderr)
            self.assertIn("Error: script", result.stderr)

    def test_insert_before_reads_an_undefined_child_as_null(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                                   "<!doctype html><body><p></p><script>const added = document.createElement('i');\n"
                                   "document.body.insertBefore(added, undefined);\n"
                                   "print(document.body.lastChild === added);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "true\n")

    def test_a_run_that_times_out_still_writes_its_screenshot(self):
        with tempfile.TemporaryDirectory() as directory:
            screenshot = Path(directory) / "page.png"
            # A server that never answers holds the page's load open.
            with socket.create_server(("127.0.0.1", 0)) as server:
                port = server.getsockname()[1]
                result = self.run_page(Path(directory),
                                       f"<!doctype html><script src='http://127.0.0.1:{port}/'></script>",
                                       "--network", "--timeout", "200", "--screenshot", str(screenshot))
            self.assertEqual(result.returncode, 124, result.stderr)
            self.assertTrue(screenshot.exists())

    def test_a_style_property_setter_runs_the_reactions_it_queues_before_it_returns(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                                   "<!doctype html><x-a id=x style='color: red'></x-a><script>const seen = [];\n"
                                   "customElements.define('x-a', class extends HTMLElement {\n"
                                   "  static get observedAttributes() { return ['style']; }\n"
                                   "  attributeChangedCallback(n, old, value) {\n"
                                   "    if (seen.length > 8) return;\n"
                                   "    seen.push(value);\n"
                                   "    if (old === null) this.style.opacity = '0.5';\n"
                                   "    if (old !== value) this.style = value;\n"
                                   "  }\n"
                                   "});\n"
                                   "print(seen.length);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "5\n")

    def test_template_contents_are_inert_and_their_observers_still_run(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                                   "<!doctype html><template id=t><x-e a=1></x-e></template><script>\n"
                                   "customElements.define('x-e', class extends HTMLElement {\n"
                                   "constructor() { super(); print('constructed'); }});\n"
                                   "const c = t.content;\n"
                                   "print(c.ownerDocument !== document, c.ownerDocument.defaultView);\n"
                                   "c.cloneNode(true);\n"
                                   "new MutationObserver(r => print('records', r.length)).observe(c, {subtree: true, attributes: true});\n"
                                   "c.firstChild.setAttribute('a', '2');\n"
                                   "document.importNode(c, true);\n"
                                   "</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["true null", "constructed", "records 1"])

    def test_modules_defer_resolve_imports_and_share_external_instances(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            modules = root / "modules"
            modules.mkdir()
            (modules / "dep #.js").write_text("export const value = 42; print('dependency');")
            (modules / "entry.js").write_text(
                "import {value} from './dep%20%23.js?version=1';print('entry', value, "
                "import.meta.url); export {value};")
            result = self.run_page(root,
                "<base href='../modules/'><script type='module'>import {value} from "
                "'./entry.js';print('inline', value, document.getElementById('later').id, "
                "import.meta.url);</script><script nomodule>throw new Error('nomodule "
                "executed');</script><script>print('classic');</script><div "
                "id='later'></div><script type='module' src='entry.js'></script><script "
                "type='module'>print('second inline');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), ["classic", "dependency",
                "entry 42 " + url(modules / "entry.js"),
                "inline 42 later " + url(modules) + "/", "second inline"])

    def test_module_cycles_and_dynamic_import(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.js").write_text("import {b} from './b.js'; export function a() { return 20; "
                                       "}export const value = b(); print('a');")
            (root / "b.js").write_text("import {a} from './a.js'; export function b() { return a() "
                                       "+ 22; }")
            result = self.run_page(root, "<script type='module'>"
                "const first = await import('../a.js'); const second = await "
                "import('../a.js');print(first.value, first === second);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), ["a", "42 true"])

    def test_top_level_await_allows_later_modules_and_timers_before_load(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script>let resolve; const ready = new Promise(r => resolve = "
                "r);window.addEventListener('load', () => print('load'));</script><script "
                "type='module'>await ready; print('resumed');await new Promise(r => setTimeout(r, "
                "0)); print('timer');</script><script type='module'>print('resolver'); "
                "resolve();</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), ["resolver", "resumed", "timer", "load"])

    def test_module_errors_report_and_do_not_skip_later_modules(self):
        # Neither a resource that does not load nor an author error fails the run.
        cases = [("import 'bare-name';", "Failed to resolve module specifier 'bare-name'", 0),
                 ("export const broken = ;", "SyntaxError", 0),
                 ("await Promise.reject(new Error('rejected module'));", "rejected module", 0)]
        for script, diagnostic, returncode in cases:
            with self.subTest(script=script), tempfile.TemporaryDirectory() as directory:
                result = self.run_page(Path(directory),
                    "<script>window.addEventListener('error', e => print('error', "
                    "e.type));</script><script type='module'>" + script + "</script>"
                    "<script type='module'>print('continued');</script>")
                self.assertEqual(result.returncode, returncode, result.stderr)
                self.assertIn(diagnostic, result.stderr)
                self.assertIn("continued", result.stdout.splitlines())
                self.assertIn("error error", result.stdout.splitlines())

    def test_module_that_does_not_load_fires_error_at_its_script(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "a.js").write_text("import './missing.js'; print('a');")
            result = self.run_page(root,
                "<script type='module' onerror=\"print('missing')\">import '../missing.js';</script>"
                "<script type='module' src='../a.js' onerror=\"print('a missing')\"></script>"
                "<script type='module'>print('continued');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["missing", "a missing", "continued"])

    def test_unsettled_module_cannot_report_a_successful_load(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script>window.addEventListener('load', () => print('load'));</script><script "
                "type='module'>await new Promise(() => {});</script>")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertIn("module evaluation is pending", result.stderr)

    def test_module_maps_are_per_document(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = []
            for name in ("first", "second"):
                folder = root / name
                folder.mkdir()
                (folder / "entry.js").write_text("print('" + name + "');")
                page = folder / "page.html"
                page.write_text("<script type='module' src='entry.js'></script>")
                arguments += ["--html", str(page)]
            result = subprocess.run([SHELL, *arguments], capture_output=True, encoding="utf-8", timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), ["first", "second"])

    def test_async_module_can_execute_before_parsing_finishes(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script type='module' async>print(document.getElementById('later') === "
                "null);</script><div id='later'></div><script "
                "type='module'>print(document.getElementById('later').id);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), ["true", "later"])

    def test_deferred_classic_and_module_scripts_keep_document_order(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "deferred.js").write_text("print('deferred', "
                                              "document.getElementById('later').id);")
            result = self.run_page(root,
                "<script defer src='../deferred.js'></script><script "
                "type='module'>print('module');</script><script defer>print('inline "
                "classic');</script><div id='later'></div>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(),
                             ["inline classic", "deferred later", "module"])

    def test_inline_module_identity_cannot_shadow_a_resource_url(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script type='module'>try { await import(document.URL + '#inline-module-1'); "
                "print('unexpected'); }catch (error) { print(error.name); }</script>")
            # The import fetches the document, which is not JavaScript.
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout, "TypeError\n")


    def test_rejections_no_handler_took_are_notified_after_the_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script>addEventListener('unhandledrejection', e => {  print('unhandled', "
                "e.reason.message); if (e.reason.message === 'kept') e.preventDefault(); "
                "});Promise.reject(new Error('kept'));Promise.reject(new "
                "Error('handled')).catch(() => print('caught'));Promise.reject(new "
                "Error('reported'));</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["caught", "unhandled kept", "unhandled reported"])
            self.assertIn("reported", result.stderr)
            self.assertNotIn("kept", result.stderr)

    def test_event_handler_content_attributes_compile_in_their_element_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<body onload=\"print('load', this === window)\"><div id=d onclick=\"print(id, "
                "event.type); return false\"></div><script>const d = "
                "document.getElementById('d');print(d.dispatchEvent(new Event('click', "
                "{cancelable: true})), typeof d.onclick);d.removeAttribute('onclick'); "
                "print(d.onclick);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["d click", "false function", "null", "load true"])

    def test_iframes_run_scripts_in_their_own_windows_and_load_before_the_page(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "child.html").write_text(
                "<p id=p>child</p><script>print('child', location.href, parent !== window, top === "
                "parent)</script>")
            result = self.run_page(root,
                "<iframe id=f src='child.html'></iframe><iframe id=g srcdoc='<b>x</b>'></iframe>"
                "<script>const f = document.getElementById('f');"
                "f.onload = () => print('frame load', f.contentDocument.getElementById('p').textContent, "
                "f.contentWindow.parent === window);"
                "onload = () => { const g = document.getElementById('g'); g.onload = () => "
                "print('removed load');"
                "  g.remove(); print('page load', g.contentWindow); };</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "child %s true true" % url(root / "pages" / "child.html"), "frame load child true",
                "page load null"])

    def test_images_load_in_tasks_before_the_page_and_a_new_source_supersedes_the_old(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            # A PNG header of 3 by 2 pixels, which is all that the engine reads of the file. The image without a source
            # fails in a task before the fetched images arrive.
            (root / "pages" / "a.png").write_bytes(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x03\0\0\0\x02")
            result = self.run_page(root,
                "<img src=a.png onload=\"print('load', naturalWidth, naturalHeight, "
                "complete)\"><img src=missing.png onerror=\"print('error', "
                "currentSrc)\"><script>const i = new Image(); i.src = 'a.png'; i.onload = () => "
                "print('superseded');i.src = ''; i.onerror = () => print('empty');onload = () => "
                "print('page load');</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "empty", "load 3 2 true", "error " + url(root / "pages") + "/missing.png",
                "page load"])

    def test_an_input_that_becomes_an_image_button_keeps_a_box_that_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Once a text field's box is rendered, it stays an element's box when the input changes to the image state.
            result = self.run_page(root,
                "<body><script>const b = document.createElement('input'); document.body.appendChild(b);"
                "requestAnimationFrame(() => requestAnimationFrame(() => { b.type = 'image';"
                "requestAnimationFrame(() => requestAnimationFrame(() => print('rendered'))); }));</script>",
                "--verify", "--virtual-time")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["rendered"])
            self.assertNotIn("render verify", result.stderr)

    def test_focus_moves_with_events_retargeted_out_of_shadow_trees(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<input id=a><div id=host></div><input id=b onblur=\"c.focus()\"><input "
                "id=c><script>for (const type of ['focus', 'blur']) addEventListener(type, e => {  "
                "if (e.target !== window) print(type, e.target.id, e.relatedTarget && "
                "e.relatedTarget.id); }, true);const shadow = host.attachShadow({mode: 'open', "
                "delegatesFocus: true});shadow.innerHTML = '<input id=inner>';a.focus(); "
                "host.focus();print(document.activeElement.id, shadow.activeElement.id, "
                "host.matches(':focus'));b.focus(); a.focus(); "
                "print(document.activeElement.id);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "focus a null", "blur a host", "focus host a", "host inner true",
                "blur host b", "focus b host", "blur b a", "focus c null", "c"])

    def test_xml_pages_parse_as_xml_with_html_and_svg_scripts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            page = root / "pages" / "test.svg"
            page.parent.mkdir()
            page.write_text(
                "<svg xmlns='http://www.w3.org/2000/svg' "
                "xmlns:h='http://www.w3.org/1999/xhtml'><h:script>print(document.contentType, "
                "document.documentElement instanceof "
                "SVGElement)</h:script><script><![CDATA[print(1 < 2 && "
                "document.createElement('DIV').localName)]]></script></svg>")
            result = subprocess.run([SHELL, "--html", str(page)], capture_output=True, encoding="utf-8", timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["image/svg+xml true", "DIV"])

            page.write_text("<svg><a></svg>")
            result = subprocess.run([SHELL, "--html", str(page)], capture_output=True, encoding="utf-8", timeout=10)
            self.assertEqual(result.returncode, 1)
            self.assertIn("an end tag does not match the element it closes", result.stderr)

    def test_post_message_crosses_windows_with_the_incumbent_window_as_its_source(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<iframe id=f srcdoc=\"<script>onmessage = e => { print('child', e.data.x, "
                "e.source === parent);  e.source.postMessage([e.data.x + 1], '*'); "
                "};</script>\"></iframe><script>onmessage = e => print('parent', e.data[0], "
                "e.source === f.contentWindow);onload = () => { f.contentWindow.postMessage({x: "
                "1}, '*');  f.contentWindow.postMessage({x: 9}, 'http://elsewhere.test');  try { "
                "postMessage(() => 0, '*'); } catch (e) { print(e.name); } };</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["DataCloneError", "child 1 true", "parent 2 true"])

    def test_document_write_parses_where_the_parser_is_or_into_a_document_it_opens(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script>document.write('<p id=a>inline</p>')</script><iframe id=f></iframe>"
                "<script>const d = f.contentWindow.document; "
                "print(document.getElementById('a').textContent, d.open() === d);"
                "d.write('<di'); d.write('v "
                "id=x>text</div><script>print(document.getElementById(\\'x\\').textContent)<\\/script>');"
                "d.close(); print(d.body.innerHTML.startsWith('<div id=\"x\">text</div>'));"
                "try { document.implementation.createDocument(null, 'x').write('y'); } catch (e) { "
                "print(e.name); }"
                "onload = () => { document.write('<p>late</p>'); document.close(); "
                "print(document.body.innerHTML); };</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["inline true", "text", "true", "InvalidStateError", "<p>late</p>"])

    def test_document_timelines_follow_rendering_updates_from_the_window_time_origin(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<script>const start = document.timeline.currentTime;print(performance instanceof "
                "EventTarget, start >= 0, document.timeline === "
                "document.timeline);requestAnimationFrame(() => requestAnimationFrame(time => {  "
                "print(document.timeline.currentTime > start, document.timeline.currentTime === "
                "time,        time <= performance.now(), new DocumentTimeline({originTime: "
                "5}).currentTime === time - 5,        new DOMParser().parseFromString('', "
                "'text/html').timeline.currentTime); }));</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["true true true", "true true true true null"])

    def test_slots_assign_by_name_or_by_hand_and_signal_their_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<div id=host><span slot=a id=sa>A</span><b id=d>B</b></div><script>"
                "const s = host.attachShadow({mode: 'open'}); s.innerHTML = '<slot "
                "name=a></slot><slot></slot>';" "const [a, rest] = s.querySelectorAll('slot');"
                "s.addEventListener('slotchange', e => print('slotchange', e.target.name || "
                "'default'));" "print(a.assignedNodes()[0] === sa, d.assignedSlot === rest, "
                "a.matches(':has-slotted'));"
                "d.addEventListener('click', e => print(e.composedPath().includes(rest)));"
                "d.dispatchEvent(new Event('click', {composed: true}));"
                "sa.slot = 'b'; print(a.assignedNodes().length, a.matches(':has-slotted'));"
                "const m = document.createElement('div'); m.innerHTML = '<p>1</p>';"
                "const ms = m.attachShadow({mode: 'open', slotAssignment: 'manual'}); ms.innerHTML "
                "= '<slot></slot>';"
                "print(ms.firstChild.assignedNodes().length); ms.firstChild.assign(m.firstChild);"
                "print(ms.firstChild.assignedNodes().length);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "true true true", "true", "0 false", "0", "1", "slotchange a",
                "slotchange default"])

    def test_custom_elements_upgrade_construct_and_react_to_their_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_page(Path(directory),
                "<my-el id=a foo=1></my-el><script>const log = "
                "[];print(a.matches(':defined'));class MyEl extends HTMLElement {  static "
                "observedAttributes = ['foo'];  constructor() { super(); log.push('ctor'); }  "
                "connectedCallback() { log.push('connected'); this.internals = "
                "this.attachInternals(); }  disconnectedCallback() { log.push('disconnected'); }  "
                "attributeChangedCallback(n, o, v) { log.push(n + ' ' + o + ' ' + v); } "
                "}customElements.whenDefined('my-el').then(c => print('defined', c === "
                "MyEl));customElements.define('my-el', MyEl);print(log.join(), a instanceof MyEl, "
                "a.matches(':defined'));a.setAttribute('foo', '2'); "
                "a.internals.states.add('on');print(log.at(-1), a.matches(':state(on)'));const b = "
                "document.createElement('my-el'); document.body.append(b); b.remove();print(b "
                "instanceof MyEl, log.slice(-3).join());try { new HTMLElement(); } catch (e) { "
                "print(e.name); }const d = document.createElement('div'); d.innerHTML = "
                "'<my-el></my-el>';print(d.firstChild instanceof MyEl, "
                "customElements.getName(MyEl));</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "false", "ctor,foo null 1,connected true true", "foo 1 2 true",
                "true ctor,connected,disconnected", "TypeError", "true my-el", "defined true"])

    def test_media_elements_load_their_metadata_and_play_their_timeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "clip.mp4").write_bytes(mp4(2, 320, 240))
            result = self.run_page(root,
                "<video id=v muted></video><script>const log = [];for (const e of "
                "['loadedmetadata', 'canplay', 'play', 'playing', 'seeking', 'seeked', 'pause', "
                "'error'])  v.addEventListener(e, () => log.push(e));print(v.paused, v.muted, "
                "v.matches(':paused'), v.matches(':muted'), "
                "v.canPlayType('video/mp4'));v.addEventListener('canplay', async () => {  "
                "print(v.duration, v.videoWidth, v.videoHeight, v.readyState, v.seekable.end(0));  "
                "await v.play(); print(v.matches(':playing'), v.paused);  "
                "v.addEventListener('seeking', () => print(v.matches(':seeking')), {once: true});  "
                "v.addEventListener('seeked', () => { v.pause(); print(v.currentTime.toFixed(2), "
                "log.join()); }, {once: true});  v.currentTime = 1.5; }, {once: true});v.src = "
                "'clip.mp4';const bad = document.createElement('audio'); "
                "document.body.append(bad); bad.src = 'missing.mp4';bad.play().catch(e => "
                "print(e.name, bad.error.code));</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                "true true true true maybe", "2 320 240 4 2", "NotSupportedError 4", "true false",
                "true", "1.50 loadedmetadata,canplay,play,playing,seeking,seeked"])

    def test_a_window_that_its_frame_navigated_away_from_keeps_its_document_and_navigates_nowhere(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "a.html").write_text("<p>a</p><script>setTimeout(() => location.href = 'b.html');</script>")
            (root / "pages" / "b.html").write_text("<p>b</p>")
            result = self.run_page(root,
                "<iframe id=f src=a.html></iframe><script>f.addEventListener('load', () => { const w = f.contentWindow;"
                "const held = w.location; f.addEventListener('load', () => { held.hash = 'x';"
                "print(held.href.split('/').pop(), w.document.querySelector('p').textContent,"
                "f.contentDocument.body.textContent, f.contentWindow.location.href.split('/').pop());"
                "held.reload(); setTimeout(() => print(f.contentDocument.body.textContent), 20); },"
                "{once: true}); }, {once: true});</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["a.html a b b.html", "b"])

    def test_a_video_is_replaced_at_its_resource_size_once_its_metadata_loads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "clip.mp4").write_bytes(mp4(2, 320, 240))
            result = self.run_page(root,
                "<video id=v muted></video><script>const size = () => { const r = v.getBoundingClientRect(); "
                "return r.width + 'x' + r.height; };print(size(), document.visibilityState, document.hidden);"
                "v.addEventListener('loadedmetadata', () => print(size(), v.videoWidth, v.videoHeight));"
                "v.src = 'clip.mp4';</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["300x150 visible false", "320x240 320 240"])

    def test_a_video_without_data_takes_its_poster_frame_size_and_its_metadata_size_over_it(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "poster.png").write_bytes(png(40, 30))
            (root / "pages" / "clip.mp4").write_bytes(mp4(2, 320, 240))
            result = self.run_page(root,
                "<video id=v poster=poster.png></video><script>const size = () => { const r = "
                "v.getBoundingClientRect(); return r.width + 'x' + r.height; };"
                "const wait = () => size() === '300x150' ? requestAnimationFrame(wait) : shown();"
                "const shown = () => { print(size()); v.addEventListener('loadedmetadata', () => print(size()));"
                "v.src = 'clip.mp4'; };requestAnimationFrame(wait);</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), ["40x30", "320x240"])

    def test_a_page_settles_while_media_plays_in_a_loop_and_waits_for_media_that_ends(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages").mkdir()
            (root / "pages" / "clip.mp4").write_bytes(mp4(0.5, 320, 240))
            result = self.run_page(root,
                "<video id=looping autoplay loop muted src=clip.mp4></video>"
                "<video id=once autoplay muted src=clip.mp4></video><script>"
                "looping.addEventListener('playing', () => print('looping plays'), {once: true});"
                "once.addEventListener('ended', () => print('once ended', looping.paused));</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted(result.stdout.splitlines()), ["looping plays", "once ended false"])

    def test_fetch_reads_a_resource_against_the_base_url_and_reads_its_body_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data").mkdir()
            (root / "data" / "mod.json").write_text('﻿{"name": "base", "items": [1, 2, 3]}', encoding="utf-8")
            (root / "data" / "bytes.bin").write_bytes(bytes([0, 1, 254, 255]))
            result = self.run_page(root,
                "<base href='../data/'><script>(async () => {"
                "const response = await fetch('mod.json#part');"
                "print(response.ok, response.status, response.type, response.url === document.baseURI + 'mod.json',"
                "      response.headers.get('Content-Type'), response.bodyUsed);"
                "const value = await response.json();"
                "print(value.name, value.items.length, response.bodyUsed);"
                "await response.text().catch(e => print('read twice', e.name));"
                "const bytes = new Uint8Array(await (await fetch('bytes.bin')).arrayBuffer());"
                "print(bytes.join());"
                "print(await (await fetch('data:text/plain;charset=utf-8,%E2%9C%93')).text());"
                "await fetch('missing.json').catch(e => print('missing', e.name));"
                "await fetch('http://[bad').catch(e => print('invalid URL', e.name));"
                "await fetch('https://user:pass@example.test/').catch(e => print('credentials', e.name));"
                "await fetch('mod.json', {body: 'x'}).catch(e => print('GET with a body', e.name));"
                "await fetch('mod.json', {method: 'TRACE'}).catch(e => print('forbidden method', e.name));"
                "await fetch('mod.json', {mode: 'navigate'}).catch(e => print('navigate', e.name));"
                "await fetch('mod.json', {mode: 'no-cors', method: 'PUT'}).catch(e => print('no-cors PUT', e.name));"
                "await fetch().catch(e => print('no arguments', e.name));"
                "})();</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), [
                "true 200 basic true application/json false", "base 3 true", "read twice TypeError",
                "0,1,254,255", "✓", "missing TypeError", "invalid URL TypeError", "credentials TypeError",
                "GET with a body TypeError", "forbidden method TypeError", "navigate TypeError",
                "no-cors PUT TypeError", "no arguments TypeError"])

    def test_headers_and_responses_follow_their_guards_and_bodies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pages" / "a.txt").parent.mkdir()
            (root / "pages" / "a.txt").write_text("text")
            result = self.run_page(root, "<script>(async () => {"
                "const headers = new Headers([['B', ' 2 '], ['a', '1']]);"
                "headers.append('b', '3'); headers.append('Set-Cookie', 'x=1'); headers.append('set-cookie', 'y=2');"
                "print([...headers].map(pair => pair.join('=')).join(' '), headers.get('B'));"
                "print(headers.getSetCookie().join(), headers.has('A'), [...headers.keys()].length);"
                "headers.set('b', '4'); headers.delete('A');"
                "print([...headers.entries()].join(' '));"
                "try { headers.append('bad name', 'x'); } catch (e) { print('name', e.name); }"
                "try { headers.get('a\\u0100'); } catch (e) { print('ByteString', e.name); }"
                "const response = await fetch('a.txt');"
                "try { response.headers.set('x', 'y'); } catch (e) { print('immutable', e.name); }"
                "const made = new Response(new Uint8Array([104, 105]), {status: 299, statusText: 'Fine',"
                "                                                      headers: {'Set-Cookie': 'z', 'X-A': 'b'}});"
                "print(made.type, made.status, made.ok, made.statusText, made.url === '', made.headers.has('set-cookie'),"
                "      made.headers.get('content-type'));"
                "const copy = made.clone();"
                "print(await made.text(), await copy.text(), made.bodyUsed);"
                "try { made.clone(); } catch (e) { print('clone a used body', e.name); }"
                "print(await new Response(new URLSearchParams('a=1&b=2')).text(),"
                "      new Response(new URLSearchParams()).headers.get('content-type'));"
                "print(await new Response().text() === '', new Response(null, {status: 204}).bodyUsed);"
                "for (const init of [{status: 199}, {status: 600}, {status: 200, statusText: 'a\\nb'}])"
                "    try { new Response('', init); } catch (e) { print(e.name); }"
                "try { new Response('x', {status: 304}); } catch (e) { print('null body status', e.name); }"
                "const error = Response.error();"
                "print(error.type, error.status, (await Response.json({a: [1]}, {status: 201})).status);"
                "await new Response('{').json().catch(e => print('bad JSON', e.name));"
                "print('body' in made);"
                "})();</script>")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, "")
            self.assertEqual(result.stdout.splitlines(), [
                "a=1 b=2, 3 set-cookie=x=1 set-cookie=y=2 2, 3", "x=1,y=2 true 4", "b,4 set-cookie,x=1 set-cookie,y=2",
                "name TypeError", "ByteString TypeError", "immutable TypeError",
                "default 299 true Fine true false null", "hi hi true", "clone a used body TypeError",
                "a=1&b=2 application/x-www-form-urlencoded;charset=UTF-8", "true false", "RangeError", "RangeError",
                "TypeError", "null body status TypeError", "error 0 201", "bad JSON SyntaxError", "false"])


if __name__ == "__main__":
    unittest.main()

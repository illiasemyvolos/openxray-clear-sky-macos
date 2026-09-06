#!/usr/bin/env python3
"""Generate no-op stubs for the five interfaces a renderer module must install.

RendererModule::SetupEnv installs IRender, CDUInterface, IUIRender,
IRenderFactory and IDebugRender. Together they are a few hundred pure virtual
methods, which is boilerplate rather than design: typing them by hand produces
an unreviewable diff and goes stale the moment upstream touches an interface.

This reads the headers and emits one stub class per interface whose every
method logs the first time it is called and returns a value-initialized result.
Run it again after an upstream merge; the generated file is committed so that
building does not require Python.

    python3 tools/generate_render_stubs.py

The parser is deliberately small. It understands the shapes these five headers
actually use - including the RENDER_FACTORY_INTERFACE macro, which hides
IRenderFactory's methods from any plain text search - and nothing more. The
compiler is the real check: a missed method leaves the stub class abstract and
the build fails loudly rather than silently.
"""

import os
import re
import sys

ENGINE_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = "src/Layers/xrRenderPC_VK/vk_stubs_generated.h"

# header, class, the pointer GEnv holds it in
INTERFACES = [
    ("src/xrEngine/Render.h",                 "IRender",         "Render"),
    ("src/Include/xrRender/DrawUtils.h",      "CDUInterface",    "DU"),
    ("src/Include/xrRender/UIRender.h",       "IUIRender",       "UIRender"),
    ("src/Include/xrRender/RenderFactory.h",  "IRenderFactory",  "RenderFactory"),
    ("src/Include/xrRender/DebugRender.h",    "IDebugRender",    "DRender"),
]


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def expand_factory_macro(text):
    """RENDER_FACTORY_INTERFACE(X) declares two pure virtuals. Nothing that
    reads the header as text can see them until the macro is expanded."""
    def repl(m):
        cls = m.group(1)
        return ("virtual I{0}* Create{0}() = 0; "
                "virtual void Destroy{0}(I{0}* pObject) = 0;").format(cls)
    return re.sub(r'RENDER_FACTORY_INTERFACE\s*\(\s*(\w+)\s*\)', repl, text)


def drop_inline_bodies(body):
    """Replace every brace-enclosed region with ';'.

    The class bodies mix pure virtuals with inline helpers and nested enums.
    An inline body such as `{ return GetGeneration() >= GENERATION_R2; }`
    contains its own ';', so splitting the raw text into statements would cut
    through it and leave the next declaration prefixed with a stray '}' - which
    silently dropped three methods until the compiler pointed them out.
    """
    out = []
    depth = 0
    for ch in body:
        if ch == '{':
            depth += 1
            continue
        if ch == '}':
            depth -= 1
            if depth == 0:
                out.append(';')
            continue
        if depth == 0:
            out.append(ch)
    return ''.join(out)


def class_body(text, name):
    """Return the body of `class ... name { ... }` by brace matching."""
    pattern = r'\bclass\b[^;{}]*?\b%s\b[^;{}]*\{' % re.escape(name)
    m = re.search(pattern, text)
    if not m:
        return None
    open_brace = m.end() - 1
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
    return None


DECL = re.compile(
    r'^virtual\s+'
    r'(?P<ret>.+?)\s*'
    r'(?P<name>\b[A-Za-z_]\w*)\s*'
    r'\((?P<args>.*)\)\s*'
    r'(?P<quals>(?:const|noexcept|\s)*)'
    r'=\s*0$'
)


def pure_virtuals(body):
    """Split the class body into statements and keep the pure virtuals.

    Done line by line rather than by one split on ';' because a declaration
    that follows a preprocessor directive would otherwise arrive with the
    directive glued to its front and be discarded silently. The conditional a
    method sits under is carried along and re-emitted around the stub, so a
    DEBUG-only method stays DEBUG-only.
    """
    found = []
    guards = []
    buffer = ''

    def flush(text, guard):
        text = ' '.join(text.split())
        text = text.lstrip('} ')
        text = re.sub(r'^(?:public|protected|private)\s*:\s*', '', text)
        if not text.startswith('virtual '):
            return
        if not re.search(r'=\s*0$', text):
            return
        if '~' in text.split('(')[0]:
            return  # pure virtual destructor: the stub inherits a real one
        m = DECL.match(text)
        if not m:
            raise SystemExit("cannot parse declaration: %s" % text)
        entry = m.groupdict()
        entry['guard'] = list(guard)
        found.append(entry)

    for line in drop_inline_bodies(body).split('\n'):
        stripped = line.strip()
        if stripped.startswith('#'):
            directive = stripped.split()[0]
            if directive in ('#if', '#ifdef', '#ifndef'):
                guards.append(stripped)
            elif directive == '#endif':
                if guards:
                    guards.pop()
            elif directive in ('#else', '#elif'):
                raise SystemExit("#else/#elif inside an interface is not handled: %s"
                                 % stripped)
            continue
        buffer += ' ' + line
        while ';' in buffer:
            head, buffer = buffer.split(';', 1)
            flush(head, guards)
    flush(buffer, guards)
    return found


def body_for(ret, label):
    """A neutral body. Value initialization covers pointers, bools, arithmetic
    types, enums and default-constructible structs alike; a reference return
    has nothing neutral to give, so it needs storage to point at."""
    ret = ret.strip()
    if ret == 'void':
        return 'XRVK_STUB("%s");' % label
    if ret.endswith('&'):
        base = ret[:-1].strip()
        if base.startswith('const '):
            base = base[len('const '):]
        return ('XRVK_STUB("%s"); static %s value{}; return value;'
                % (label, base))
    return 'XRVK_STUB("%s"); return {};' % label


def generate():
    out = []
    out.append('#pragma once')
    out.append('')
    out.append('// GENERATED by tools/generate_render_stubs.py - do not edit by hand.')
    out.append('//')
    out.append('// One stub class per interface that RendererModule::SetupEnv installs.')
    out.append('// Every method logs the first time it is reached and returns a')
    out.append('// value-initialized result, so a renderer_vk session reports exactly which')
    out.append('// parts of the engine it fails to serve, in the order the engine asks.')
    out.append('')
    out.append('#include "xrEngine/Render.h"')
    out.append('#include "Include/xrRender/DrawUtils.h"')
    out.append('#include "Include/xrRender/UIRender.h"')
    out.append('#include "Include/xrRender/RenderFactory.h"')
    out.append('#include "Include/xrRender/DebugRender.h"')
    out.append('')
    out.append('namespace xray::render::vk')
    out.append('{')
    out.append('// Logged once per call site. A Vulkan session otherwise buries the useful')
    out.append('// line under thousands of repeats of whatever the engine calls per frame.')
    out.append('#define XRVK_STUB(what)\\')
    out.append('    do {\\')
    out.append('        static bool reported = false;\\')
    out.append('        if (!reported)\\')
    out.append('        {\\')
    out.append('            reported = true;\\')
    out.append('            Msg("~ [vk stub] %s", what);\\')
    out.append('        }\\')
    out.append('    } while (false)')
    out.append('')

    total = 0
    counts = []
    for rel, cls, _slot in INTERFACES:
        path = os.path.join(ENGINE_ROOT, rel)
        text = expand_factory_macro(strip_comments(open(path, encoding='utf-8',
                                                        errors='replace').read()))
        body = class_body(text, cls)
        if body is None:
            raise SystemExit("class %s not found in %s" % (cls, rel))
        methods = pure_virtuals(body)
        total += len(methods)
        counts.append((cls, len(methods)))

        stub = "VK" + (cls[1:] if cls.startswith('I') else cls) + "Stub"
        out.append('// %s: %d pure virtual methods, from %s'
                   % (cls, len(methods), rel))
        out.append('class %s final : public %s' % (stub, cls))
        out.append('{')
        out.append('public:')
        open_guards = []
        for m in methods:
            while open_guards and open_guards != m['guard'][:len(open_guards)]:
                open_guards.pop()
                out.append('#endif')
            for guard in m['guard'][len(open_guards):]:
                out.append(guard)
                open_guards.append(guard)
            quals = ' '.join(m['quals'].split())
            quals = (' ' + quals) if quals else ''
            label = '%s::%s' % (cls, m['name'])
            out.append('    %s %s(%s)%s override { %s }'
                       % (m['ret'], m['name'], m['args'].strip(), quals,
                          body_for(m['ret'], label)))
        while open_guards:
            open_guards.pop()
            out.append('#endif')
        out.append('};')
        out.append('')

    out.append('#undef XRVK_STUB')
    out.append('} // namespace xray::render::vk')
    out.append('')

    return '\n'.join(out), counts, total


def main():
    text, counts, total = generate()
    target = os.path.join(ENGINE_ROOT, OUTPUT)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, 'w', encoding='utf-8') as handle:
        handle.write(text)
    for cls, n in counts:
        print("%-16s %3d" % (cls, n))
    print("%-16s %3d" % ("total", total))
    print("written to", OUTPUT)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
import argparse
import concurrent.futures
import multiprocessing
import os
import re
import shutil
import subprocess
import tempfile

HEADER_INCLUDE_RE = re.compile(r'#include <([^>]+)>')
HEADER_REL_DIR_RE = re.compile(
    r"(arch/flux/include/uapi/|arch/flux/include/generated/uapi/|include/uapi/|include/generated/uapi/|include/generated)(.*)"
)
COMMENT_RE = re.compile(r"(\/\*(\*(?!\/)|[^*])*\*\/)", re.S | re.M)
PREPROCESSOR_RE = re.compile(r"\s*#")


class Installer:
    def __init__(self, install_path):
        self.srctree = os.environ["srctree"]
        self.objtree = os.environ["objtree"]
        self.header_paths = [ "include/uapi/", "arch/flux/include/uapi/",
                              "arch/flux/include/generated/uapi/", "include/generated/" ]
        self.headers = set()
        self.includes = set()
        self.defines = set()
        self.structs = set()
        self.unions = set()
        self.install_path = install_path
        os.makedirs(self.install_path, exist_ok=True)
        self.stage_dir = tempfile.mkdtemp(prefix=".flux-headers.", dir=install_path)
        self.final_headers = {}
        self.abspath_cache = {}
        self.include_cache = {}
        self.content_cache = {}
        self.enum_content_cache = {}

    def relpath2abspath(self, relpath):
        if relpath not in self.abspath_cache:
            if "generated" in relpath:
                self.abspath_cache[relpath] = self.objtree + "/" + relpath
            else:
                self.abspath_cache[relpath] = self.srctree + "/" + relpath
        return self.abspath_cache[relpath]

    def read_content(self, path):
        if path not in self.content_cache:
            with open(path) as f:
                self.content_cache[path] = f.read()
        return self.content_cache[path]

    def read_enum_content(self, path):
        if path not in self.enum_content_cache:
            content = COMMENT_RE.sub(" ", self.read_content(path))
            lines = []
            for line in content.splitlines():
                if PREPROCESSOR_RE.match(line):
                    continue
                lines.append(line)
            self.enum_content_cache[path] = "\n".join(lines) + "\n"
        return self.enum_content_cache[path]

    def resolve_include(self, include):
        if include not in self.include_cache:
            resolved = None
            for prefix in self.header_paths:
                candidate = prefix + include
                if os.access(self.relpath2abspath(candidate), os.R_OK):
                    resolved = candidate
                    break
            self.include_cache[include] = resolved
        return self.include_cache[include]

    def find_headers(self, path):
        pending = [path]
        while pending:
            current = pending.pop()
            if current in self.headers:
                continue
            self.headers.add(current)
            content = self.read_content(self.relpath2abspath(current))
            for include in HEADER_INCLUDE_RE.findall(content):
                resolved = self.resolve_include(include)
                if resolved and resolved not in self.headers:
                    self.includes.add(include)
                    pending.append(resolved)

    def has_flux_prefix(self, w):
        return w.startswith("flux") or w.startswith("_flux") or \
            w.startswith("__flux")  or w.startswith("FLUX") or \
            w.startswith("_FLUX") or w.startswith("__FLUX") or \
            w.startswith("__attribute")

    def find_symbols(self, regexp, store):
        for h in self.headers:
            for l in self.read_content(h).splitlines(True):
                m = regexp.search(l)
                if not m:
                    continue
                for e in reversed(m.groups()):
                    if e:
                        if not self.has_flux_prefix(e):
                            store.add(e)
                        break

    def find_ml_symbols(self, regexp, store):
        for h in self.headers:
            for i in regexp.finditer(self.read_content(h)):
                for j in reversed(i.groups()):
                    if j:
                        if not self.has_flux_prefix(j):
                            store.add(j)
                        break

    def find_enums(self, block_regexp, symbol_regexp, store):
        for h in self.headers:
            for i in block_regexp.finditer(self.read_enum_content(h)):
                for j in reversed(i.groups()):
                    if j:
                        for k in symbol_regexp.finditer(j):
                            for l in k.groups():
                                if l:
                                    if not self.has_flux_prefix(l):
                                        store.add(l)
                                    break

    def flux_prefix(self, w):
        r = ""

        if w.startswith("__"):
            r = "__"
        elif w.startswith("_"):
            r = "_"

        if w.isupper():
            r += "FLUX"
        else:
            r += "flux"

        if not w.startswith("_"):
            r += "_"

        r += w

        return r

    def install_headers(self):
        self.find_headers("arch/flux/include/uapi/asm/syscalls.h")
        self.headers.add("arch/flux/include/uapi/asm/mpk.h")
        self.headers.add("arch/flux/include/uapi/asm/host_ops.h")
        self.headers.add("arch/flux/include/uapi/asm/flux_ops.h")
        self.headers.add("arch/flux/include/uapi/asm/flux_oci.h")
        self.headers.add("arch/flux/include/uapi/asm/spdk.h")
        self.headers.add("arch/flux/include/uapi/asm/fnet.h")
        self.find_headers("include/uapi/linux/android/binder.h")
        self.find_headers("include/uapi/linux/uhid.h")
        self.find_headers("include/uapi/linux/mman.h")
        self.find_headers("include/uapi/linux/input-event-codes.h")

        if 'FLUX_INSTALL_ADDITIONAL_HEADERS' in os.environ:
            with open(os.environ['FLUX_INSTALL_ADDITIONAL_HEADERS'], 'rU') as f:
                for line in f.readlines():
                    line = line.split('#', 1)[0].strip()
                    if line != '':
                        self.headers.add(line)

        source_headers = sorted(self.headers)
        staged_headers = set()
        install_jobs = []

        for h in source_headers:
            dir = os.path.dirname(h)
            rel_dir = HEADER_REL_DIR_RE.sub(r"kernel/\2", dir)
            out_dir = os.path.join(self.install_path, rel_dir)
            stage_dir = os.path.join(self.stage_dir, rel_dir)
            os.makedirs(out_dir, exist_ok=True)
            os.makedirs(stage_dir, exist_ok=True)
            install_jobs.append((h, out_dir, stage_dir))

        def install_one(item):
            h, out_dir, stage_dir = item
            src_header = self.relpath2abspath(h)
            stage_header = os.path.join(stage_dir, os.path.basename(h))
            final_header = os.path.join(out_dir, os.path.basename(h))
            subprocess.run(
                [os.path.join(self.srctree, "scripts/headers_install.sh"), src_header, stage_header],
                check=True,
            )
            return stage_header, final_header

        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
            for stage_header, final_header in executor.map(install_one, install_jobs):
                staged_headers.add(stage_header)
                self.final_headers[stage_header] = final_header

        self.headers = staged_headers

    def find_all_symbols(self):
        p = re.compile(r"#[ \t]*define[ \t]*(\w+)")
        self.find_symbols(p, self.defines)
        p = re.compile(r"typedef.*(\(\*(\w+)\)\(.*\)\s*|\W+(\w+)\s*|\s+(\w+)\(.*\)\s*);")
        self.find_symbols(p, self.defines)
        p = re.compile(r"typedef\s+(struct|union)\s+\w*\s*{[^\\{\}]*}\W*(\w+)\s*;", re.M|re.S)
        self.find_ml_symbols(p, self.defines)
        self.defines.add("siginfo_t")
        self.defines.add("sigevent_t")
        p = re.compile(r"struct\s+(\w+)\s*\{")
        self.find_symbols(p, self.structs)
        self.structs.add("iovec")
        self.structs.add("sched_param")
        self.structs.add("sched_attr")
        p = re.compile(r"union\s+(\w+)\s*\{")
        self.find_symbols(p, self.unions)
        p = re.compile(r"static\s+__inline__(\s+\w+)+\s+(\w+)\([^)]*\)\s")
        self.find_symbols(p, self.defines)
        p = re.compile(r"static\s+__always_inline(\s+\w+)+\s+(\w+)\([^)]*\)\s")
        self.find_symbols(p, self.defines)
        p = re.compile(r"enum\s+(\w*)\s*{([^}]*)}", re.M|re.S)
        q = re.compile(r"(\w+)\s*(,|=\s*\w+\s*\([^()]*\)|=[^,]*|$)", re.M|re.S)
        self.find_enums(p, q, self.defines)

        # needed for i386
        self.defines.add("__NR_stime")

    def render_header(self, h):
        content = self.read_content(h)
        for i in self.includes:
            search_str = r"(#[ \t]*include[ \t]*[<\"][ \t]*)" + i + r"([ \t]*[>\"])"
            replace_str = "\\1" + "kernel/" + i + "\\2"
            content = re.sub(search_str, replace_str, content)
        tmp = ""
        for w in re.split(r"(\W+)", content):
            if w in self.defines:
                w = self.flux_prefix(w)
            tmp += w
        content = tmp
        for s in self.structs:
            # XXX: cleaner way?
            if s == 'TAG':
                continue
            search_str = r"(\W?struct\s+)" + s + r"(\W)"
            replace_str = "\\1" + self.flux_prefix(s) + "\\2"
            content = re.sub(search_str, replace_str, content, flags = re.MULTILINE)
        for s in self.unions:
            search_str = r"(\W?union\s+)" + s + r"(\W)"
            replace_str = "\\1" + self.flux_prefix(s) + "\\2"
            content = re.sub(search_str, replace_str, content, flags = re.MULTILINE)
        return content

    def update_header(self, h):
        out = self.final_headers[h]
        content = self.render_header(h)
        if os.path.exists(out):
            with open(out) as f:
                if f.read() == content:
                    return
        print("  INSTALL\t%s" % out)
        with open(out, 'w') as f:
            f.write(content)

    def update_headers(self):
        p = multiprocessing.Pool(args.jobs)
        try:
            p.map_async(installer.update_header, installer.headers).wait(999999)
            p.close()
        except:
            p.terminate()
        finally:
            p.join()

    def cleanup(self):
        shutil.rmtree(self.stage_dir, ignore_errors=True)

if __name__ == '__main__':
    multiprocessing.freeze_support()
    default_jobs = 1

    parser = argparse.ArgumentParser(description='install flux headers')
    parser.add_argument('path', help='path to install to', )
    parser.add_argument('-j', '--jobs', help='number of parallel jobs', default=default_jobs, type=int)
    args = parser.parse_args()

    installer = Installer(args.path)
    try:
        installer.install_headers()
        installer.find_all_symbols()
        installer.update_headers()
    finally:
        installer.cleanup()

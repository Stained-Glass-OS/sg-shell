"""Drive a headless LibreOffice over UNO for the gates.

Always on a profile of its own (-env:UserInstallation under a scratch
directory), never the user's, and with no display: --headless.
xcd= and bundled= stand in for the installed package: a directory of .xcd
files read beside LibreOffice's own, and a directory of unpacked extensions
read as its bundled ones -- so a gate tests our defaults and add-in without
installing anything.

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import os
import shutil
import subprocess
import tempfile
import time

import uno
from com.sun.star.beans import PropertyValue


def props(**kw):
    out = []
    for k, v in kw.items():
        p = PropertyValue()
        p.Name = k
        p.Value = v
        out.append(p)
    return tuple(out)


def wine_path(path):
    """A Unix path as the Windows side sees it (Z:)."""
    return "Z:" + os.path.abspath(path).replace("/", "\\")


def wine_url(path):
    return "file:///Z:" + uno.systemPathToFileUrl(os.path.abspath(path))[len("file://"):]


class Office:
    def __init__(self, soffice="soffice", profile=None, env=None, xcd=None, oxt=None, bundled=None, wine=None,
                 seed=None, headless=True, start=None):
        """wine: the Windows soffice.exe (a Windows path) to run under $WINE
        instead of the native soffice; it is reached over a local socket.
        seed: a registrymodifications.xcu the profile starts with (what SG
        Office's launchers give a new user)."""
        self.wine = wine
        self.seed = seed
        self.headless = headless
        self.start = start        # e.g. "--calc": LibreOffice opens a document window itself
        if wine:
            return self._start_wine(wine, env)
        self.tmp = tempfile.mkdtemp(prefix="sgoffice-lo-", dir=os.environ.get("TMPDIR", "/var/tmp"))
        self.profile = profile or os.path.join(self.tmp, "profile")
        profile_url = uno.systemPathToFileUrl(self.profile)
        if seed and not os.path.exists(os.path.join(self.profile, "user", "registrymodifications.xcu")):
            os.makedirs(os.path.join(self.profile, "user"), exist_ok=True)
            shutil.copy(seed, os.path.join(self.profile, "user", "registrymodifications.xcu"))
        extra = []
        if xcd:
            # our defaults as one more machine layer, above LibreOffice's own and
            # below the user's -- where an installed package's .xcd is read
            base = "file:///usr/lib/libreoffice"
            ext = "${${BRAND_BASE_DIR}/program/lounorc:%s}/registry/" \
                  "com.sun.star.comp.deployment.configuration.PackageRegistryBackend/configmgr.ini"
            # LibreOffice's own .xcd files and ours side by side in one layer, as
            # installed: a layer of their own is read, but Calc's formula
            # options ignore values from it
            stage = os.path.join(self.tmp, "registry")
            os.makedirs(stage)
            for d in ("/usr/lib/libreoffice/share/registry", os.path.abspath(xcd)):
                for f in os.listdir(d):
                    if f.endswith(".xcd") and not os.path.exists(os.path.join(stage, f)):
                        os.symlink(os.path.join(d, f), os.path.join(stage, f))
            extra.append("-env:CONFIGURATION_LAYERS=xcsxcu:%s res:%s/share/registry "
                         "bundledext:%s sharedext:%s userext:%s user:!%s/user/registrymodifications.xcu"
                         % (uno.systemPathToFileUrl(stage), base,
                            ext % "BUNDLED_EXTENSIONS_USER", ext % "SHARED_EXTENSIONS_USER",
                            ext % "UNO_USER_PACKAGES_CACHE", profile_url))
        if bundled:
            # a directory of unpacked extensions read as LibreOffice's own bundled
            # ones, as the package installs ours (share/extensions)
            extra.append("-env:BUNDLED_EXTENSIONS=" + uno.systemPathToFileUrl(os.path.abspath(bundled)))
        self.pipe = "sgoffice%d" % os.getpid()
        e = dict(os.environ)
        e.pop("DISPLAY", None)
        e.pop("WAYLAND_DISPLAY", None)
        e["HOME"] = e.get("HOME") if e.get("HOME", "").startswith("/var/tmp") else self.tmp
        if env:
            e.update(env)
        if oxt:
            subprocess.run(["unopkg", "add", "-f", "-s", "-env:UserInstallation=" + profile_url] + extra + [oxt],
                           env=e, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
        self.proc = subprocess.Popen(
            [soffice, "--headless", "--invisible", "--nologo", "--norestore", "--nodefault",
             "-env:UserInstallation=" + profile_url] + extra + [
             "--accept=pipe,name=%s;urp;StarOffice.ComponentContext" % self.pipe],
            env=e, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        local = uno.getComponentContext()
        resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
        last = None
        for _ in range(240):
            try:
                self.ctx = resolver.resolve("uno:pipe,name=%s;urp;StarOffice.ComponentContext" % self.pipe)
                break
            except Exception as ex:  # not up yet
                last = ex
                if self.proc.poll() is not None:
                    raise RuntimeError("soffice exited with %s" % self.proc.returncode)
                time.sleep(0.25)
        else:
            raise RuntimeError("could not connect to soffice: %s" % last)
        self.smgr = self.ctx.ServiceManager
        self.desktop = self.smgr.createInstanceWithContext("com.sun.star.frame.Desktop", self.ctx)

    def _start_wine(self, soffice, env):
        import socket
        self.tmp = tempfile.mkdtemp(prefix="sgoffice-lo-", dir=os.environ.get("TMPDIR", "/var/tmp"))
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        e = dict(os.environ)
        if env:
            e.update(env)
        self.profile_url = wine_url(os.path.join(self.tmp, "profile"))
        if self.seed:
            os.makedirs(os.path.join(self.tmp, "profile", "user"))
            shutil.copy(self.seed, os.path.join(self.tmp, "profile", "user", "registrymodifications.xcu"))
        self.proc = subprocess.Popen(
            [os.environ.get("WINE", "wine"), soffice] + (["--headless", "--invisible"] if self.headless else []) +
            ([self.start] if self.start else ["--nodefault"]) + ["--nologo", "--norestore",
             "-env:UserInstallation=" + self.profile_url,
             "--accept=socket,host=127.0.0.1,port=%d;urp;StarOffice.ComponentContext" % port],
            env=e, stdout=subprocess.DEVNULL, stderr=open(os.path.join(self.tmp, "wine.log"), "w"))
        local = uno.getComponentContext()
        resolver = local.ServiceManager.createInstanceWithContext("com.sun.star.bridge.UnoUrlResolver", local)
        last = None
        for _ in range(1200):
            try:
                self.ctx = resolver.resolve("uno:socket,host=127.0.0.1,port=%d;urp;StarOffice.ComponentContext" % port)
                break
            except Exception as ex:
                last = ex
                if self.proc.poll() is not None:
                    raise RuntimeError("soffice.exe exited with %s" % self.proc.returncode)
                time.sleep(0.25)
        else:
            raise RuntimeError("could not connect to soffice.exe: %s" % last)
        self.smgr = self.ctx.ServiceManager
        self.desktop = self.smgr.createInstanceWithContext("com.sun.star.frame.Desktop", self.ctx)

    def current(self, service, timeout=120):
        """The document LibreOffice opened at its start (start=...)."""
        for _ in range(timeout * 4):
            # every open document, not only the active one: with no window
            # manager, no window is active
            e = self.desktop.getComponents().createEnumeration()
            while e.hasMoreElements():
                c = e.nextElement()
                if c is not None and hasattr(c, "supportsService") and c.supportsService(service):
                    return c
            time.sleep(0.25)
        raise RuntimeError("no %s window" % service)

    def url(self, path):
        return wine_url(path) if self.wine else uno.systemPathToFileUrl(os.path.abspath(path))

    def load(self, path, **kw):
        return self.desktop.loadComponentFromURL(self.url(path), "_blank", 0, props(Hidden=True, **kw))

    def new(self, kind, hidden=True):
        return self.desktop.loadComponentFromURL("private:factory/" + kind, "_blank", 0, props(Hidden=hidden))

    def store(self, doc, path, filt):
        doc.storeToURL(self.url(path), props(FilterName=filt, Overwrite=True))

    def config(self, nodepath):
        cp = self.smgr.createInstanceWithContext("com.sun.star.configuration.ConfigurationProvider", self.ctx)
        return cp.createInstanceWithArguments("com.sun.star.configuration.ConfigurationAccess",
                                              props(nodepath=nodepath))

    def close(self):
        try:
            self.desktop.terminate()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        log = os.path.join(self.tmp, "wine.log")
        if os.path.exists(log):     # kept for a look after a failure
            shutil.copy(log, os.path.join(os.environ.get("TMPDIR", "/var/tmp"), "sgoffice-last-wine.log"))
        shutil.rmtree(self.tmp, ignore_errors=True)

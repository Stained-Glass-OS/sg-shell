"""SG Office Functions -- the UNO side of the Calc add-in: one method a
function of sgoffice_spec.py, each calling its logic in sgoffice_functions.py
(both in the extension's pythonpath/).

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import unohelper
from com.sun.star.lang import XServiceInfo
from org.stainedglass.office import XSgFunctions

import sgoffice_functions as logic
from sgoffice_spec import FUNCTIONS, IMPL

SERVICES = (IMPL, "com.sun.star.sheet.AddIn")


def _method(name, returns):
    fn = getattr(logic, name)
    array = returns == "array"

    def call(self, *args):
        out = fn(*args, array=array)
        if array and not isinstance(out, tuple):
            out = ((out,),)
        return out
    call.__name__ = name
    return call


class Functions(unohelper.Base, XSgFunctions, XServiceInfo):
    def __init__(self, ctx):
        self.ctx = ctx

    def getImplementationName(self):
        return IMPL

    def supportsService(self, name):
        return name in SERVICES

    def getSupportedServiceNames(self):
        return SERVICES


for _m, _n, _x, _c, _d, _p, _r in FUNCTIONS:
    setattr(Functions, _m, _method(_m, _r))

g_ImplementationHelper = unohelper.ImplementationHelper()
g_ImplementationHelper.addImplementation(Functions, IMPL, SERVICES)

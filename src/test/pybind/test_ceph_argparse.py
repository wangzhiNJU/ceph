#!/usr/bin/nosetests --nocapture
# -*- mode:python; tab-width:4; indent-tabs-mode:t -*-
# vim: ts=4 sw=4 smarttab expandtab
#
# Ceph - scalable distributed file system
#
# Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
#
# Author: Loic Dachary <loic@dachary.org>
#
#  This library is free software; you can redistribute it and/or
#  modify it under the terms of the GNU Lesser General Public
#  License as published by the Free Software Foundation; either
#  version 2.1 of the License, or (at your option) any later version.
#

from nose.tools import eq_ as eq
from nose.tools import *

from ceph_argparse import validate_command, parse_json_funcsigs

import os
import re
import json

def get_command_descriptions(what):
    buffer = os.popen("./get_command_descriptions " + "--" + what
					  + " 2>&1 | grep cmd000").read()
    return re.sub(r'^.*?(\{.*\})', '\g<1>', buffer)

def test_parse_json_funcsigs():
    commands = get_command_descriptions("all")
    cmd_json = parse_json_funcsigs(commands, 'cli')

    # syntax error https://github.com/ceph/ceph/pull/585
    commands = get_command_descriptions("pull585")
    assert_raises(TypeError, parse_json_funcsigs, commands, 'cli')

# Local Variables:
# compile-command: "cd ../.. ; make -j4 && 
#  PYTHONPATH=pybind nosetests --stop \
#  test/pybind/test_ceph_argparse.py # test_ceph_argparse.py:TestOSD.test_rm"
# End:

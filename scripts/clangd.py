#!/usr/bin/env python3
"""Start native clangd with the active machine's persistent compiler context."""
import os
import sys

arguments = []
for argument in sys.argv[1:]:
    if argument.startswith('--mapple-state-dir='):
        os.environ['MAPPLE_LAB_DIR'] = argument.split('=', 1)[1]
    else:
        arguments.append(argument)

from mapple_dev import navigation

try:
    navigation.launch(arguments)
except (RuntimeError, OSError, ValueError) as error:
    print('Mapple navigation: ' + str(error), file=sys.stderr)
    sys.exit(1)

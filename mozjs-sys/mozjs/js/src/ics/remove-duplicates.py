#!/usr/bin/env python3

import glob
import os
import hashlib

have_hashes = set()

for file in glob.glob('IC-*'):
    with open(file, 'rb') as f:
        content = f.read()
        h = hashlib.new('sha256')
        h.update(content)
        digest = h.hexdigest()
        if digest in have_hashes:
            print("Removing: %s" % file)
            os.unlink(file)
        have_hashes.add(digest)

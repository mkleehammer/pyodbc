#!/bin/bash

# Run this from the root.

# Delete old builds to make it easy to upload "dist/*" with twine.
rm dist/*

# Make the source build.
python setup.py sdist

# Make a wheel build for each virtual environment we find.

for d in venv*/; do
    source ${d}bin/activate
    python setup.py clean -a bdist_wheel
    deactivate
done

ls -l dist

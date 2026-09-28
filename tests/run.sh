#!/bin/sh
# Lance les tests automatiques (calculs + interface).
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -O1 tests/test_core.cpp src/core.cpp -o tests/test_core -lm
./tests/test_core
node --check src/ui/app.js && echo "app.js : syntaxe OK"
python3 src/ui/bundle.py /tmp/lm-bundle.html && echo "interface assemblee : OK"
